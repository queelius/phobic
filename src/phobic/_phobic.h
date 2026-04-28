#ifndef PHOBIC_H
#define PHOBIC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t *pilots;
    size_t    num_keys;
    size_t    range_size;
    size_t    num_buckets;
    size_t    bucket_size;
    uint64_t  seed;
    size_t    collisions;  /* 0 for a perfect hash function */
} phobic_phf;

/* strict=1: return NULL if any bucket cannot be placed (classic PHF behaviour).
 * strict=0: fall back to pilot 0 for unsolvable buckets, record collision count.
 *           Tries up to max_retries seeds and returns the attempt with the fewest
 *           collisions (may be 0 if a perfect build is found early).
 * bucket_size_param: average keys per bucket; pass 0 for the auto default of
 *           ceil(log2(num_keys)). Smaller values build faster but use more
 *           bits per key (less pilot amortization). Larger values use fewer
 *           bits per key but the per-bucket pilot search is exponentially
 *           harder; values much above log2(N) often fail to build. */
phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                          size_t num_keys, double alpha, uint64_t seed,
                          int max_retries, int strict,
                          size_t bucket_size_param);
size_t phobic_query(const phobic_phf *phf, const char *key, size_t key_len);
void phobic_free(phobic_phf *phf);
size_t phobic_serialize(const phobic_phf *phf, uint8_t *buf, size_t buf_len);
phobic_phf *phobic_deserialize(const uint8_t *buf, size_t buf_len);
double phobic_bits_per_key(const phobic_phf *phf);

#endif
