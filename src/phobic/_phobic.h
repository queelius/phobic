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
} phobic_phf;

phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                          size_t num_keys, double alpha,
                          uint64_t seed, int threads);
size_t phobic_query(const phobic_phf *phf, const char *key, size_t key_len);
void phobic_free(phobic_phf *phf);
size_t phobic_serialize(const phobic_phf *phf, uint8_t *buf, size_t buf_len);
phobic_phf *phobic_deserialize(const uint8_t *buf, size_t buf_len);
double phobic_bits_per_key(const phobic_phf *phf);

#endif
