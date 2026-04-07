#include "_phobic.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── little-endian load helpers ──────────────────────────────────────── */

/* Explicit LE reads ensure identical hash output on all architectures. */
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

    bl->members = malloc(num_keys * sizeof(size_t));
    size_t *write_pos = malloc(num_buckets * sizeof(size_t));
    if (!bl->members || !write_pos) { free(write_pos); goto fail; }
    memcpy(write_pos, bl->prefix, num_buckets * sizeof(size_t));
    for (size_t i = 0; i < num_keys; i++) {
        size_t b = bucket_for(hashes[i].h1, num_buckets);
        bl->members[write_pos[b]++] = i;
    }
    free(write_pos);

    /* Sort bucket indices by descending size using a packed {count, index}
     * array so qsort's comparator needs no external state. */
    bucket_entry *entries = malloc(num_buckets * sizeof(bucket_entry));
    bl->order = malloc(num_buckets * sizeof(size_t));
    if (!entries || !bl->order) { free(entries); goto fail; }
    for (size_t b = 0; b < num_buckets; b++)
        entries[b] = (bucket_entry){ bl->counts[b], b };
    qsort(entries, num_buckets, sizeof(bucket_entry), bucket_entry_cmp_desc);
    for (size_t b = 0; b < num_buckets; b++) bl->order[b] = entries[b].index;
    free(entries);

    bl->max_bucket = bl->counts[bl->order[0]];
    return 0;

fail:
    bucket_layout_free(bl);
    return -1;
}

/* ── assemble a phobic_phf from completed pilots ─────────────────────── */

static phobic_phf *assemble_phf(uint16_t *pilots, size_t num_keys,
                                size_t range_size, size_t num_buckets,
                                size_t bucket_size, uint64_t seed,
                                size_t collisions) {
    phobic_phf *phf = malloc(sizeof(phobic_phf));
    if (!phf) { free(pilots); return NULL; }
    phf->pilots      = pilots;
    phf->num_keys    = num_keys;
    phf->range_size  = range_size;
    phf->num_buckets = num_buckets;
    phf->bucket_size = bucket_size;
    phf->seed        = seed;
    phf->collisions  = collisions;
    return phf;
}

/* ── build ───────────────────────────────────────────────────────────── */

/* strict=1: return NULL on first unsolvable bucket.
 * strict=0: fall back to pilot 0 for unsolvable buckets; count collisions. */
static phobic_phf *try_build(const dual_hash *hashes, size_t num_keys,
                              size_t range_size, size_t num_buckets,
                              size_t bucket_size, uint64_t seed, int strict) {
    bucket_layout bl;
    if (bucket_layout_init(&bl, hashes, num_keys, num_buckets) < 0)
        return NULL;

    uint16_t *pilots = calloc(num_buckets, sizeof(uint16_t));
    size_t bsw = bitset_words(range_size);
    uint64_t *occupied = calloc(bsw, sizeof(uint64_t));
    size_t *candidates = malloc(bl.max_bucket * sizeof(size_t));
    if (!pilots || !occupied || !candidates) {
        free(pilots); free(occupied); free(candidates);
        bucket_layout_free(&bl);
        return NULL;
    }

    size_t collisions = 0;
    int failed = 0;
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
            if (strict) { failed = 1; break; }
            /* Non-strict: fall back to pilot 0.  Compute actual slots,
             * mark them in the bitset, and count only the keys whose
             * slot was already occupied (true collisions). */
            pilots[b] = 0;
            for (size_t k = 0; k < bsize; k++) {
                size_t ki = bl.members[bstart + k];
                size_t slot = slot_with_pilot(hashes[ki].h2, 0, range_size);
                if (bitset_test(occupied, slot))
                    collisions++;
                bitset_set(occupied, slot);
            }
        }
    }

    free(occupied);
    free(candidates);
    bucket_layout_free(&bl);

    if (failed) { free(pilots); return NULL; }
    return assemble_phf(pilots, num_keys, range_size, num_buckets, bucket_size, seed,
                        collisions);
}

/* ── query ───────────────────────────────────────────────────────────── */

size_t phobic_query(const phobic_phf *phf, const char *key, size_t key_len) {
    dual_hash dh = hash_key(key, key_len, phf->seed);
    size_t b = bucket_for(dh.h1, phf->num_buckets);
    return slot_with_pilot(dh.h2, phf->pilots[b], phf->range_size);
}

/* ── public build ────────────────────────────────────────────────────── */

phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                          size_t num_keys, double alpha, uint64_t seed,
                          int max_retries, int strict) {
    if (num_keys == 0) return NULL;
    if (max_retries <= 0) max_retries = 1;

    static const int BUMP_INTERVAL = 10;
    static const double ALPHA_BUMP = 0.005;

    phobic_phf *best = NULL; /* non-strict: track fewest collisions */

    for (int attempt = 0; attempt < max_retries; attempt++) {
        uint64_t cur_seed = seed ^ (uint64_t)attempt;
        double cur_alpha = alpha + ALPHA_BUMP * (double)(attempt / BUMP_INTERVAL);

        size_t range_size = (size_t)ceil((double)num_keys * (1.0 + cur_alpha));
        if (range_size < num_keys + 1) range_size = num_keys + 1;

        size_t bucket_size = (size_t)ceil(log2((double)num_keys));
        if (bucket_size < 1) bucket_size = 1;
        size_t num_buckets = (num_keys + bucket_size - 1) / bucket_size;

        dual_hash *hashes = malloc(num_keys * sizeof(dual_hash));
        if (!hashes) { phobic_free(best); return NULL; }
        for (size_t i = 0; i < num_keys; i++)
            hashes[i] = hash_key(keys[i], key_lens[i], cur_seed);

        phobic_phf *phf = try_build(hashes, num_keys, range_size,
                                    num_buckets, bucket_size, cur_seed, strict);
        free(hashes);

        if (!phf) continue; /* strict mode: try failed, next attempt */

        if (phf->collisions == 0) {
            phobic_free(best);
            return phf; /* perfect — done */
        }

        /* Non-strict: keep this if it is better than our current best. */
        if (!best || phf->collisions < best->collisions) {
            phobic_free(best);
            best = phf;
        } else {
            phobic_free(phf);
        }
    }
    return best; /* NULL in strict mode if all attempts failed */
}

/* ── serialization ──────────────────────────────────────────────────── */

#define PHOBIC_MAGIC   ((uint32_t)0x50484F42) /* "PHOB" */
#define PHOBIC_VERSION ((uint32_t)2)

/* v2 header: magic(4) + version(4) + num_keys(8) + range_size(8) +
 *            num_buckets(8) + bucket_size(8) + seed(8) + collisions(8) = 56 */
#define PHOBIC_HEADER_SIZE (sizeof(uint32_t) * 2 + sizeof(uint64_t) * 6)

static inline void write_u32(uint8_t *p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static inline void write_u64(uint8_t *p, uint64_t v) {
    write_u32(p, (uint32_t)v); write_u32(p+4, (uint32_t)(v>>32));
}
static inline uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint64_t read_u64(const uint8_t *p) {
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p+4)<<32);
}
static inline void write_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1]<<8));
}

size_t phobic_serialize(const phobic_phf *phf, uint8_t *buf, size_t buf_len) {
    if (!phf) return 0;

    size_t pilots_bytes = phf->num_buckets * sizeof(uint16_t);
    size_t total = PHOBIC_HEADER_SIZE + pilots_bytes;

    if (!buf) return total;
    if (buf_len < total) return 0;

    uint8_t *p = buf;
    write_u32(p, PHOBIC_MAGIC);    p += sizeof(uint32_t);
    write_u32(p, PHOBIC_VERSION);  p += sizeof(uint32_t);
    write_u64(p, (uint64_t)phf->num_keys);    p += sizeof(uint64_t);
    write_u64(p, (uint64_t)phf->range_size);  p += sizeof(uint64_t);
    write_u64(p, (uint64_t)phf->num_buckets); p += sizeof(uint64_t);
    write_u64(p, (uint64_t)phf->bucket_size); p += sizeof(uint64_t);
    write_u64(p, phf->seed);                  p += sizeof(uint64_t);
    write_u64(p, (uint64_t)phf->collisions);  p += sizeof(uint64_t);
    for (size_t b = 0; b < phf->num_buckets; b++, p += 2)
        write_u16(p, phf->pilots[b]);

    return total;
}

phobic_phf *phobic_deserialize(const uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len < PHOBIC_HEADER_SIZE) return NULL;

    const uint8_t *p = buf;
    uint32_t magic   = read_u32(p); p += sizeof(uint32_t);
    uint32_t version = read_u32(p); p += sizeof(uint32_t);

    if (magic != PHOBIC_MAGIC || version != PHOBIC_VERSION) return NULL;

    uint64_t num_keys    = read_u64(p); p += sizeof(uint64_t);
    uint64_t range_size  = read_u64(p); p += sizeof(uint64_t);
    uint64_t num_buckets = read_u64(p); p += sizeof(uint64_t);
    uint64_t bucket_size = read_u64(p); p += sizeof(uint64_t);
    uint64_t seed        = read_u64(p); p += sizeof(uint64_t);
    uint64_t collisions  = read_u64(p); p += sizeof(uint64_t);

    if (num_keys == 0 || num_buckets == 0 || range_size == 0 || bucket_size == 0) return NULL;
    if (num_buckets > SIZE_MAX / sizeof(uint16_t)) return NULL;
    size_t pilots_bytes = (size_t)num_buckets * sizeof(uint16_t);
    if (buf_len < PHOBIC_HEADER_SIZE + pilots_bytes) return NULL;

    uint16_t *pilots = malloc(pilots_bytes);
    if (!pilots) return NULL;
    for (size_t b = 0; b < (size_t)num_buckets; b++)
        pilots[b] = read_u16(p + b * 2);

    return assemble_phf(pilots, (size_t)num_keys, (size_t)range_size,
                        (size_t)num_buckets, (size_t)bucket_size, seed,
                        (size_t)collisions);
}
