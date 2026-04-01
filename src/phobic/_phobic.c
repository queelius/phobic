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
