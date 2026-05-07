# phobic 0.3.0 design: unified, parallel, minimal-surface PHF

> **Status**: design pinned, awaiting review before C work begins.
> **Audience**: future implementer (Claude or human) executing the rewrite.
> **Predecessors**: `.claude/SESSION_NOTES.md`, `.claude/SWEEP_BUCKET_SIZE.md`, `.claude/cipher-maps-v1a-asks.md`.

## 1. Mission (sharpened)

phobic builds **minimal-ish perfect hash functions for very large key sets, with a parameterised load factor, in reasonable time and space**.

What's in scope:
- `load_factor` (slot occupancy, in `(0, 1]`)
- `bucket_size` (per-shard tuning of build-speed / bits-per-key trade-off)
- Parallelism that makes very-large-N feasible (multi-threaded build, multi-threaded batch query)
- A wire format that survives going to disk and coming back, including from a different process

What's out of scope (lives in `maph` or elsewhere):
- Membership testing, fingerprints, Bloom-style filters
- Value retrieval / Bloomier / cipher-map plumbing
- Approximate / non-perfect modes
- NumPy bulk APIs (deferred until there's a real performance reason; not a 0.3.0 feature)
- Memory-mapped read of on-disk PHFs (deferred to 0.4.0; format chosen to *allow* it later)

## 2. Public API surface

The entire public surface, no hidden helpers.

```python
import phobic

# Build
phf = phobic.build(
    keys, *,
    load_factor=0.5,         # num_keys / range_size, in (0, 1]
    seed=None,               # one knob; internals (shard partition seed, per-shard seeds) are derived
    num_shards=None,         # None = auto (data-driven heuristic; see §7)
    num_threads=None,        # None = min(num_shards, os.cpu_count())
    bucket_size=None,        # None = ceil(log2(per_shard_N)) per shard
    max_retries=100,
) -> PHF

# Deserialize
phf = phobic.from_bytes(blob) -> PHF

# Query
phf[key]                              # scalar (str or bytes); returns int slot
phf.lookup(keys, num_threads=None)    # batch (parallel C path); returns list[int]

# Persist
phf.to_bytes() -> bytes               # wire format v3; one magic, mmap-friendly layout

# Introspect
phf.range_size      # total slot range across all shards
phf.num_shards      # int (always >= 1; single-shard is the small-N degenerate)
phf.bits_per_key    # float; total serialized size in bits / num_keys
len(phf)            # number of keys
repr(phf)           # 'PHF(num_keys=..., range_size=..., num_shards=..., bits_per_key=...)'

# Equality, copy, cross-process transport
phf == other            # via to_bytes() comparison
copy.deepcopy(phf)      # via __reduce__ ↔ to_bytes/from_bytes
# Cross-process transport (e.g. multiprocessing.Pool worker hand-off) works
# via the standard __reduce__ protocol returning (phobic.from_bytes, (blob,)).
# That contract makes the round-trip a documented entry point, not arbitrary
# bytecode: __reduce__ returns a top-level function call, not a __setstate__
# that could execute custom code.

# The only exported type
phobic.PHF              # __all__ = ['PHF', 'build', 'from_bytes']
```

That's seven callables, four properties + `len`, plus equality/repr/copy. Nothing else is public.

### What's deleted vs 0.2.0

| Removed | Why |
|---|---|
| `PartitionedPHF` class | folded into `PHF` |
| `build_partitioned`, `build_partitioned_with_slots` | `build(num_shards=...)` |
| `build_with_slots` | `phf.lookup(keys)` is parallel C now; no longer worth a dedicated function |
| `alpha` parameter | renamed to `load_factor` (canonical hash-table semantics) |
| `strict` / `require_perfect` parameter | always strict; non-perfect "PHF" is a category error |
| `phf.is_perfect` property | always `True` |
| `phf.collisions` property | always `0` |
| `phf.num_keys` property | redundant with `len(phf)` |
| `phf.slot()` method | redundant with `phf[key]` |
| `phf.shards` accessor | implementation detail |
| `BOHP` and `PPHF` magic split | one new magic (`PHF3`) |
| Polymorphic `from_bytes` dispatch | only one type now |

### What survives from 0.2.0

| Kept | Why |
|---|---|
| `phf[key]` scalar | Pythonic; ~10 lines of C; cipher-maps' `evaluate()` is one query per cipher input |
| `phf.lookup(keys)` batch | now parallel C; the workhorse path |
| `phf.to_bytes()` / `phobic.from_bytes()` | core persistence story |
| `seed` reproducibility | one knob now (was two) |
| `bucket_size` knob | the build-speed / bits-per-key dial (proven by SWEEP) |
| `max_retries` knob | retries with seed and load-factor variation |
| GIL release during build | load-bearing for parallel callers |

## 3. C struct (phobic_phf)

```c
typedef struct {
    /* global */
    size_t   num_keys;       /* sum over shards */
    size_t   total_range;    /* sum of shard_range[]; == phf.range_size */
    size_t   num_shards;     /* >= 1 */
    uint64_t seed;           /* the user-provided seed; internals derived */
    uint64_t shard_seed;     /* derived from seed; partitions keys to shards */

    /* per-shard arrays, length = num_shards */
    uint64_t  *shard_seeds;       /* derived from seed; per-shard hash seed */
    size_t    *shard_range;       /* slots per shard */
    size_t    *shard_offsets;     /* prefix sum: offsets[s] = sum(range[0..s]); offsets[num_shards] not stored */
    size_t    *shard_num_buckets; /* per shard */
    size_t    *shard_bucket_size; /* per shard */
    uint16_t **shard_pilots;      /* shard_pilots[s] = uint16[num_buckets[s]] */
} phobic_phf;
```

For `num_shards == 1`, every per-shard array has length 1. The query path is uniform; no special-case branch.

## 4. Build options struct

```c
typedef struct {
    double   load_factor;    /* in (0, 1]; range_size_per_shard = ceil(N_shard / load_factor) */
    uint64_t seed;
    int      max_retries;    /* per-shard; default 100 */
    size_t   bucket_size;    /* 0 = auto (ceil(log2(N_shard))) */
    size_t   num_shards;     /* 0 = auto */
    int      num_threads;    /* 0 = auto */
} phobic_build_opts;

phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                         size_t num_keys, const phobic_build_opts *opts);

/* Diagnostic on failure: filled in by phobic_build when it returns NULL.
 * Caller's job to allocate and zero before the call. */
typedef struct {
    int    failed_shard;          /* which shard couldn't build; -1 if other failure */
    size_t best_collisions;       /* best collision count seen on the failing shard */
    double resolved_load_factor;  /* effective load_factor after retry bumps */
    size_t resolved_bucket_size;  /* effective bucket_size for the failing shard */
} phobic_build_diag;

phobic_phf *phobic_build_with_diag(const char **keys, const size_t *key_lens,
                                    size_t num_keys,
                                    const phobic_build_opts *opts,
                                    phobic_build_diag *diag /* may be NULL */);
```

`phobic_build` is a thin wrapper around `phobic_build_with_diag(..., NULL)`. The diagnostic fields feed the rich error message in §6.

## 5. Wire format v3

Single magic, mmap-friendly section ordering.

```
+--------+------------------------------------------------------------+
| offset | content                                                    |
+--------+------------------------------------------------------------+
|   0    | magic:        4 bytes  = b"PHF3"                           |
|   4    | version:      uint32   = 3                                 |
|   8    | num_keys:     uint64                                       |
|  16    | total_range:  uint64                                       |
|  24    | num_shards:   uint64                                       |
|  32    | seed:         uint64    (user-provided seed, for reproducibility)
|  40    | shard_seed:   uint64    (derived; lets shard partition be re-derived)
|  48    | reserved:     uint64    (zeros; future use)
|  56    | --- end of global header (56 bytes) ---                    |
+--------+------------------------------------------------------------+
|  56    | shard_descriptors: num_shards × shard_descriptor (40 bytes)|
|  56+40s| shard_descriptor[s]:                                       |
|  +0    |   shard_seed_s:  uint64                                    |
|  +8    |   bucket_size_s: uint64                                    |
|  +16   |   num_buckets_s: uint64                                    |
|  +24   |   range_s:       uint64                                    |
|  +32   |   pilots_offset: uint64    (absolute offset into the blob) |
+--------+------------------------------------------------------------+
| pilots_offset[s] | pilots_s: num_buckets_s × uint16                 |
|   ...            | (8-byte aligned start; padding bytes if needed)  |
+--------+------------------------------------------------------------+
```

All integers little-endian (matches the existing format and most modern hardware).

Properties:
- **Mmap-friendly**: shard_descriptors are a fixed-size array, indexable in O(1). Each `pilots_offset` is absolute, so reading `phobic_query` from a memory map needs zero parsing of variable-size sections.
- **Self-validating**: `pilots_offset[s]` must be 8-byte aligned and within `[56 + 40*num_shards, blob_size - num_buckets_s*2]`. Deserialiser checks all.
- **No collisions field**: builds are always perfect.
- **Backward compat**: none. `BOHP` (0.1.x, 0.2.x PHF) and `PPHF` (0.2.x partitioned) blobs are *not readable* by 0.3.0. Aggressive cut, consistent with pre-1.0 status.

The 8-byte alignment and absolute offsets cost a few padding bytes per blob; at 100M keys with ~14 bytes per key in pilots, this is negligible (<0.0001%).

## 6. Threading model

### 6a. Build

Two levels of concurrency:

1. **Per-shard**: each shard's pilot search is independent. Default: spawn `num_threads` worker pthreads, each takes shards from a work queue. C-level fork-join inside `phobic_build`.
2. **Within-shard (deferred to 0.4.0)**: SIMD inner loop over pilot candidates. Not in 0.3.0.

The Python wrapper releases the GIL for the entire build so concurrent callers (e.g., a webserver building PHFs on demand) don't block each other.

### 6b. Batch query

Implementation, in `_phobic.c`:

```c
void phobic_query_batch(const phobic_phf *phf,
                        const char **keys, const size_t *key_lens, size_t n,
                        size_t *out_slots,
                        int num_threads /* 0 = serial fast path */);
```

Strategy:
- For `n < BATCH_THREADING_THRESHOLD` (~10K) or `num_threads <= 1`: serial loop. The pthread fork-join overhead exceeds the gain at small batches.
- For `n >= threshold` and `num_threads > 1`: chunk `[0, n)` into `num_threads` disjoint slices; each thread fills its slice of `out_slots`. No synchronization (outputs disjoint, inputs read-only).

Why chunk-by-index instead of group-by-shard:
- Chunk-by-index is the simplest correct algorithm.
- Group-by-shard improves cache locality (each thread touches only one shard's pilots), but requires an O(n) bucketing pass first. Adopt only if profiling shows the locality wins exceed the bucketing cost.

The Python wrapper:
1. Encode str keys to bytes (Python loop, under GIL)
2. Copy keys into a flat C buffer + lens array (under GIL; this is the safety contract for GIL-released worker threads)
3. `Py_BEGIN_ALLOW_THREADS`
4. `phobic_query_batch(...)` runs pthread fork-join
5. `Py_END_ALLOW_THREADS`
6. Convert C slot array to Python list (under GIL)

The flat-copy step is O(n) memory but enables real parallelism. For batches large enough to want threading, the copy is amortised by the parallel work.

## 7. Seed derivation

One user-visible `seed`. Internally derived via splitmix64 finalizer:

```c
static inline uint64_t splitmix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Inside phobic_build */
uint64_t shard_seed = splitmix64(seed ^ 0xA5A5A5A5A5A5A5A5ULL);
for (size_t s = 0; s < num_shards; s++) {
    phf->shard_seeds[s] = splitmix64(seed ^ ((uint64_t)s * 0xC2B2AE3D27D4EB4FULL));
}
```

Properties:
- Same `seed` ⇒ same shard partition ⇒ same per-shard seeds ⇒ identical PHF byte-for-byte
- Independent dimensions: bumping `s` doesn't reveal information about adjacent shards' seeds
- Not cryptographic; phobic explicitly does not promise crypto-grade unpredictability of seeds (cipher-maps owns that layer)

On retry (build attempt `a`): replace `seed` with `splitmix64(seed ^ a * 0xD2B74407B1CE6E93ULL)` and re-run the failing shard. Each attempt gets an independent seed without repeating prior attempts.

## 8. Auto num_shards heuristic (proposed; benchmarked in phase 5)

Goals:
- Single shard at small N (matches the old serial PHF in space efficiency)
- Enough shards at large N to keep per-shard pilot search tractable (`bucket_size = ceil(log2(N_shard))` ≤ ~14)
- Don't oversubscribe threads more than ~4×

Proposed formula:

```c
size_t auto_num_shards(size_t num_keys, int num_threads) {
    /* Below ~32K, single shard is fine: bucket_size = ceil(log2(32K)) = 15
     * is solvable, build is fast. */
    if (num_keys < 32768) return 1;

    /* Target shard size of ~16K-32K keys: pilot search stays tractable,
     * per-shard header overhead amortises over enough keys. */
    size_t shards_by_size = (num_keys + 15999) / 16000;

    /* Cap at ~4× thread count: more shards just adds metadata. */
    int t = num_threads > 0 ? num_threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (t < 1) t = 1;
    size_t shards_by_threads = (size_t)t * 4;

    return shards_by_size < shards_by_threads ? shards_by_size : shards_by_threads;
}
```

Examples on an 8-core machine (`num_threads=8`):

| N | shards_by_size | shards_by_threads | result |
|---|---|---|---|
| 10K | 1 | 32 | 1 |
| 100K | 7 | 32 | 7 |
| 1M | 63 | 32 | 32 |
| 10M | 625 | 32 | 32 |
| 100M | 6250 | 32 | 32 |

This is a starting point. **Phase 5 will benchmark and tune.** The constants `16000` and `4` may move based on actual numbers.

## 9. Migration story

For 0.2.x → 0.3.0:

| 0.2.x call | 0.3.0 call |
|---|---|
| `phobic.build(keys, alpha=0.05)` | `phobic.build(keys, load_factor=1.0/(1+0.05))` ≈ `load_factor=0.95` |
| `phobic.build(keys, alpha=1.0)` | `phobic.build(keys)` (default `load_factor=0.5`) |
| `phobic.build(keys, strict=False)` | not supported; must adjust `load_factor` / `max_retries` and let it raise on failure |
| `phobic.build_partitioned(keys, ...)` | `phobic.build(keys, num_shards=...)` |
| `phobic.build_with_slots(keys)` | `phf = phobic.build(keys); slots = phf.lookup(keys)` (now parallel C) |
| `phobic.build_partitioned_with_slots(keys, ...)` | same as above |
| 0.2.x `BOHP`/`PPHF` blobs | not readable; rebuild |
| `phf.is_perfect` | always `True`; remove the check |
| `phf.collisions` | always `0`; remove the check |
| `phf.num_keys` | `len(phf)` |

A short MIGRATION.md ships with the 0.3.0 release.

## 10. Test strategy

Existing tests (73 fast + 3 slow in 0.2.0):
- Tests of `PartitionedPHF` API (test_partitioned.py): rewrite to test the unified `PHF` with explicit `num_shards`
- Tests of `strict=False`: delete (mode removed)
- Tests of `is_perfect` / `collisions` properties: delete (always-true now; one assert that build raises on impossible config)
- Tests of `alpha`: rewrite as `load_factor` tests
- Tests of `build_with_slots`, `build_partitioned`, `build_partitioned_with_slots`: delete (functions removed)
- Tests of polymorphic `from_bytes`: simplified (one type now)

New tests:
- `load_factor` validation: rejects 0, negative, > 1
- `load_factor` semantics: at `load_factor=0.5`, `range_size == 2 * num_keys` (within rounding)
- `load_factor=1.0` MPHF: builds (with retries) or raises a *useful* error
- `num_shards=1` produces a valid PHF with single-shard wire format
- `num_shards=N` produces N shards
- Auto num_shards: small N → 1 shard, large N → many shards
- Parallel batch query correctness: results match scalar query for arbitrary thread counts
- Parallel batch query thread safety: same PHF queried concurrently from multiple Python threads
- `__reduce__` round-trip (covers cross-process transport and `copy.deepcopy`)
- `seed` determinism: same seed ⇒ identical `to_bytes()` (byte-for-byte)
- Wire format v3 round-trip preserves shard structure
- Wire format alignment: `pilots_offset` is 8-byte aligned
- Failure error message includes diagnostic fields (shard, best_collisions, resolved_*)

Estimated test count post-rewrite: ~80 fast tests + 3 slow.

## 11. Implementation order (phases 3–6)

**Phase 3a: C foundations** (touches `_phobic.h`, `_phobic.c`, `_module.c`)
- Replace struct, replace `phobic_build` signature with options struct
- Implement seed derivation, auto-shard heuristic (initial constants from §8)
- Implement single-threaded multi-shard build (correctness first; threading next)
- Implement single-threaded `phobic_query` against new struct
- Implement single-threaded `phobic_query_batch` against new struct
- v3 serialize / deserialize
- Smoke test: pure-C round-trip works (no Python yet)

**Phase 3b: C threading**
- pthread worker pool for build (per-shard parallelism)
- pthread fan-out for `phobic_query_batch` (above threshold)
- Verify on `pthread`-less platforms (macOS supports it; Windows uses pthread-w32 or skip multi-threaded build there)

**Phase 4: Python wrapper rewrite**
- Delete `partitioned.py`
- Rewrite `__init__.py`: one `PHF`, one `build`, one `from_bytes`, `__reduce__`
- Rewrite `_module.c` to call new C API, parse new options struct from kwargs
- All Python-side validation: `load_factor` in `(0, 1]`, etc.

**Phase 5: Benchmark + tune**
- Sweep N ∈ {1K, 100K, 1M, 10M, 100M}
- For each: measure build wall-time (1, 2, 4, 8 threads), bits/key, query latency, parallel-batch throughput
- Compare against maph's `partitioned_phf<phobic4>` numbers
- Tune `auto_num_shards` constants based on the data
- Save results to `.claude/SWEEP_0_3_0.md`

**Phase 6: Release**
- Bump to 0.3.0
- README rewrite for the unified API
- MIGRATION.md (0.2.x → 0.3.0)
- CLAUDE.md refresh
- Build, smoke-test, TestPyPI surrogate (local sdist install in fresh venv), then PyPI
- Tag, push, GitHub release with full changelog

## 12. Open questions to settle during implementation

These don't block design, but flag them so they're explicit:

1. **`__eq__` semantics**: byte-for-byte via `to_bytes()` comparison? Or structural (same num_keys, same range, same query results on a sample)? Lean toward byte-for-byte for simplicity; revisit if it surprises users.
2. **Empty-shard handling**: in 0.2.0, empty shards got a single-key dummy PHF. In 0.3.0 with multi-shard build inside one C call, we can do better: just allocate a 0-bucket pilots array and skip the build. Verify this in the C path.
3. **Windows / pthread**: ship a single-threaded-on-Windows fallback for 0.3.0, or use `c11/threads.h`? `<threads.h>` would be cleanest but MSVC support is patchy. Pragmatic: detect `_WIN32`, fall back to single-threaded build there. Add `__cdecl`-compatible thread shim in 0.3.1 if anyone asks.
4. **Wheel building for PyPI**: 0.2.0 was sdist-only. Should 0.3.0 also ship Linux/macOS wheels via cibuildwheel? Probably yes, but treat as orthogonal release engineering, not a 0.3.0 feature.

## 13. References

- 0.2.0 commit: `b4e8bc1` (this is the implementation baseline; 0.3.0 starts here)
- `.claude/SESSION_NOTES.md`: historical context (load-bearing invariants like GIL release)
- `.claude/SWEEP_BUCKET_SIZE.md`: measured trade-offs from 0.2.0
- `.claude/cipher-maps-v1a-asks.md`: original consumer asks (subsumed by 0.3.0; preserved for context)
- maph repo (`~/github/repos/maph/docs/BENCHMARK_RESULTS.md`): the comparison target
- Lehmann & Walzer, "PHOBIC: Perfect Hashing with Optimised Bucket sizes and Interleaved Coding" (2024): the published algorithm phobic is named after. The bucket-assignment skewing they describe is *not* in 0.3.0 but is on the long-term roadmap.
