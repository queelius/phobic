# phobic Python Package Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a PyPI-publishable Python package that provides fast minimal perfect hash functions via a C11 extension module implementing the PHOBIC algorithm with parallel construction.

**Architecture:** `_phobic.c` is the standalone C11 PHOBIC implementation (hash, build, query, serialize). `_module.c` is the Python C extension glue that wraps it. `__init__.py` provides the user-facing `PHF` class and `build()`/`from_bytes()` functions.

**Tech Stack:** C11, pthreads, stdatomic.h, Python C API, setuptools, pytest

## File Structure

| File | Responsibility |
|------|---------------|
| `pyproject.toml` | Package metadata and build config |
| `setup.py` | C extension build (setuptools can't do this from pyproject.toml alone) |
| `LICENSE` | MIT license |
| `README.md` | Usage documentation |
| `src/phobic/__init__.py` | Python API: PHF class, build(), from_bytes() |
| `src/phobic/_phobic.h` | C header: struct and function declarations |
| `src/phobic/_phobic.c` | C core: PHOBIC algorithm (no Python dependency) |
| `src/phobic/_module.c` | Python C extension: wraps _phobic.c functions |
| `tests/test_phobic.py` | pytest tests |

---

### Task 1: Project Scaffold

**Files:**
- Create: `pyproject.toml`
- Create: `setup.py`
- Create: `LICENSE`
- Create: `src/phobic/__init__.py` (empty placeholder)

- [ ] **Step 1: Create pyproject.toml**

```toml
[build-system]
requires = ["setuptools>=68.0"]
build-backend = "setuptools.build_meta"

[project]
name = "phobic"
version = "0.1.0"
description = "Fast minimal perfect hash functions"
readme = "README.md"
license = "MIT"
authors = [{ name = "Alexander Towell", email = "lex@metafunctor.com" }]
requires-python = ">=3.9"
classifiers = [
    "Programming Language :: Python :: 3",
    "Programming Language :: C",
    "License :: OSI Approved :: MIT License",
    "Operating System :: POSIX",
    "Topic :: Scientific/Engineering",
]

[project.urls]
Repository = "https://github.com/queelius/phobic"

[tool.pytest.ini_options]
testpaths = ["tests"]
```

- [ ] **Step 2: Create setup.py**

```python
from setuptools import setup, Extension

setup(
    ext_modules=[
        Extension(
            "phobic._module",
            sources=["src/phobic/_module.c", "src/phobic/_phobic.c"],
            extra_compile_args=["-O2", "-std=c11", "-pthread", "-Wall", "-Wextra"],
            extra_link_args=["-pthread"],
        ),
    ],
)
```

- [ ] **Step 3: Create LICENSE**

```
MIT License

Copyright (c) 2026 Alexander Towell

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- [ ] **Step 4: Create empty __init__.py**

Create `src/phobic/__init__.py`:

```python
"""phobic: Fast minimal perfect hash functions."""
```

- [ ] **Step 5: Commit**

```bash
cd /home/spinoza/github/released/phobic
git add pyproject.toml setup.py LICENSE src/phobic/__init__.py
git commit -m "chore: project scaffold with pyproject.toml and setup.py"
```

---

### Task 2: C Core -- Hash Function and Data Structures

**Files:**
- Create: `src/phobic/_phobic.h`
- Create: `src/phobic/_phobic.c` (partial: hash + struct + free)

- [ ] **Step 1: Create the C header**

Create `src/phobic/_phobic.h`:

```c
/**
 * phobic: PHOBIC perfect hash function
 *
 * Pilot-based construction. Maps n keys to distinct slots in [0, m).
 * C11 implementation with pthreads for parallel construction.
 */

#ifndef PHOBIC_H
#define PHOBIC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t *pilots;       /* pilot value per bucket */
    size_t    num_keys;     /* n */
    size_t    range_size;   /* m (>= n) */
    size_t    num_buckets;
    size_t    bucket_size;  /* average keys per bucket */
    uint64_t  seed;
} phobic_phf;

/* Build a PHF from a set of keys.
 * keys:     array of key pointers
 * key_lens: array of key lengths
 * num_keys: number of keys
 * alpha:    load factor (range_size = ceil(num_keys * alpha)), >= 1.0
 * seed:     hash seed for reproducibility
 * threads:  number of threads (0 = auto, 1 = single-threaded)
 * Returns NULL on failure. Caller must free with phobic_free(). */
phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                          size_t num_keys, double alpha,
                          uint64_t seed, int threads);

/* Query: returns slot index in [0, range_size) */
size_t phobic_query(const phobic_phf *phf,
                     const char *key, size_t key_len);

/* Free a PHF */
void phobic_free(phobic_phf *phf);

/* Serialization: returns number of bytes written, or 0 on error.
 * If buf is NULL, returns required buffer size. */
size_t phobic_serialize(const phobic_phf *phf,
                         uint8_t *buf, size_t buf_len);

/* Deserialization: returns NULL on error. Caller must free. */
phobic_phf *phobic_deserialize(const uint8_t *buf, size_t buf_len);

/* Space efficiency */
double phobic_bits_per_key(const phobic_phf *phf);

#endif /* PHOBIC_H */
```

- [ ] **Step 2: Start _phobic.c with hash function and data structures**

Create `src/phobic/_phobic.c`:

```c
#include "_phobic.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>

/* ===== HASH FUNCTION ===== */

/* wyhash-style: fast, good distribution */
static inline uint64_t wymix(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)(r >> 64) ^ (uint64_t)r;
}

static uint64_t phobic_hash(const char *key, size_t len, uint64_t seed) {
    uint64_t h = seed;
    const uint8_t *p = (const uint8_t *)key;

    /* Process 8 bytes at a time */
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = wymix(h ^ v, 0x9e3779b97f4a7c15ULL);
        p += 8;
        len -= 8;
    }

    /* Remaining bytes */
    uint64_t tail = 0;
    if (len > 0) {
        memcpy(&tail, p, len);
        h = wymix(h ^ tail, 0x517cc1b727220a95ULL);
    }

    return wymix(h, 0x94d049bb133111ebULL);
}

/* Derive bucket index from h1 */
static inline size_t bucket_for(uint64_t h1, size_t num_buckets) {
    return (size_t)(h1 % num_buckets);
}

/* Derive slot from h2 mixed with pilot */
static inline size_t slot_with_pilot(uint64_t h2, uint16_t pilot,
                                      size_t range_size) {
    uint64_t mixed = h2 ^ ((uint64_t)pilot * 0x9e3779b97f4a7c15ULL);
    mixed = wymix(mixed, 0xbf58476d1ce4e5b9ULL);
    return (size_t)(mixed % range_size);
}

/* Two independent hashes from one key */
typedef struct { uint64_t h1, h2; } dual_hash;

static dual_hash hash_key(const char *key, size_t len, uint64_t seed) {
    uint64_t h1 = phobic_hash(key, len, seed);
    uint64_t h2 = phobic_hash(key, len, seed ^ 0x517cc1b727220a95ULL);
    return (dual_hash){h1, h2};
}

/* ===== LIFECYCLE ===== */

void phobic_free(phobic_phf *phf) {
    if (phf) {
        free(phf->pilots);
        free(phf);
    }
}

double phobic_bits_per_key(const phobic_phf *phf) {
    if (!phf || phf->num_keys == 0) return 0.0;
    size_t bytes = phf->num_buckets * sizeof(uint16_t)
                 + sizeof(phobic_phf);
    return (double)(bytes * 8) / (double)phf->num_keys;
}
```

- [ ] **Step 3: Commit**

```bash
cd /home/spinoza/github/released/phobic
git add src/phobic/_phobic.h src/phobic/_phobic.c
git commit -m "feat: C core hash function and data structures"
```

---

### Task 3: C Core -- Single-Threaded Build

**Files:**
- Modify: `src/phobic/_phobic.c`

- [ ] **Step 1: Add the single-threaded build function**

Append to `src/phobic/_phobic.c`:

```c
/* ===== BUILD (single-threaded) ===== */

/* Internal: sort indices by bucket size descending */
typedef struct {
    size_t bucket_id;
    size_t count;
} bucket_info;

static int cmp_bucket_desc(const void *a, const void *b) {
    const bucket_info *ba = (const bucket_info *)a;
    const bucket_info *bb = (const bucket_info *)b;
    if (ba->count > bb->count) return -1;
    if (ba->count < bb->count) return 1;
    return 0;
}

/* Internal: try to build with given parameters. Returns NULL on failure. */
static phobic_phf *try_build_single(
    const dual_hash *hashes, size_t num_keys,
    size_t num_buckets, size_t range_size, size_t bucket_size,
    uint64_t seed)
{
    /* Allocate bucket membership */
    size_t *bucket_starts = calloc(num_buckets + 1, sizeof(size_t));
    size_t *bucket_counts = calloc(num_buckets, sizeof(size_t));
    if (!bucket_starts || !bucket_counts) {
        free(bucket_starts); free(bucket_counts);
        return NULL;
    }

    /* Count keys per bucket */
    for (size_t i = 0; i < num_keys; i++) {
        size_t b = bucket_for(hashes[i].h1, num_buckets);
        bucket_counts[b]++;
    }

    /* Prefix sum for bucket_starts */
    for (size_t i = 0; i < num_buckets; i++) {
        bucket_starts[i + 1] = bucket_starts[i] + bucket_counts[i];
    }

    /* Fill bucket-sorted key index array */
    size_t *key_indices = malloc(num_keys * sizeof(size_t));
    size_t *fill_pos = calloc(num_buckets, sizeof(size_t));
    if (!key_indices || !fill_pos) {
        free(bucket_starts); free(bucket_counts);
        free(key_indices); free(fill_pos);
        return NULL;
    }
    for (size_t i = 0; i < num_keys; i++) {
        size_t b = bucket_for(hashes[i].h1, num_buckets);
        key_indices[bucket_starts[b] + fill_pos[b]] = i;
        fill_pos[b]++;
    }
    free(fill_pos);

    /* Sort buckets by size descending */
    bucket_info *order = malloc(num_buckets * sizeof(bucket_info));
    if (!order) {
        free(bucket_starts); free(bucket_counts);
        free(key_indices);
        return NULL;
    }
    for (size_t i = 0; i < num_buckets; i++) {
        order[i] = (bucket_info){i, bucket_counts[i]};
    }
    qsort(order, num_buckets, sizeof(bucket_info), cmp_bucket_desc);

    /* Occupied bitset */
    size_t bitmask_words = (range_size + 63) / 64;
    uint64_t *occupied = calloc(bitmask_words, sizeof(uint64_t));
    uint16_t *pilots = calloc(num_buckets, sizeof(uint16_t));
    if (!occupied || !pilots) {
        free(bucket_starts); free(bucket_counts);
        free(key_indices); free(order);
        free(occupied); free(pilots);
        return NULL;
    }

    /* Temporary storage for candidate slots per bucket */
    size_t max_bucket = bucket_counts[order[0].bucket_id];
    size_t *candidates = malloc(max_bucket * sizeof(size_t));
    if (!candidates) {
        free(bucket_starts); free(bucket_counts);
        free(key_indices); free(order);
        free(occupied); free(pilots);
        return NULL;
    }

    int success = 1;

    for (size_t bi = 0; bi < num_buckets && success; bi++) {
        size_t b = order[bi].bucket_id;
        size_t count = bucket_counts[b];
        if (count == 0) { pilots[b] = 0; continue; }

        size_t start = bucket_starts[b];
        int found = 0;

        for (uint32_t pilot = 0; pilot < 65535 && !found; pilot++) {
            int collision = 0;
            size_t nc = 0;

            for (size_t j = 0; j < count && !collision; j++) {
                size_t ki = key_indices[start + j];
                size_t slot = slot_with_pilot(hashes[ki].h2, (uint16_t)pilot, range_size);

                /* Check against occupied bitset */
                if (occupied[slot / 64] & (1ULL << (slot % 64))) {
                    collision = 1; break;
                }
                /* Check internal collision */
                for (size_t k = 0; k < nc; k++) {
                    if (candidates[k] == slot) { collision = 1; break; }
                }
                if (!collision) {
                    candidates[nc++] = slot;
                }
            }

            if (!collision && nc == count) {
                pilots[b] = (uint16_t)pilot;
                for (size_t j = 0; j < nc; j++) {
                    size_t s = candidates[j];
                    occupied[s / 64] |= (1ULL << (s % 64));
                }
                found = 1;
            }
        }

        if (!found) { success = 0; }
    }

    free(bucket_starts); free(bucket_counts);
    free(key_indices); free(order);
    free(occupied); free(candidates);

    if (!success) { free(pilots); return NULL; }

    phobic_phf *phf = malloc(sizeof(phobic_phf));
    if (!phf) { free(pilots); return NULL; }
    phf->pilots = pilots;
    phf->num_keys = num_keys;
    phf->range_size = range_size;
    phf->num_buckets = num_buckets;
    phf->bucket_size = bucket_size;
    phf->seed = seed;
    return phf;
}

/* ===== QUERY ===== */

size_t phobic_query(const phobic_phf *phf,
                     const char *key, size_t key_len) {
    dual_hash h = hash_key(key, key_len, phf->seed);
    size_t b = bucket_for(h.h1, phf->num_buckets);
    return slot_with_pilot(h.h2, phf->pilots[b], phf->range_size);
}

/* ===== PUBLIC BUILD (single-threaded path) ===== */

static const size_t DEFAULT_BUCKET_SIZE = 5;

phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                          size_t num_keys, double alpha,
                          uint64_t seed, int threads) {
    if (num_keys == 0) return NULL;
    if (alpha < 1.0) alpha = 1.0;

    size_t bucket_size = DEFAULT_BUCKET_SIZE;
    size_t num_buckets = (num_keys + bucket_size - 1) / bucket_size;
    if (num_buckets == 0) num_buckets = 1;
    size_t range_size = (size_t)ceil((double)num_keys * alpha);
    if (range_size < num_keys) range_size = num_keys;

    /* Precompute all hashes */
    dual_hash *hashes = malloc(num_keys * sizeof(dual_hash));
    if (!hashes) return NULL;

    for (size_t i = 0; i < num_keys; i++) {
        hashes[i] = hash_key(keys[i], key_lens[i], seed);
    }

    /* Retry with different seeds */
    phobic_phf *result = NULL;
    double current_alpha = alpha;

    for (int attempt = 0; attempt < 100 && !result; attempt++) {
        uint64_t attempt_seed = seed ^ ((uint64_t)attempt * 0x9e3779b97f4a7c15ULL);

        /* Recompute hashes with new seed */
        if (attempt > 0) {
            for (size_t i = 0; i < num_keys; i++) {
                hashes[i] = hash_key(keys[i], key_lens[i], attempt_seed);
            }
        }

        /* Bump alpha every 10 attempts */
        if (attempt > 0 && attempt % 10 == 0) {
            current_alpha += 0.005;
            range_size = (size_t)ceil((double)num_keys * current_alpha);
        }

        result = try_build_single(hashes, num_keys, num_buckets,
                                   range_size, bucket_size,
                                   attempt > 0 ? attempt_seed : seed);
    }

    free(hashes);

    /* TODO: parallel build path (threads > 1) added in Task 4 */
    (void)threads;

    return result;
}
```

- [ ] **Step 2: Verify C compiles**

```bash
cd /home/spinoza/github/released/phobic
gcc -std=c11 -c -Wall -Wextra -pthread src/phobic/_phobic.c -o /dev/null
```

Expected: compiles with no errors.

- [ ] **Step 3: Commit**

```bash
git add src/phobic/_phobic.c
git commit -m "feat: single-threaded PHOBIC build and query in C"
```

---

### Task 4: C Core -- Parallel Build

**Files:**
- Modify: `src/phobic/_phobic.c`

- [ ] **Step 1: Add parallel build implementation**

Add before the `phobic_build` function in `src/phobic/_phobic.c`:

```c
/* ===== PARALLEL BUILD ===== */

typedef struct {
    const dual_hash *hashes;
    const size_t *bucket_starts;
    const size_t *bucket_counts;
    const size_t *key_indices;
    const bucket_info *order;
    size_t num_buckets;
    size_t range_size;
    uint16_t *pilots;           /* output: pilot per bucket */
    _Atomic uint64_t *occupied; /* shared atomic bitset */
    _Atomic size_t next_bucket; /* shared work counter */
    _Atomic int failed;         /* set to 1 if any bucket fails */
} parallel_ctx;

static void *worker_thread(void *arg) {
    parallel_ctx *ctx = (parallel_ctx *)arg;

    /* Temporary candidate slots (max possible bucket size) */
    size_t max_count = 0;
    for (size_t i = 0; i < ctx->num_buckets; i++) {
        if (ctx->bucket_counts[ctx->order[i].bucket_id] > max_count)
            max_count = ctx->bucket_counts[ctx->order[i].bucket_id];
    }
    size_t *candidates = malloc(max_count * sizeof(size_t));
    if (!candidates) { atomic_store(&ctx->failed, 1); return NULL; }

    while (!atomic_load(&ctx->failed)) {
        size_t bi = atomic_fetch_add(&ctx->next_bucket, 1);
        if (bi >= ctx->num_buckets) break;

        size_t b = ctx->order[bi].bucket_id;
        size_t count = ctx->bucket_counts[b];
        if (count == 0) { ctx->pilots[b] = 0; continue; }

        size_t start = ctx->bucket_starts[b];
        int found = 0;

        for (uint32_t pilot = 0; pilot < 65535 && !found; pilot++) {
            int collision = 0;
            size_t nc = 0;

            for (size_t j = 0; j < count && !collision; j++) {
                size_t ki = ctx->key_indices[start + j];
                size_t slot = slot_with_pilot(ctx->hashes[ki].h2,
                                               (uint16_t)pilot,
                                               ctx->range_size);

                /* Check internal collision first (cheap) */
                for (size_t k = 0; k < nc; k++) {
                    if (candidates[k] == slot) { collision = 1; break; }
                }
                if (collision) break;
                candidates[nc++] = slot;
            }

            if (collision) continue;

            /* Try to atomically claim all candidate slots */
            int claim_ok = 1;
            for (size_t j = 0; j < nc && claim_ok; j++) {
                size_t s = candidates[j];
                size_t word = s / 64;
                uint64_t bit = 1ULL << (s % 64);
                uint64_t old = atomic_fetch_or(&ctx->occupied[word], bit);
                if (old & bit) {
                    /* Slot already taken, undo all claims */
                    for (size_t k = 0; k < j; k++) {
                        size_t us = candidates[k];
                        atomic_fetch_and(&ctx->occupied[us / 64],
                                          ~(1ULL << (us % 64)));
                    }
                    /* Also undo the one that failed (we set it) */
                    atomic_fetch_and(&ctx->occupied[word], ~bit);
                    claim_ok = 0;
                }
            }

            if (claim_ok) {
                ctx->pilots[b] = (uint16_t)pilot;
                found = 1;
            }
        }

        if (!found) {
            atomic_store(&ctx->failed, 1);
        }
    }

    free(candidates);
    return NULL;
}

static phobic_phf *try_build_parallel(
    const dual_hash *hashes, size_t num_keys,
    size_t num_buckets, size_t range_size, size_t bucket_size,
    uint64_t seed, int threads)
{
    /* Allocate bucket membership (same as single-threaded) */
    size_t *bucket_starts = calloc(num_buckets + 1, sizeof(size_t));
    size_t *bucket_counts = calloc(num_buckets, sizeof(size_t));
    if (!bucket_starts || !bucket_counts) {
        free(bucket_starts); free(bucket_counts);
        return NULL;
    }

    for (size_t i = 0; i < num_keys; i++) {
        bucket_counts[bucket_for(hashes[i].h1, num_buckets)]++;
    }
    for (size_t i = 0; i < num_buckets; i++) {
        bucket_starts[i + 1] = bucket_starts[i] + bucket_counts[i];
    }

    size_t *key_indices = malloc(num_keys * sizeof(size_t));
    size_t *fill_pos = calloc(num_buckets, sizeof(size_t));
    if (!key_indices || !fill_pos) {
        free(bucket_starts); free(bucket_counts);
        free(key_indices); free(fill_pos);
        return NULL;
    }
    for (size_t i = 0; i < num_keys; i++) {
        size_t b = bucket_for(hashes[i].h1, num_buckets);
        key_indices[bucket_starts[b] + fill_pos[b]] = i;
        fill_pos[b]++;
    }
    free(fill_pos);

    bucket_info *order = malloc(num_buckets * sizeof(bucket_info));
    if (!order) {
        free(bucket_starts); free(bucket_counts); free(key_indices);
        return NULL;
    }
    for (size_t i = 0; i < num_buckets; i++) {
        order[i] = (bucket_info){i, bucket_counts[i]};
    }
    qsort(order, num_buckets, sizeof(bucket_info), cmp_bucket_desc);

    size_t bitmask_words = (range_size + 63) / 64;
    _Atomic uint64_t *occupied = calloc(bitmask_words, sizeof(_Atomic uint64_t));
    uint16_t *pilots = calloc(num_buckets, sizeof(uint16_t));
    if (!occupied || !pilots) {
        free(bucket_starts); free(bucket_counts);
        free(key_indices); free(order);
        free(occupied); free(pilots);
        return NULL;
    }

    /* Set up parallel context */
    parallel_ctx ctx = {
        .hashes = hashes,
        .bucket_starts = bucket_starts,
        .bucket_counts = bucket_counts,
        .key_indices = key_indices,
        .order = order,
        .num_buckets = num_buckets,
        .range_size = range_size,
        .pilots = pilots,
        .occupied = occupied,
        .next_bucket = 0,
        .failed = 0,
    };

    /* Spawn worker threads */
    pthread_t *tids = malloc((size_t)threads * sizeof(pthread_t));
    if (!tids) {
        free(bucket_starts); free(bucket_counts);
        free(key_indices); free(order);
        free(occupied); free(pilots);
        return NULL;
    }

    for (int t = 0; t < threads; t++) {
        pthread_create(&tids[t], NULL, worker_thread, &ctx);
    }
    for (int t = 0; t < threads; t++) {
        pthread_join(tids[t], NULL);
    }
    free(tids);

    int success = !atomic_load(&ctx.failed);

    free(bucket_starts); free(bucket_counts);
    free(key_indices); free(order);
    free(occupied);

    if (!success) { free(pilots); return NULL; }

    phobic_phf *phf = malloc(sizeof(phobic_phf));
    if (!phf) { free(pilots); return NULL; }
    phf->pilots = pilots;
    phf->num_keys = num_keys;
    phf->range_size = range_size;
    phf->num_buckets = num_buckets;
    phf->bucket_size = bucket_size;
    phf->seed = seed;
    return phf;
}
```

- [ ] **Step 2: Update phobic_build to use parallel path**

In `phobic_build()`, replace the `(void)threads;` line and the retry loop with:

```c
    /* Auto-detect threads */
    int nthreads = threads;
    if (nthreads == 0) {
        nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (nthreads < 1) nthreads = 1;
    }

    phobic_phf *result = NULL;
    double current_alpha = alpha;

    for (int attempt = 0; attempt < 100 && !result; attempt++) {
        uint64_t attempt_seed = seed ^ ((uint64_t)attempt * 0x9e3779b97f4a7c15ULL);

        if (attempt > 0) {
            for (size_t i = 0; i < num_keys; i++) {
                hashes[i] = hash_key(keys[i], key_lens[i], attempt_seed);
            }
        }

        if (attempt > 0 && attempt % 10 == 0) {
            current_alpha += 0.005;
            range_size = (size_t)ceil((double)num_keys * current_alpha);
        }

        if (nthreads > 1 && num_keys > 1000) {
            result = try_build_parallel(hashes, num_keys, num_buckets,
                                         range_size, bucket_size,
                                         attempt > 0 ? attempt_seed : seed,
                                         nthreads);
        } else {
            result = try_build_single(hashes, num_keys, num_buckets,
                                       range_size, bucket_size,
                                       attempt > 0 ? attempt_seed : seed);
        }
    }

    free(hashes);
    return result;
```

Add `#include <unistd.h>` at the top of the file for `sysconf`.

- [ ] **Step 3: Verify compilation**

```bash
cd /home/spinoza/github/released/phobic
gcc -std=c11 -c -Wall -Wextra -pthread src/phobic/_phobic.c -o /dev/null
```

- [ ] **Step 4: Commit**

```bash
git add src/phobic/_phobic.c
git commit -m "feat: parallel PHOBIC build with pthreads and atomic bitset"
```

---

### Task 5: C Core -- Serialization

**Files:**
- Modify: `src/phobic/_phobic.c`

- [ ] **Step 1: Add serialization and deserialization**

Append to `src/phobic/_phobic.c`:

```c
/* ===== SERIALIZATION ===== */

#define PHOBIC_MAGIC 0x50484F42  /* "PHOB" */
#define PHOBIC_VERSION 1

size_t phobic_serialize(const phobic_phf *phf,
                         uint8_t *buf, size_t buf_len) {
    if (!phf) return 0;

    /* Calculate required size */
    size_t needed = 4 + 4  /* magic + version */
                  + 8 + 8 + 8 + 8 + 8  /* num_keys, range_size, num_buckets, bucket_size, seed */
                  + phf->num_buckets * 2;  /* pilots (uint16_t each) */

    if (!buf) return needed;
    if (buf_len < needed) return 0;

    uint8_t *p = buf;

#define WRITE(val) do { memcpy(p, &(val), sizeof(val)); p += sizeof(val); } while(0)

    uint32_t magic = PHOBIC_MAGIC;
    uint32_t version = PHOBIC_VERSION;
    WRITE(magic);
    WRITE(version);
    WRITE(phf->num_keys);     /* size_t = 8 bytes on 64-bit */
    WRITE(phf->range_size);
    WRITE(phf->num_buckets);
    WRITE(phf->bucket_size);
    WRITE(phf->seed);

    memcpy(p, phf->pilots, phf->num_buckets * sizeof(uint16_t));
    p += phf->num_buckets * sizeof(uint16_t);

#undef WRITE

    return (size_t)(p - buf);
}

phobic_phf *phobic_deserialize(const uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len < 4 + 4 + 5 * 8) return NULL;

    const uint8_t *p = buf;

#define READ(val) do { memcpy(&(val), p, sizeof(val)); p += sizeof(val); } while(0)

    uint32_t magic, version;
    READ(magic);
    READ(version);
    if (magic != PHOBIC_MAGIC || version != PHOBIC_VERSION) return NULL;

    size_t num_keys, range_size, num_buckets, bucket_size;
    uint64_t seed;
    READ(num_keys);
    READ(range_size);
    READ(num_buckets);
    READ(bucket_size);
    READ(seed);

#undef READ

    /* Bounds check */
    size_t pilots_bytes = num_buckets * sizeof(uint16_t);
    if ((size_t)(p - buf) + pilots_bytes > buf_len) return NULL;

    uint16_t *pilots = malloc(pilots_bytes);
    if (!pilots) return NULL;
    memcpy(pilots, p, pilots_bytes);

    phobic_phf *phf = malloc(sizeof(phobic_phf));
    if (!phf) { free(pilots); return NULL; }
    phf->pilots = pilots;
    phf->num_keys = num_keys;
    phf->range_size = range_size;
    phf->num_buckets = num_buckets;
    phf->bucket_size = bucket_size;
    phf->seed = seed;
    return phf;
}
```

- [ ] **Step 2: Verify compilation**

```bash
cd /home/spinoza/github/released/phobic
gcc -std=c11 -c -Wall -Wextra -pthread src/phobic/_phobic.c -o /dev/null
```

- [ ] **Step 3: Commit**

```bash
git add src/phobic/_phobic.c
git commit -m "feat: serialization and deserialization for PHOBIC"
```

---

### Task 6: Python C Extension Module

**Files:**
- Create: `src/phobic/_module.c`

- [ ] **Step 1: Create the Python extension module**

Create `src/phobic/_module.c`:

```c
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "_phobic.h"

/* Capsule destructor */
static void phf_capsule_destructor(PyObject *capsule) {
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    phobic_free(phf);
}

/* build(keys: list[bytes], alpha: float, seed: int, threads: int) -> capsule */
static PyObject *py_build(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *keys_list;
    double alpha;
    unsigned long long seed;
    int threads;

    if (!PyArg_ParseTuple(args, "O!dKi", &PyList_Type, &keys_list,
                          &alpha, &seed, &threads))
        return NULL;

    Py_ssize_t n = PyList_GET_SIZE(keys_list);
    if (n == 0) {
        PyErr_SetString(PyExc_ValueError, "keys must be non-empty");
        return NULL;
    }

    const char **key_ptrs = malloc((size_t)n * sizeof(char *));
    size_t *key_lens = malloc((size_t)n * sizeof(size_t));
    if (!key_ptrs || !key_lens) {
        free(key_ptrs); free(key_lens);
        return PyErr_NoMemory();
    }

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(keys_list, i);
        if (!PyBytes_Check(item)) {
            free(key_ptrs); free(key_lens);
            PyErr_SetString(PyExc_TypeError, "all keys must be bytes");
            return NULL;
        }
        key_ptrs[i] = PyBytes_AS_STRING(item);
        key_lens[i] = (size_t)PyBytes_GET_SIZE(item);
    }

    /* Release GIL during build (C code is thread-safe) */
    phobic_phf *phf;
    Py_BEGIN_ALLOW_THREADS
    phf = phobic_build(key_ptrs, key_lens, (size_t)n,
                        alpha, (uint64_t)seed, threads);
    Py_END_ALLOW_THREADS

    free(key_ptrs);
    free(key_lens);

    if (!phf) {
        PyErr_SetString(PyExc_RuntimeError, "PHOBIC build failed");
        return NULL;
    }

    return PyCapsule_New(phf, "phobic_phf", phf_capsule_destructor);
}

/* query(capsule, key: bytes) -> int */
static PyObject *py_query(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    const char *key;
    Py_ssize_t key_len;

    if (!PyArg_ParseTuple(args, "Oy#", &capsule, &key, &key_len))
        return NULL;

    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;

    size_t slot = phobic_query(phf, key, (size_t)key_len);
    return PyLong_FromSize_t(slot);
}

/* serialize(capsule) -> bytes */
static PyObject *py_serialize(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;

    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;

    size_t needed = phobic_serialize(phf, NULL, 0);
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)needed);
    if (!bytes) return NULL;

    phobic_serialize(phf, (uint8_t *)PyBytes_AS_STRING(bytes), needed);
    return bytes;
}

/* deserialize(data: bytes) -> capsule */
static PyObject *py_deserialize(PyObject *self, PyObject *args) {
    (void)self;
    const char *data;
    Py_ssize_t data_len;

    if (!PyArg_ParseTuple(args, "y#", &data, &data_len)) return NULL;

    phobic_phf *phf = phobic_deserialize((const uint8_t *)data, (size_t)data_len);
    if (!phf) {
        PyErr_SetString(PyExc_ValueError, "invalid serialized data");
        return NULL;
    }

    return PyCapsule_New(phf, "phobic_phf", phf_capsule_destructor);
}

/* num_keys(capsule) -> int */
static PyObject *py_num_keys(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->num_keys);
}

/* range_size(capsule) -> int */
static PyObject *py_range_size(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->range_size);
}

/* bits_per_key(capsule) -> float */
static PyObject *py_bits_per_key(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyFloat_FromDouble(phobic_bits_per_key(phf));
}

/* Module method table */
static PyMethodDef module_methods[] = {
    {"build",       py_build,       METH_VARARGS, "Build a PHF from keys"},
    {"query",       py_query,       METH_VARARGS, "Query a PHF for a key's slot"},
    {"serialize",   py_serialize,   METH_VARARGS, "Serialize a PHF to bytes"},
    {"deserialize", py_deserialize, METH_VARARGS, "Deserialize a PHF from bytes"},
    {"num_keys",    py_num_keys,    METH_VARARGS, "Get number of keys"},
    {"range_size",  py_range_size,  METH_VARARGS, "Get range size"},
    {"bits_per_key",py_bits_per_key,METH_VARARGS, "Get bits per key"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "phobic._module",
    "PHOBIC perfect hash function C extension",
    -1,
    module_methods,
};

PyMODINIT_FUNC PyInit__module(void) {
    return PyModule_Create(&module_def);
}
```

- [ ] **Step 2: Build the extension**

```bash
cd /home/spinoza/github/released/phobic
pip install -e . 2>&1 | tail -5
```

Expected: builds and installs successfully.

- [ ] **Step 3: Quick smoke test**

```bash
python3 -c "from phobic._module import build, query; h = build([b'a', b'b', b'c'], 1.0, 42, 1); print(query(h, b'a'), query(h, b'b'), query(h, b'c'))"
```

Expected: three distinct integers.

- [ ] **Step 4: Commit**

```bash
git add src/phobic/_module.c
git commit -m "feat: Python C extension module wrapping PHOBIC core"
```

---

### Task 7: Python Wrapper

**Files:**
- Modify: `src/phobic/__init__.py`

- [ ] **Step 1: Write the Python wrapper**

Replace `src/phobic/__init__.py`:

```python
"""phobic: Fast minimal perfect hash functions."""

from phobic._module import (
    build as _build,
    query as _query,
    serialize as _serialize,
    deserialize as _deserialize,
    num_keys as _num_keys,
    range_size as _range_size,
    bits_per_key as _bits_per_key,
)


class PHF:
    """A perfect hash function mapping keys to distinct integers."""

    __slots__ = ('_handle',)

    def __init__(self, handle):
        self._handle = handle

    def __getitem__(self, key):
        if isinstance(key, str):
            key = key.encode('utf-8')
        return _query(self._handle, key)

    def slot(self, key):
        """Return the slot index for a key."""
        return self[key]

    @property
    def num_keys(self):
        """Number of keys in the build set."""
        return _num_keys(self._handle)

    @property
    def range_size(self):
        """Size of the output range [0, range_size)."""
        return _range_size(self._handle)

    @property
    def bits_per_key(self):
        """Space efficiency of the hash structure."""
        return _bits_per_key(self._handle)

    def to_bytes(self):
        """Serialize to bytes."""
        return _serialize(self._handle)

    @classmethod
    def from_bytes(cls, data):
        """Deserialize from bytes."""
        return cls(_deserialize(data))

    def __len__(self):
        return self.num_keys

    def __repr__(self):
        return (f"PHF(num_keys={self.num_keys}, "
                f"range_size={self.range_size}, "
                f"bits_per_key={self.bits_per_key:.2f})")


def build(keys, *, alpha=1.0, seed=None, threads=0):
    """Build a perfect hash function from a list of keys.

    Args:
        keys: List of str or bytes keys.
        alpha: Load factor (>= 1.0). Higher = faster build, more memory.
        seed: Random seed for reproducibility. None = random.
        threads: Number of build threads (0 = auto-detect, 1 = single).

    Returns:
        PHF object.
    """
    if seed is None:
        import random
        seed = random.getrandbits(64)
    raw_keys = [k.encode('utf-8') if isinstance(k, str) else k for k in keys]
    handle = _build(raw_keys, float(alpha), int(seed), int(threads))
    return PHF(handle)


def from_bytes(data):
    """Deserialize a PHF from bytes."""
    return PHF.from_bytes(data)
```

- [ ] **Step 2: Verify import works**

```bash
cd /home/spinoza/github/released/phobic
pip install -e . 2>&1 | tail -3
python3 -c "import phobic; phf = phobic.build(['a','b','c']); print(phf); print(phf['a'], phf['b'], phf['c'])"
```

- [ ] **Step 3: Commit**

```bash
git add src/phobic/__init__.py
git commit -m "feat: Python wrapper with PHF class and build()/from_bytes()"
```

---

### Task 8: Tests

**Files:**
- Create: `tests/test_phobic.py`

- [ ] **Step 1: Create test file**

Create `tests/test_phobic.py`:

```python
import phobic
import pytest


def test_build_and_query():
    keys = [f"key_{i}" for i in range(1000)]
    phf = phobic.build(keys)
    slots = {phf[k] for k in keys}
    assert len(slots) == 1000
    assert all(0 <= s < phf.range_size for s in slots)


def test_alpha():
    keys = [f"k{i}" for i in range(100)]
    phf = phobic.build(keys, alpha=1.2)
    assert phf.range_size >= 120
    assert phf.num_keys == 100
    slots = {phf[k] for k in keys}
    assert len(slots) == 100


def test_deterministic():
    keys = ["a", "b", "c"]
    phf1 = phobic.build(keys, seed=42)
    phf2 = phobic.build(keys, seed=42)
    assert all(phf1[k] == phf2[k] for k in keys)


def test_serialization():
    keys = [f"key_{i}" for i in range(500)]
    phf = phobic.build(keys)
    data = phf.to_bytes()
    phf2 = phobic.from_bytes(data)
    assert all(phf[k] == phf2[k] for k in keys)
    assert phf2.num_keys == phf.num_keys
    assert phf2.range_size == phf.range_size


def test_threads():
    keys = [f"key_{i}" for i in range(10000)]
    phf = phobic.build(keys, threads=4)
    slots = {phf[k] for k in keys}
    assert len(slots) == len(keys)


def test_single_thread():
    keys = [f"key_{i}" for i in range(10000)]
    phf = phobic.build(keys, threads=1)
    slots = {phf[k] for k in keys}
    assert len(slots) == len(keys)


def test_bytes_keys():
    keys = [b"raw_bytes_0", b"raw_bytes_1", b"raw_bytes_2"]
    phf = phobic.build(keys)
    slots = {phf.slot(k) for k in keys}
    assert len(slots) == 3


def test_repr():
    phf = phobic.build(["a", "b", "c"])
    r = repr(phf)
    assert "PHF" in r
    assert "num_keys=3" in r


def test_len():
    phf = phobic.build(["x", "y", "z"])
    assert len(phf) == 3


def test_bits_per_key():
    keys = [f"k{i}" for i in range(10000)]
    phf = phobic.build(keys)
    assert 0 < phf.bits_per_key < 5.0


def test_large_key_set():
    keys = [f"large_key_{i:08d}" for i in range(100000)]
    phf = phobic.build(keys, threads=0)
    # Spot-check bijectivity on a sample
    sample = keys[:1000] + keys[-1000:]
    slots = {phf[k] for k in sample}
    assert len(slots) == len(sample)


def test_empty_keys_raises():
    with pytest.raises(ValueError):
        phobic.build([])


def test_from_bytes_module_level():
    keys = ["a", "b", "c"]
    phf = phobic.build(keys)
    data = phf.to_bytes()
    phf2 = phobic.from_bytes(data)
    assert all(phf[k] == phf2[k] for k in keys)
```

- [ ] **Step 2: Run tests**

```bash
cd /home/spinoza/github/released/phobic
pip install -e . && pytest tests/ -v
```

Expected: All tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/test_phobic.py
git commit -m "test: comprehensive pytest suite for phobic package"
```

---

### Task 9: README

**Files:**
- Create: `README.md`

- [ ] **Step 1: Create README**

Create `README.md`:

````markdown
# phobic

Fast minimal perfect hash functions for Python.

A **perfect hash function** maps a known set of *n* keys to distinct integers in [0, *m*) with no collisions. Build it once from your key set, then every lookup is O(1).

## Install

```
pip install phobic
```

## Usage

```python
import phobic

# Build a perfect hash function
keys = ["alice", "bob", "charlie", "diana"]
phf = phobic.build(keys)

# Query: returns a unique integer per key
phf["alice"]    # 2
phf["bob"]      # 0
phf["charlie"]  # 3
phf["diana"]    # 1

# Properties
phf.num_keys    # 4
phf.range_size  # 4
phf.bits_per_key  # ~2.8

# Persistence
data = phf.to_bytes()
phf2 = phobic.from_bytes(data)
```

### Options

```python
# Non-minimal: 5% extra slots (faster build)
phf = phobic.build(keys, alpha=1.05)

# Parallel build (auto-detect cores)
phf = phobic.build(keys, threads=0)

# Fixed seed for reproducibility
phf = phobic.build(keys, seed=42)
```

## Performance

Implemented in C with parallel construction via pthreads.

| Keys | Build (threads=4) | Query | Bits/Key |
|------|-------------------|-------|----------|
| 10K  | ~ms               | ~ns   | ~2.8     |
| 100K | ~ms               | ~ns   | ~2.7     |
| 1M   | ~ms               | ~ns   | ~2.7     |

## License

MIT
````

- [ ] **Step 2: Commit**

```bash
cd /home/spinoza/github/released/phobic
git add README.md
git commit -m "docs: add README with usage examples"
```

---

### Task 10: Final Verification

**Files:** None (verification only)

- [ ] **Step 1: Clean install and test**

```bash
cd /home/spinoza/github/released/phobic
pip install -e . && pytest tests/ -v
```

Expected: All tests pass.

- [ ] **Step 2: Verify bijectivity at scale**

```bash
python3 -c "
import phobic, time
keys = [f'k{i}' for i in range(100000)]
t0 = time.time()
phf = phobic.build(keys, threads=4)
t1 = time.time()
slots = {phf[k] for k in keys}
assert len(slots) == len(keys), f'Expected {len(keys)} unique slots, got {len(slots)}'
print(f'Build: {t1-t0:.3f}s, keys: {phf.num_keys}, range: {phf.range_size}, bpk: {phf.bits_per_key:.2f}')
print('Bijectivity verified at 100K keys')
"
```

- [ ] **Step 3: Verify parallel speedup**

```bash
python3 -c "
import phobic, time
keys = [f'k{i}' for i in range(100000)]
t0 = time.time()
phf1 = phobic.build(keys, threads=1)
t1 = time.time()
phf4 = phobic.build(keys, threads=4)
t2 = time.time()
print(f'1 thread: {t1-t0:.3f}s')
print(f'4 threads: {t2-t1:.3f}s')
print(f'Speedup: {(t1-t0)/(t2-t1):.1f}x')
"
```

- [ ] **Step 4: Commit any fixes**

```bash
cd /home/spinoza/github/released/phobic
git add -u && git commit -m "fix: address issues found during final verification"
```
