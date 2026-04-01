#include "_phobic.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

/* ── wyhash-style mixing ─────────────────────────────────────────────── */

static inline uint64_t wymix(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)(r >> 64) ^ (uint64_t)r;
}

static uint64_t phobic_hash(const char *key, size_t len, uint64_t seed) {
    uint64_t h = seed;
    const uint8_t *p = (const uint8_t *)key;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = wymix(h ^ v, 0x9e3779b97f4a7c15ULL);
        p += 8; len -= 8;
    }
    uint64_t tail = 0;
    if (len > 0) { memcpy(&tail, p, len); h = wymix(h ^ tail, 0x517cc1b727220a95ULL); }
    return wymix(h, 0x94d049bb133111ebULL);
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

/* ── lifetime / stats ────────────────────────────────────────────────── */

void phobic_free(phobic_phf *phf) {
    if (phf) { free(phf->pilots); free(phf); }
}

double phobic_bits_per_key(const phobic_phf *phf) {
    if (!phf || phf->num_keys == 0) return 0.0;
    size_t bytes = phf->num_buckets * sizeof(uint16_t) + sizeof(phobic_phf);
    return (double)(bytes * 8) / (double)phf->num_keys;
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

/* ── single-threaded build ───────────────────────────────────────────── */

static phobic_phf *try_build_single(const dual_hash *hashes, size_t num_keys,
                                     size_t range_size, size_t num_buckets,
                                     size_t bucket_size, uint64_t seed) {
    /*
     * 1. Compute bucket membership via prefix-sum.
     * 2. Sort buckets by descending size.
     * 3. For each bucket (largest first), brute-force pilot search
     *    against a global occupied bitset.
     */

    /* ── bucket counts ──────────────────────────────────────────────── */
    size_t *counts = calloc(num_buckets, sizeof(size_t));
    if (!counts) return NULL;

    for (size_t i = 0; i < num_keys; i++)
        counts[bucket_for(hashes[i].h1, num_buckets)]++;

    /* ── prefix-sum for O(1) bucket member access ───────────────────── */
    size_t *prefix = malloc((num_buckets + 1) * sizeof(size_t));
    if (!prefix) { free(counts); return NULL; }
    prefix[0] = 0;
    for (size_t b = 0; b < num_buckets; b++)
        prefix[b + 1] = prefix[b] + counts[b];

    /* ── scatter keys into bucket-ordered array ─────────────────────── */
    size_t *bucket_members = malloc(num_keys * sizeof(size_t));
    size_t *write_pos = malloc(num_buckets * sizeof(size_t));
    if (!bucket_members || !write_pos) {
        free(counts); free(prefix); free(bucket_members); free(write_pos);
        return NULL;
    }
    memcpy(write_pos, prefix, num_buckets * sizeof(size_t));
    for (size_t i = 0; i < num_keys; i++) {
        size_t b = bucket_for(hashes[i].h1, num_buckets);
        bucket_members[write_pos[b]++] = i;
    }
    free(write_pos);

    /* ── sort bucket indices by descending size ─────────────────────── */
    size_t *order = malloc(num_buckets * sizeof(size_t));
    if (!order) {
        free(counts); free(prefix); free(bucket_members); free(order);
        return NULL;
    }
    for (size_t b = 0; b < num_buckets; b++) order[b] = b;

    /* simple insertion sort -- num_buckets is moderate */
    for (size_t i = 1; i < num_buckets; i++) {
        size_t key = order[i];
        size_t key_cnt = counts[key];
        size_t j = i;
        while (j > 0 && counts[order[j - 1]] < key_cnt) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    /* ── allocate pilots + occupied bitset ──────────────────────────── */
    uint16_t *pilots = calloc(num_buckets, sizeof(uint16_t));
    size_t bsw = bitset_words(range_size);
    uint64_t *occupied = calloc(bsw, sizeof(uint64_t));
    if (!pilots || !occupied) {
        free(counts); free(prefix); free(bucket_members); free(order);
        free(pilots); free(occupied);
        return NULL;
    }

    /* ── temp array for candidate slots within a bucket ─────────────── */
    size_t max_bucket = counts[order[0]];
    size_t *candidates = malloc(max_bucket * sizeof(size_t));
    if (!candidates) {
        free(counts); free(prefix); free(bucket_members); free(order);
        free(pilots); free(occupied); free(candidates);
        return NULL;
    }

    /* ── pilot search ───────────────────────────────────────────────── */
    int failed = 0;
    for (size_t oi = 0; oi < num_buckets && !failed; oi++) {
        size_t b = order[oi];
        size_t bsize = counts[b];
        if (bsize == 0) { pilots[b] = 0; continue; }

        size_t bstart = prefix[b];
        int found = 0;

        for (uint32_t pilot = 0; pilot < 65535 && !found; pilot++) {
            /* compute candidate slots for this pilot */
            int collision = 0;
            for (size_t k = 0; k < bsize && !collision; k++) {
                size_t ki = bucket_members[bstart + k];
                size_t slot = slot_with_pilot(hashes[ki].h2, (uint16_t)pilot, range_size);
                /* check global occupied */
                if (bitset_test(occupied, slot)) { collision = 1; break; }
                /* check internal collisions within this bucket */
                for (size_t prev = 0; prev < k; prev++) {
                    if (candidates[prev] == slot) { collision = 1; break; }
                }
                candidates[k] = slot;
            }
            if (!collision) {
                /* commit: mark slots occupied, store pilot */
                for (size_t k = 0; k < bsize; k++)
                    bitset_set(occupied, candidates[k]);
                pilots[b] = (uint16_t)pilot;
                found = 1;
            }
        }
        if (!found) failed = 1;
    }

    free(counts); free(prefix); free(bucket_members);
    free(order); free(occupied); free(candidates);

    if (failed) { free(pilots); return NULL; }

    /* ── assemble result ────────────────────────────────────────────── */
    phobic_phf *phf = malloc(sizeof(phobic_phf));
    if (!phf) { free(pilots); return NULL; }
    phf->pilots      = pilots;
    phf->num_keys    = num_keys;
    phf->range_size  = range_size;
    phf->num_buckets = num_buckets;
    phf->bucket_size = bucket_size;
    phf->seed        = seed;
    return phf;
}

/* ── query ───────────────────────────────────────────────────────────── */

size_t phobic_query(const phobic_phf *phf, const char *key, size_t key_len) {
    dual_hash dh = hash_key(key, key_len, phf->seed);
    size_t b = bucket_for(dh.h1, phf->num_buckets);
    return slot_with_pilot(dh.h2, phf->pilots[b], phf->range_size);
}

/* ── public build ────────────────────────────────────────────────────── */

phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                          size_t num_keys, double alpha,
                          uint64_t seed, int threads) {
    (void)threads; /* parallel build deferred to Task 4 */

    if (num_keys == 0) return NULL;

    static const int MAX_RETRIES = 100;
    static const int BUMP_INTERVAL = 10;
    static const double ALPHA_BUMP = 0.005;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        uint64_t cur_seed = seed ^ (uint64_t)attempt;
        double cur_alpha = alpha + ALPHA_BUMP * (double)(attempt / BUMP_INTERVAL);

        size_t range_size = (size_t)ceil((double)num_keys * (1.0 + cur_alpha));
        if (range_size < num_keys + 1) range_size = num_keys + 1;

        size_t bucket_size = (size_t)ceil(log2((double)num_keys));
        if (bucket_size < 1) bucket_size = 1;
        size_t num_buckets = (num_keys + bucket_size - 1) / bucket_size;

        /* precompute hashes with current seed */
        dual_hash *hashes = malloc(num_keys * sizeof(dual_hash));
        if (!hashes) return NULL;
        for (size_t i = 0; i < num_keys; i++)
            hashes[i] = hash_key(keys[i], key_lens[i], cur_seed);

        phobic_phf *phf = try_build_single(hashes, num_keys, range_size,
                                            num_buckets, bucket_size, cur_seed);
        free(hashes);

        if (phf) return phf;
    }
    return NULL;
}
