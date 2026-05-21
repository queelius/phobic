#include "_phobic.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>   /* sysconf */

#ifndef _WIN32
#include <pthread.h>
#define PHOBIC_HAVE_PTHREAD 1
#else
#define PHOBIC_HAVE_PTHREAD 0
#endif

#ifdef PHOBIC_PROFILE
#include <time.h>
#include <stdio.h>
static inline double pp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#define PP_REPORT(label, t0) \
    fprintf(stderr, "[phobic-profile] %-40s %7.1f ms\n", \
            (label), (pp_now() - (t0)) * 1000.0)
#endif

/* ── little-endian load helpers ──────────────────────────────────────── */

static inline uint64_t load_u64_le(const uint8_t *p) {
    return (uint64_t)p[0]         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static inline uint64_t load_u64_le_partial(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* ── wyhash-style mixing ─────────────────────────────────────────────── */

static inline uint64_t wymix(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)(r >> 64) ^ (uint64_t)r;
}

static uint64_t phobic_hash(const char *key, size_t len, uint64_t seed) {
    uint64_t h = seed;
    const uint8_t *p = (const uint8_t *)key;
    while (len >= 8) {
        h = wymix(h ^ load_u64_le(p), 0x9e3779b97f4a7c15ULL);
        p += 8; len -= 8;
    }
    if (len > 0) h = wymix(h ^ load_u64_le_partial(p, len), 0x517cc1b727220a95ULL);
    return wymix(h, 0x94d049bb133111ebULL);
}

/* ── splitmix64 (used for deterministic seed derivation) ─────────────── */

static inline uint64_t splitmix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* ── bucket / slot helpers ───────────────────────────────────────────── */

static inline size_t bucket_for(uint64_t h1, size_t num_buckets) {
    return (size_t)(h1 % num_buckets);
}

static inline size_t slot_with_pilot(uint64_t h2, uint16_t pilot, size_t range_size) {
    uint64_t mixed = h2 ^ ((uint64_t)pilot * 0x9e3779b97f4a7c15ULL);
    mixed = wymix(mixed, 0xbf58476d1ce4e5b9ULL);
    return (size_t)(mixed % range_size);
}

/* ── dual hash ───────────────────────────────────────────────────────── */

typedef struct { uint64_t h1, h2; } dual_hash;

static dual_hash hash_key(const char *key, size_t len, uint64_t seed) {
    return (dual_hash){
        phobic_hash(key, len, seed),
        phobic_hash(key, len, seed ^ 0x517cc1b727220a95ULL)
    };
}

/* ── shard membership ────────────────────────────────────────────────── */

/* Independent hash from the per-shard pilot search hash. Lets a shard
 * re-run pilot search with new seeds without altering the partition. */
static inline size_t shard_for_key(const char *key, size_t key_len,
                                    uint64_t shard_seed, size_t num_shards) {
    return phobic_hash(key, key_len, shard_seed) % num_shards;
}

/* ── lifetime / stats ────────────────────────────────────────────────── */

void phobic_free(phobic_phf *phf) {
    if (!phf) return;
    if (phf->shard_pilots) {
        for (size_t s = 0; s < phf->num_shards; s++) free(phf->shard_pilots[s]);
        free(phf->shard_pilots);
    }
    free(phf->shard_seeds);
    free(phf->shard_range);
    free(phf->shard_offsets);
    free(phf->shard_num_buckets);
    free(phf->shard_bucket_size);
    free(phf);
}

/* Total wire-format size in bytes (matches phobic_serialize layout).
 * 56 byte global header + 40 byte per shard descriptor + pilot blocks
 * with 8-byte alignment between them. */
static size_t serialized_size(const phobic_phf *phf) {
    size_t off = 56 + 40 * phf->num_shards;
    /* Round to 8-byte boundary before first pilot block */
    off = (off + 7) & ~(size_t)7;
    for (size_t s = 0; s < phf->num_shards; s++) {
        off += phf->shard_num_buckets[s] * sizeof(uint16_t);
        if (s + 1 < phf->num_shards)
            off = (off + 7) & ~(size_t)7;
    }
    return off;
}

double phobic_bits_per_key(const phobic_phf *phf) {
    if (!phf || phf->num_keys == 0) return 0.0;
    return (double)(serialized_size(phf) * 8) / (double)phf->num_keys;
}

/* ── bitset helpers ──────────────────────────────────────────────────── */

static inline size_t bitset_words(size_t nbits) {
    return (nbits + 63) / 64;
}

static inline int bitset_test(const uint64_t *bs, size_t idx) {
    return (int)((bs[idx / 64] >> (idx % 64)) & 1);
}

static inline void bitset_set(uint64_t *bs, size_t idx) {
    bs[idx / 64] |= (uint64_t)1 << (idx % 64);
}

/* ── bucket layout ───────────────────────────────────────────────────── */

typedef struct { size_t count; size_t index; } bucket_entry;

static int bucket_entry_cmp_desc(const void *a, const void *b) {
    size_t ca = ((const bucket_entry *)a)->count;
    size_t cb = ((const bucket_entry *)b)->count;
    return (ca < cb) - (ca > cb);
}

typedef struct {
    size_t *counts;
    size_t *prefix;
    size_t *members;
    size_t *order;
    size_t  num_buckets;
    size_t  max_bucket;
} bucket_layout;

static void bucket_layout_free(bucket_layout *bl) {
    free(bl->counts);
    free(bl->prefix);
    free(bl->members);
    free(bl->order);
}

static int bucket_layout_init(bucket_layout *bl, const dual_hash *hashes,
                              size_t num_keys, size_t num_buckets) {
    bl->num_buckets = num_buckets;
    bl->counts  = NULL;
    bl->prefix  = NULL;
    bl->members = NULL;
    bl->order   = NULL;

    bl->counts = calloc(num_buckets, sizeof(size_t));
    if (!bl->counts) goto fail;

    for (size_t i = 0; i < num_keys; i++)
        bl->counts[bucket_for(hashes[i].h1, num_buckets)]++;

    bl->prefix = malloc((num_buckets + 1) * sizeof(size_t));
    if (!bl->prefix) goto fail;
    bl->prefix[0] = 0;
    for (size_t b = 0; b < num_buckets; b++)
        bl->prefix[b + 1] = bl->prefix[b] + bl->counts[b];

    bl->members = num_keys ? malloc(num_keys * sizeof(size_t)) : NULL;
    size_t *write_pos = malloc(num_buckets * sizeof(size_t));
    if ((num_keys && !bl->members) || !write_pos) { free(write_pos); goto fail; }
    memcpy(write_pos, bl->prefix, num_buckets * sizeof(size_t));
    for (size_t i = 0; i < num_keys; i++) {
        size_t b = bucket_for(hashes[i].h1, num_buckets);
        bl->members[write_pos[b]++] = i;
    }
    free(write_pos);

    bucket_entry *entries = malloc(num_buckets * sizeof(bucket_entry));
    bl->order = malloc(num_buckets * sizeof(size_t));
    if (!entries || !bl->order) { free(entries); goto fail; }
    for (size_t b = 0; b < num_buckets; b++)
        entries[b] = (bucket_entry){ bl->counts[b], b };
    qsort(entries, num_buckets, sizeof(bucket_entry), bucket_entry_cmp_desc);
    for (size_t b = 0; b < num_buckets; b++) bl->order[b] = entries[b].index;
    free(entries);

    bl->max_bucket = num_buckets ? bl->counts[bl->order[0]] : 0;
    return 0;

fail:
    bucket_layout_free(bl);
    return -1;
}

/* ── per-shard pilot search (always strict) ──────────────────────────── */

/* Returns calloc'd pilots array on success, NULL on failure.
 * On failure, *out_collisions is set to the count of keys in the first
 * unsolvable bucket (a diagnostic proxy for "how unsolvable was this attempt"). */
static uint16_t *try_build_shard(const dual_hash *hashes, size_t num_keys,
                                  size_t range_size, size_t num_buckets,
                                  size_t *out_collisions_on_fail) {
    bucket_layout bl;
    if (bucket_layout_init(&bl, hashes, num_keys, num_buckets) < 0)
        return NULL;

    uint16_t *pilots = calloc(num_buckets, sizeof(uint16_t));
    size_t bsw = bitset_words(range_size);
    uint64_t *occupied = calloc(bsw, sizeof(uint64_t));
    size_t *candidates = bl.max_bucket > 0
        ? malloc(bl.max_bucket * sizeof(size_t)) : NULL;
    if (!pilots || !occupied || (bl.max_bucket > 0 && !candidates)) {
        free(pilots); free(occupied); free(candidates);
        bucket_layout_free(&bl);
        return NULL;
    }

    int failed = 0;
    size_t failed_bsize = 0;
    for (size_t oi = 0; oi < num_buckets; oi++) {
        size_t b = bl.order[oi];
        size_t bsize = bl.counts[b];
        if (bsize == 0) { pilots[b] = 0; continue; }

        size_t bstart = bl.prefix[b];
        int found = 0;

        for (uint32_t pilot = 0; pilot < 65536 && !found; pilot++) {
            int collision = 0;
            for (size_t k = 0; k < bsize && !collision; k++) {
                size_t ki = bl.members[bstart + k];
                size_t slot = slot_with_pilot(hashes[ki].h2, (uint16_t)pilot, range_size);
                if (bitset_test(occupied, slot)) { collision = 1; break; }
                for (size_t prev = 0; prev < k; prev++) {
                    if (candidates[prev] == slot) { collision = 1; break; }
                }
                candidates[k] = slot;
            }
            if (!collision) {
                for (size_t k = 0; k < bsize; k++)
                    bitset_set(occupied, candidates[k]);
                pilots[b] = (uint16_t)pilot;
                found = 1;
            }
        }
        if (!found) {
            failed = 1;
            failed_bsize = bsize;
            break;
        }
    }

    free(occupied);
    free(candidates);
    bucket_layout_free(&bl);

    if (failed) {
        if (out_collisions_on_fail) *out_collisions_on_fail = failed_bsize;
        free(pilots);
        return NULL;
    }
    return pilots;
}

/* ── shard build orchestrator (single-threaded; threading in 3b) ─────── */

typedef struct {
    /* outputs on success */
    uint16_t *pilots;
    size_t    num_buckets;
    size_t    range;
    size_t    bucket_size;
    uint64_t  seed;        /* the actual seed that worked */
    /* outputs on failure */
    size_t    best_collisions;
    double    resolved_load_factor;
} shard_build_result;

static int build_one_shard(
    const char **shard_keys, const size_t *shard_key_lens, size_t shard_n,
    double load_factor, uint64_t base_seed,
    int max_retries, size_t bucket_size_param,
    shard_build_result *r
) {
    /* Resolve bucket_size for this shard. */
    size_t bucket_size;
    if (bucket_size_param > 0) {
        bucket_size = bucket_size_param;
    } else if (shard_n < 2) {
        bucket_size = 1;
    } else {
        bucket_size = (size_t)ceil(log2((double)shard_n));
        if (bucket_size < 1) bucket_size = 1;
    }
    size_t num_buckets = (shard_n + bucket_size - 1) / bucket_size;
    if (num_buckets == 0) num_buckets = 1;  /* shouldn't happen, defensive */

    static const int BUMP_INTERVAL = 10;
    static const double LF_BUMP = 0.005;

    size_t best_coll = SIZE_MAX;
    double cur_lf = load_factor;

    for (int attempt = 0; attempt < max_retries; attempt++) {
        uint64_t cur_seed = splitmix64(base_seed
            ^ ((uint64_t)attempt * 0xD2B74407B1CE6E93ULL));
        cur_lf = load_factor - LF_BUMP * (double)(attempt / BUMP_INTERVAL);
        if (cur_lf <= 0.01) cur_lf = 0.01;

        size_t range = (size_t)ceil((double)shard_n / cur_lf);
        /* MPHF mode (load_factor=1.0) allows range == shard_n exactly.
         * Anything below that is impossible by pigeonhole. */
        if (range < shard_n) range = shard_n;

        /* Hash all this shard's keys with cur_seed */
        dual_hash *hashes = malloc(shard_n * sizeof(dual_hash));
        if (!hashes) return -1;
        for (size_t i = 0; i < shard_n; i++)
            hashes[i] = hash_key(shard_keys[i], shard_key_lens[i], cur_seed);

        size_t coll_on_fail = 0;
        uint16_t *pilots = try_build_shard(hashes, shard_n, range, num_buckets,
                                            &coll_on_fail);
        free(hashes);

        if (pilots) {
            r->pilots                = pilots;
            r->num_buckets           = num_buckets;
            r->range                 = range;
            r->bucket_size           = bucket_size;
            r->seed                  = cur_seed;
            r->resolved_load_factor  = cur_lf;
            return 0;
        }
        if (coll_on_fail < best_coll) best_coll = coll_on_fail;
    }

    r->best_collisions       = best_coll;
    r->resolved_load_factor  = cur_lf;
    r->bucket_size           = bucket_size;
    return -1;
}

/* ── pthread build pool ──────────────────────────────────────────────── */

#if PHOBIC_HAVE_PTHREAD
typedef struct {
    /* shared inputs (read-only) */
    const char ***shard_keys_arr;   /* shard_keys_arr[s] = key ptrs for shard s */
    size_t       **shard_lens_arr;
    const size_t  *shard_counts;
    const uint64_t *base_seeds;     /* per-shard base seeds (from splitmix64) */
    double         load_factor;
    int            max_retries;
    size_t         bucket_size_param;

    /* outputs (each worker writes to its own index, no contention) */
    shard_build_result *results;
    int                *result_codes;  /* 0 = success, -1 = failure */

    /* atomic work counter */
    pthread_mutex_t mu;
    size_t          next_shard;
    size_t          total_shards;
} build_pool;

static void *build_worker(void *arg) {
    build_pool *p = (build_pool *)arg;
    for (;;) {
        size_t s;
        pthread_mutex_lock(&p->mu);
        s = p->next_shard++;
        pthread_mutex_unlock(&p->mu);
        if (s >= p->total_shards) return NULL;

        if (p->shard_counts[s] == 0) {
            p->result_codes[s] = 0;
            continue;
        }
        int rc = build_one_shard(
            p->shard_keys_arr[s], p->shard_lens_arr[s], p->shard_counts[s],
            p->load_factor, p->base_seeds[s],
            p->max_retries, p->bucket_size_param,
            &p->results[s]);
        p->result_codes[s] = rc;
    }
}
#endif /* PHOBIC_HAVE_PTHREAD */

/* ── auto num_shards heuristic ───────────────────────────────────────── */

static size_t auto_num_shards(size_t num_keys, int num_threads) {
    /* num_shards is a function of num_keys ALONE, not of available
     * parallelism. This is load-bearing for cross-machine reproducibility:
     * the same `phobic.build(keys, seed=42)` call must produce
     * byte-identical output regardless of CPU count.
     *
     * It also keeps very-large-N builds tractable. With a thread-based
     * cap, a single-threaded build at N=10M would be forced into 4 shards
     * of 2.5M keys each, giving bucket_size = ceil(log2(2.5M)) = 22, which
     * exhausts the uint16 pilot space in any realistic time. Without the
     * cap, the same build gets ~625 shards of 16K keys each, bucket_size=14,
     * always solvable.
     *
     * Threads orchestrate work; they do not redefine the work. */
    (void)num_threads;

    if (num_keys < 32768) return 1;

    /* Aim at ~16K keys per shard: bucket_size = ceil(log2(16K)) = 14
     * stays in the empirical "always solvable" regime, and per-shard
     * descriptor overhead (40 bytes) amortises over 16K keys. */
    return (num_keys + 15999) / 16000;
}

/* ── top-level build with diag ───────────────────────────────────────── */

phobic_phf *phobic_build_with_diag(const char **keys,
                                    const size_t *key_lens,
                                    size_t num_keys,
                                    const phobic_build_opts *opts,
                                    phobic_build_diag *diag) {
    if (diag) {
        diag->failed_shard          = -1;
        diag->best_collisions       = 0;
        diag->resolved_load_factor  = 0.0;
        diag->resolved_bucket_size  = 0;
    }
    if (num_keys == 0 || !opts) return NULL;
    if (!(opts->load_factor > 0.0 && opts->load_factor <= 1.0)) return NULL;

    int max_retries = opts->max_retries > 0 ? opts->max_retries : 100;

    /* Resolve the worker-thread count. auto_num_shards takes num_threads
     * but ignores it by design; only the cap below actually needs it. */
    int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count < 1) cpu_count = 1;
    int num_threads = opts->num_threads > 0 ? opts->num_threads : cpu_count;

    size_t num_shards = opts->num_shards > 0
        ? opts->num_shards
        : auto_num_shards(num_keys, num_threads);
    if (num_shards == 0) num_shards = 1;

    /* Cap threads to num_shards (no benefit from more workers than shards). */
    if ((size_t)num_threads > num_shards) num_threads = (int)num_shards;
    if (num_threads < 1) num_threads = 1;

    /* Derive shard_seed and per-shard seeds via splitmix64. */
    uint64_t shard_seed = splitmix64(opts->seed ^ 0xA5A5A5A5A5A5A5A5ULL);

    /* Allocate the phobic_phf struct and per-shard arrays up front so
     * we can free cleanly on any failure path. */
    phobic_phf *phf = calloc(1, sizeof(phobic_phf));
    if (!phf) return NULL;

    phf->num_keys           = num_keys;
    phf->num_shards         = num_shards;
    phf->seed               = opts->seed;
    phf->shard_seed         = shard_seed;
    phf->shard_seeds        = calloc(num_shards, sizeof(uint64_t));
    phf->shard_range        = calloc(num_shards, sizeof(size_t));
    phf->shard_offsets      = calloc(num_shards, sizeof(size_t));
    phf->shard_num_buckets  = calloc(num_shards, sizeof(size_t));
    phf->shard_bucket_size  = calloc(num_shards, sizeof(size_t));
    phf->shard_pilots       = calloc(num_shards, sizeof(uint16_t *));
    if (!phf->shard_seeds || !phf->shard_range || !phf->shard_offsets
        || !phf->shard_num_buckets || !phf->shard_bucket_size
        || !phf->shard_pilots) {
        phobic_free(phf);
        return NULL;
    }

    for (size_t s = 0; s < num_shards; s++)
        phf->shard_seeds[s] = splitmix64(opts->seed
            ^ ((uint64_t)s * 0xC2B2AE3D27D4EB4FULL)
            ^ 0x4F1BBCDCBFA53E0BULL);

    /* Distribute keys to shards.
     * Single-pass: compute shard for each key, build per-shard pointer arrays. */
    size_t *shard_counts = calloc(num_shards, sizeof(size_t));
    if (!shard_counts) { phobic_free(phf); return NULL; }

#ifdef PHOBIC_PROFILE
    double pp_dist_t0 = pp_now();
#endif
    /* If num_shards == 1, every key is in shard 0; skip the hash computation. */
    if (num_shards == 1) {
        shard_counts[0] = num_keys;
    } else {
        for (size_t i = 0; i < num_keys; i++) {
            size_t s = shard_for_key(keys[i], key_lens[i], shard_seed, num_shards);
            shard_counts[s]++;
        }
    }
#ifdef PHOBIC_PROFILE
    PP_REPORT("shard distribution count pass", pp_dist_t0);
#endif

    /* Allocate per-shard pointer/len arrays. */
    const char ***shard_keys = calloc(num_shards, sizeof(const char **));
    size_t **shard_lens = calloc(num_shards, sizeof(size_t *));
    if (!shard_keys || !shard_lens) {
        free(shard_counts); free(shard_keys); free(shard_lens);
        phobic_free(phf);
        return NULL;
    }
    for (size_t s = 0; s < num_shards; s++) {
        if (shard_counts[s] == 0) continue;
        shard_keys[s] = malloc(shard_counts[s] * sizeof(const char *));
        shard_lens[s] = malloc(shard_counts[s] * sizeof(size_t));
        if (!shard_keys[s] || !shard_lens[s]) {
            for (size_t t = 0; t <= s; t++) {
                free(shard_keys[t]);
                free(shard_lens[t]);
            }
            free(shard_keys); free(shard_lens); free(shard_counts);
            phobic_free(phf);
            return NULL;
        }
    }

    /* Scatter pointers/lens into per-shard arrays. */
    size_t *write_pos = calloc(num_shards, sizeof(size_t));
    if (!write_pos) {
        for (size_t s = 0; s < num_shards; s++) {
            free(shard_keys[s]); free(shard_lens[s]);
        }
        free(shard_keys); free(shard_lens); free(shard_counts);
        phobic_free(phf);
        return NULL;
    }
#ifdef PHOBIC_PROFILE
    double pp_scatter_t0 = pp_now();
#endif
    if (num_shards == 1) {
        for (size_t i = 0; i < num_keys; i++) {
            shard_keys[0][i] = keys[i];
            shard_lens[0][i] = key_lens[i];
        }
    } else {
        for (size_t i = 0; i < num_keys; i++) {
            size_t s = shard_for_key(keys[i], key_lens[i], shard_seed, num_shards);
            shard_keys[s][write_pos[s]] = keys[i];
            shard_lens[s][write_pos[s]] = key_lens[i];
            write_pos[s]++;
        }
    }
    free(write_pos);
#ifdef PHOBIC_PROFILE
    PP_REPORT("shard scatter pass", pp_scatter_t0);
    double pp_build_t0 = pp_now();
#endif

    /* Build each shard. Threaded when num_threads > 1 and pthread is available;
     * serial fallback otherwise. Both paths populate per-shard `results` and
     * `result_codes`; failures are checked after the join. */
    shard_build_result *results = calloc(num_shards, sizeof(shard_build_result));
    int *result_codes = malloc(num_shards * sizeof(int));
    if (!results || !result_codes) {
        free(results); free(result_codes);
        for (size_t s = 0; s < num_shards; s++) {
            free(shard_keys[s]); free(shard_lens[s]);
        }
        free(shard_keys); free(shard_lens); free(shard_counts);
        phobic_free(phf);
        return NULL;
    }
    /* Sentinel: "build never started" looks the same as failure for accounting. */
    for (size_t s = 0; s < num_shards; s++) result_codes[s] = -1;

#if PHOBIC_HAVE_PTHREAD
    if (num_threads > 1) {
        build_pool pool = {
            .shard_keys_arr   = shard_keys,
            .shard_lens_arr   = shard_lens,
            .shard_counts     = shard_counts,
            .base_seeds       = phf->shard_seeds,
            .load_factor      = opts->load_factor,
            .max_retries      = max_retries,
            .bucket_size_param = opts->bucket_size,
            .results          = results,
            .result_codes     = result_codes,
            .next_shard       = 0,
            .total_shards     = num_shards,
        };
        pthread_mutex_init(&pool.mu, NULL);

        pthread_t *threads = malloc((size_t)num_threads * sizeof(pthread_t));
        if (!threads) {
            pthread_mutex_destroy(&pool.mu);
            free(results); free(result_codes);
            for (size_t s = 0; s < num_shards; s++) {
                free(shard_keys[s]); free(shard_lens[s]);
            }
            free(shard_keys); free(shard_lens); free(shard_counts);
            phobic_free(phf);
            return NULL;
        }
        int spawned = 0;
        for (int t = 0; t < num_threads; t++) {
            if (pthread_create(&threads[t], NULL, build_worker, &pool) == 0) {
                spawned++;
            } else {
                /* If a spawn fails, drain the queue from this thread. */
                build_worker(&pool);
                break;
            }
        }
        for (int t = 0; t < spawned; t++) pthread_join(threads[t], NULL);
        free(threads);
        pthread_mutex_destroy(&pool.mu);
    } else
#endif
    {
        /* Serial fallback (or single-threaded mode). */
        for (size_t s = 0; s < num_shards; s++) {
            if (shard_counts[s] == 0) {
                result_codes[s] = 0;
                continue;
            }
            result_codes[s] = build_one_shard(
                shard_keys[s], shard_lens[s], shard_counts[s],
                opts->load_factor, phf->shard_seeds[s],
                max_retries, opts->bucket_size,
                &results[s]);
        }
    }

#ifdef PHOBIC_PROFILE
    PP_REPORT("per-shard build (parallel section)", pp_build_t0);
#endif
    /* Find the first failed shard, if any. */
    int build_failed = 0;
    int failed_shard = -1;
    shard_build_result fail_info = {0};
    for (size_t s = 0; s < num_shards; s++) {
        if (result_codes[s] != 0) {
            build_failed = 1;
            failed_shard = (int)s;
            fail_info = results[s];
            break;
        }
    }

    /* Transfer successful per-shard results into phf. On failure, free the
     * pilots arrays of any shards that did succeed. */
    if (!build_failed) {
        for (size_t s = 0; s < num_shards; s++) {
            if (shard_counts[s] == 0) {
                phf->shard_pilots[s]      = NULL;
                phf->shard_num_buckets[s] = 0;
                phf->shard_range[s]       = 0;
                phf->shard_bucket_size[s] = 0;
                continue;
            }
            phf->shard_pilots[s]      = results[s].pilots;
            phf->shard_num_buckets[s] = results[s].num_buckets;
            phf->shard_range[s]       = results[s].range;
            phf->shard_bucket_size[s] = results[s].bucket_size;
            phf->shard_seeds[s]       = results[s].seed;
        }
    } else {
        for (size_t s = 0; s < num_shards; s++) {
            if (result_codes[s] == 0 && shard_counts[s] > 0)
                free(results[s].pilots);
        }
    }

    free(results);
    free(result_codes);

    /* Free per-shard scratch buffers regardless of outcome. */
    for (size_t s = 0; s < num_shards; s++) {
        free(shard_keys[s]);
        free(shard_lens[s]);
    }
    free(shard_keys); free(shard_lens); free(shard_counts);

    if (build_failed) {
        if (diag) {
            diag->failed_shard         = failed_shard;
            diag->best_collisions      = fail_info.best_collisions;
            diag->resolved_load_factor = fail_info.resolved_load_factor;
            diag->resolved_bucket_size = fail_info.bucket_size;
        }
        phobic_free(phf);
        return NULL;
    }

    /* Compute prefix-sum offsets and total range. */
    size_t off = 0;
    for (size_t s = 0; s < num_shards; s++) {
        phf->shard_offsets[s] = off;
        off += phf->shard_range[s];
    }
    phf->total_range = off;

    return phf;
}

phobic_phf *phobic_build(const char **keys,
                          const size_t *key_lens,
                          size_t num_keys,
                          const phobic_build_opts *opts) {
    return phobic_build_with_diag(keys, key_lens, num_keys, opts, NULL);
}

/* ── query ───────────────────────────────────────────────────────────── */

size_t phobic_query(const phobic_phf *phf, const char *key, size_t key_len) {
    size_t s = phf->num_shards == 1
        ? 0
        : shard_for_key(key, key_len, phf->shard_seed, phf->num_shards);
    /* Empty shard: num_buckets and range are 0 and pilots is NULL, so the
     * hashing below would divide by zero / deref NULL. Only reachable for a
     * key not in the build set (built keys never hash to an empty shard)
     * when num_shards exceeds the key spread. The contract for an out-of-set
     * key is "some valid slot"; 0 is always valid. */
    if (phf->shard_num_buckets[s] == 0)
        return 0;
    dual_hash dh = hash_key(key, key_len, phf->shard_seeds[s]);
    size_t b = bucket_for(dh.h1, phf->shard_num_buckets[s]);
    size_t local = slot_with_pilot(dh.h2, phf->shard_pilots[s][b], phf->shard_range[s]);
    return phf->shard_offsets[s] + local;
}

/* ── pthread batch query ─────────────────────────────────────────────── */

#if PHOBIC_HAVE_PTHREAD
typedef struct {
    const phobic_phf *phf;
    const char **keys;
    const size_t *key_lens;
    size_t *out_slots;
    size_t start, end;
} batch_chunk;

static void *batch_worker(void *arg) {
    batch_chunk *c = (batch_chunk *)arg;
    for (size_t i = c->start; i < c->end; i++)
        c->out_slots[i] = phobic_query(c->phf, c->keys[i], c->key_lens[i]);
    return NULL;
}
#endif

/* Threading threshold: pthread_create + join overhead (~10-50 us)
 * starts to amortise around N keys * per-key cost. At ~500 ns per query
 * this is ~1024 keys; below that, serial is faster. Phase 5 may tune. */
#define PHOBIC_BATCH_THREADING_THRESHOLD ((size_t)1024)

void phobic_query_batch(const phobic_phf *phf,
                         const char **keys,
                         const size_t *key_lens,
                         size_t n,
                         size_t *out_slots,
                         int num_threads) {
    /* num_threads <= 0 means "auto": resolve to the online CPU count, the
     * same convention phobic_build_with_diag uses. This makes PHF.lookup()
     * parallelise by default instead of silently staying single-threaded. */
    if (num_threads <= 0) {
        long cpu = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = cpu > 1 ? (int)cpu : 1;
    }
#if PHOBIC_HAVE_PTHREAD
    if (num_threads > 1 && n >= PHOBIC_BATCH_THREADING_THRESHOLD) {
        /* Cap threads to n so we never have idle workers. */
        if ((size_t)num_threads > n) num_threads = (int)n;

        pthread_t *threads = malloc((size_t)num_threads * sizeof(pthread_t));
        batch_chunk *chunks = malloc((size_t)num_threads * sizeof(batch_chunk));
        if (!threads || !chunks) {
            /* Fall back to serial on alloc failure. */
            free(threads); free(chunks);
            goto serial;
        }

        size_t per = n / (size_t)num_threads;
        size_t rem = n % (size_t)num_threads;
        size_t cursor = 0;
        int spawned = 0;
        for (int t = 0; t < num_threads; t++) {
            size_t this_chunk = per + ((size_t)t < rem ? 1 : 0);
            chunks[t].phf       = phf;
            chunks[t].keys      = keys;
            chunks[t].key_lens  = key_lens;
            chunks[t].out_slots = out_slots;
            chunks[t].start     = cursor;
            chunks[t].end       = cursor + this_chunk;
            cursor += this_chunk;
            if (pthread_create(&threads[t], NULL, batch_worker, &chunks[t]) == 0) {
                spawned++;
            } else {
                /* Spawn failed; do this and remaining chunks on the calling thread. */
                for (int u = t; u < num_threads; u++) batch_worker(&chunks[u]);
                break;
            }
        }
        for (int t = 0; t < spawned; t++) pthread_join(threads[t], NULL);
        free(threads); free(chunks);
        return;
    }
serial:
#endif
    for (size_t i = 0; i < n; i++)
        out_slots[i] = phobic_query(phf, keys[i], key_lens[i]);
}

/* ── serialization (wire format v3) ───────────────────────────────────── */

#define PHOBIC_MAGIC_V3 ((uint32_t)0x33464850) /* little-endian: wire bytes 0x50 0x48 0x46 0x33 = "PHF3" */
#define PHOBIC_VERSION  ((uint32_t)3)

static inline void write_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1]<<8));
}
static inline void write_u32(uint8_t *p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static inline uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8)
         | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void write_u64(uint8_t *p, uint64_t v) {
    write_u32(p, (uint32_t)v); write_u32(p+4, (uint32_t)(v>>32));
}
static inline uint64_t read_u64(const uint8_t *p) {
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p+4)<<32);
}

#define V3_GLOBAL_HEADER_SIZE  56u
#define V3_SHARD_DESC_SIZE     40u

size_t phobic_serialize(const phobic_phf *phf, uint8_t *buf, size_t buf_len) {
    if (!phf) return 0;
    size_t total = serialized_size(phf);
    if (!buf) return total;
    if (buf_len < total) return 0;

    /* Compute pilot block offsets (8-byte aligned). */
    size_t descriptors_off = V3_GLOBAL_HEADER_SIZE;
    size_t pilots_start = descriptors_off + V3_SHARD_DESC_SIZE * phf->num_shards;
    pilots_start = (pilots_start + 7) & ~(size_t)7;

    size_t *pilot_off = malloc(phf->num_shards * sizeof(size_t));
    if (!pilot_off) return 0;
    {
        size_t cur = pilots_start;
        for (size_t s = 0; s < phf->num_shards; s++) {
            pilot_off[s] = cur;
            cur += phf->shard_num_buckets[s] * sizeof(uint16_t);
            if (s + 1 < phf->num_shards) cur = (cur + 7) & ~(size_t)7;
        }
    }

    /* Zero-fill the buffer first (cheap; covers all alignment padding). */
    memset(buf, 0, total);

    /* Global header. */
    uint8_t *p = buf;
    write_u32(p, PHOBIC_MAGIC_V3);    p += 4;
    write_u32(p, PHOBIC_VERSION);     p += 4;
    write_u64(p, (uint64_t)phf->num_keys);     p += 8;
    write_u64(p, (uint64_t)phf->total_range);  p += 8;
    write_u64(p, (uint64_t)phf->num_shards);   p += 8;
    write_u64(p, phf->seed);                   p += 8;
    write_u64(p, phf->shard_seed);             p += 8;
    write_u64(p, 0);                           p += 8;  /* reserved */

    /* Shard descriptors. */
    for (size_t s = 0; s < phf->num_shards; s++) {
        uint8_t *d = buf + descriptors_off + V3_SHARD_DESC_SIZE * s;
        write_u64(d +  0, phf->shard_seeds[s]);
        write_u64(d +  8, (uint64_t)phf->shard_bucket_size[s]);
        write_u64(d + 16, (uint64_t)phf->shard_num_buckets[s]);
        write_u64(d + 24, (uint64_t)phf->shard_range[s]);
        write_u64(d + 32, (uint64_t)pilot_off[s]);
    }

    /* Pilot blocks. */
    for (size_t s = 0; s < phf->num_shards; s++) {
        uint8_t *d = buf + pilot_off[s];
        for (size_t b = 0; b < phf->shard_num_buckets[s]; b++)
            write_u16(d + b * 2, phf->shard_pilots[s][b]);
    }

    free(pilot_off);
    return total;
}

phobic_phf *phobic_deserialize(const uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len < V3_GLOBAL_HEADER_SIZE) return NULL;

    if (read_u32(buf + 0) != PHOBIC_MAGIC_V3) return NULL;
    if (read_u32(buf + 4) != PHOBIC_VERSION) return NULL;

    uint64_t num_keys     = read_u64(buf +  8);
    uint64_t total_range  = read_u64(buf + 16);
    uint64_t num_shards   = read_u64(buf + 24);
    uint64_t seed         = read_u64(buf + 32);
    uint64_t shard_seed   = read_u64(buf + 40);

    if (num_shards == 0) return NULL;
    if (num_shards > (SIZE_MAX - V3_GLOBAL_HEADER_SIZE) / V3_SHARD_DESC_SIZE) return NULL;
    size_t descriptors_end = V3_GLOBAL_HEADER_SIZE + V3_SHARD_DESC_SIZE * num_shards;
    if (buf_len < descriptors_end) return NULL;

    phobic_phf *phf = calloc(1, sizeof(phobic_phf));
    if (!phf) return NULL;
    phf->num_keys          = (size_t)num_keys;
    phf->total_range       = (size_t)total_range;
    phf->num_shards        = (size_t)num_shards;
    phf->seed              = seed;
    phf->shard_seed        = shard_seed;
    phf->shard_seeds       = calloc((size_t)num_shards, sizeof(uint64_t));
    phf->shard_range       = calloc((size_t)num_shards, sizeof(size_t));
    phf->shard_offsets     = calloc((size_t)num_shards, sizeof(size_t));
    phf->shard_num_buckets = calloc((size_t)num_shards, sizeof(size_t));
    phf->shard_bucket_size = calloc((size_t)num_shards, sizeof(size_t));
    phf->shard_pilots      = calloc((size_t)num_shards, sizeof(uint16_t *));
    if (!phf->shard_seeds || !phf->shard_range || !phf->shard_offsets
        || !phf->shard_num_buckets || !phf->shard_bucket_size
        || !phf->shard_pilots) {
        phobic_free(phf);
        return NULL;
    }

    size_t cumulative = 0;
    for (size_t s = 0; s < (size_t)num_shards; s++) {
        const uint8_t *d = buf + V3_GLOBAL_HEADER_SIZE + V3_SHARD_DESC_SIZE * s;
        uint64_t s_seed   = read_u64(d +  0);
        uint64_t s_bsize  = read_u64(d +  8);
        uint64_t s_nbck   = read_u64(d + 16);
        uint64_t s_range  = read_u64(d + 24);
        uint64_t s_poff   = read_u64(d + 32);

        phf->shard_seeds[s]       = s_seed;
        phf->shard_bucket_size[s] = (size_t)s_bsize;
        phf->shard_num_buckets[s] = (size_t)s_nbck;
        phf->shard_range[s]       = (size_t)s_range;
        phf->shard_offsets[s]     = cumulative;
        cumulative += (size_t)s_range;

        if (s_nbck == 0) {
            phf->shard_pilots[s] = NULL;
            continue;
        }

        /* Validate offset and length fit in the buffer. */
        if (s_poff < descriptors_end) { phobic_free(phf); return NULL; }
        if ((s_poff & 7) != 0) { phobic_free(phf); return NULL; }
        if (s_nbck > SIZE_MAX / sizeof(uint16_t)) { phobic_free(phf); return NULL; }
        size_t pilot_bytes = (size_t)s_nbck * sizeof(uint16_t);
        /* s_poff + pilot_bytes can overflow uint64 on a hostile blob and
         * wrap past the bound; check without adding (s_poff is untrusted). */
        if (s_poff > buf_len || pilot_bytes > buf_len - s_poff) {
            phobic_free(phf); return NULL;
        }

        uint16_t *pilots = malloc(pilot_bytes);
        if (!pilots) { phobic_free(phf); return NULL; }
        const uint8_t *src = buf + s_poff;
        for (size_t b = 0; b < (size_t)s_nbck; b++)
            pilots[b] = read_u16(src + b * 2);
        phf->shard_pilots[s] = pilots;
    }

    /* Sanity: header total_range must match prefix sum. */
    if (cumulative != phf->total_range) { phobic_free(phf); return NULL; }

    return phf;
}
