#ifndef PHOBIC_H
#define PHOBIC_H

#include <stddef.h>
#include <stdint.h>

/* phobic 0.3.0 C ABI.
 *
 * One PHF type. Always-shard-aware (single-shard is the small-N degenerate;
 * its overhead is constant: one extra uint64 add per query). Always strict
 * (a non-perfect "PHF" is a category error; on failure we return NULL with
 * a structured diagnostic). Multi-shard builds happen inside this C ABI;
 * Python no longer orchestrates them.
 */

typedef struct {
    /* global */
    size_t   num_keys;       /* sum over shards */
    size_t   total_range;    /* sum of shard_range[]; == phf.range_size */
    size_t   num_shards;     /* >= 1 */
    uint64_t seed;           /* user-provided seed; internals derived from this */
    uint64_t shard_seed;     /* derived; partitions keys to shards */

    /* per-shard arrays, length = num_shards */
    uint64_t  *shard_seeds;       /* derived per-shard hash seeds */
    size_t    *shard_range;       /* slots per shard */
    size_t    *shard_offsets;     /* prefix sum: offsets[s] = sum(range[0..s]) */
    size_t    *shard_num_buckets;
    size_t    *shard_bucket_size;
    uint16_t **shard_pilots;      /* shard_pilots[s] points to num_buckets[s] uint16s */
} phobic_phf;

/* Options passed to phobic_build. Fields set to 0 are auto-resolved.
 *
 * load_factor: in (0, 1]. range_per_shard = ceil(N_shard / load_factor).
 * seed:        user seed. Same seed -> identical PHF byte-for-byte.
 * max_retries: per-shard retry budget. Default (0) -> 100.
 * bucket_size: keys per bucket. 0 -> ceil(log2(N_shard)) per shard.
 * num_shards:  0 -> data-driven auto (see auto_num_shards in _phobic.c).
 * num_threads: 0 -> auto (sysconf or num_shards, whichever smaller).
 */
typedef struct {
    double   load_factor;
    uint64_t seed;
    int      max_retries;
    size_t   bucket_size;
    size_t   num_shards;
    int      num_threads;
} phobic_build_opts;

/* Filled in by phobic_build_with_diag when the build returns NULL.
 * Caller allocates and zeros before the call. Untouched on success.
 *
 * failed_shard:           -1 if the failure was non-shard (e.g. OOM, invalid input)
 * best_collisions:        across all retry attempts on the failing shard, the
 *                         smallest "first unplaceable bucket size" seen. A
 *                         rough proxy for how close the build got; tune with
 *                         more retries or a lower load_factor.
 * resolved_load_factor:   what the per-shard build was actually using (may have
 *                         drifted from opts->load_factor due to retry bumps)
 * resolved_bucket_size:   what the per-shard build was actually using
 */
typedef struct {
    int    failed_shard;
    size_t best_collisions;
    double resolved_load_factor;
    size_t resolved_bucket_size;
} phobic_build_diag;

/* Build. Returns NULL on failure; if diag is non-NULL, fills it. */
phobic_phf *phobic_build_with_diag(const char **keys,
                                    const size_t *key_lens,
                                    size_t num_keys,
                                    const phobic_build_opts *opts,
                                    phobic_build_diag *diag);

/* Convenience: same as above with diag = NULL. */
phobic_phf *phobic_build(const char **keys,
                          const size_t *key_lens,
                          size_t num_keys,
                          const phobic_build_opts *opts);

/* Scalar query. Caller must guarantee phf was built. key_len may be 0. */
size_t phobic_query(const phobic_phf *phf, const char *key, size_t key_len);

/* Batch query. out_slots must hold at least n size_t.
 * num_threads <= 0: auto (online CPU count).
 * num_threads == 1: serial.
 * num_threads > 1:  pthread fan-out, above an internal size threshold.
 */
void phobic_query_batch(const phobic_phf *phf,
                         const char **keys,
                         const size_t *key_lens,
                         size_t n,
                         size_t *out_slots,
                         int num_threads);

void   phobic_free(phobic_phf *phf);
double phobic_bits_per_key(const phobic_phf *phf);

/* Wire format v3.
 *
 * phobic_serialize(phf, NULL, 0)  returns the byte size needed.
 * phobic_serialize(phf, buf, n)   writes the blob if n is large enough,
 *                                  returns the number of bytes written or 0 on error.
 *
 * phobic_deserialize(buf, n)      returns a freshly-allocated phobic_phf or NULL.
 *                                  Magic check: first 4 bytes must equal "PHF3" on the wire.
 */
size_t       phobic_serialize(const phobic_phf *phf, uint8_t *buf, size_t buf_len);
phobic_phf  *phobic_deserialize(const uint8_t *buf, size_t buf_len);

#endif /* PHOBIC_H */
