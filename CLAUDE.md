# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

**phobic** builds minimal-ish perfect hash functions for very large key sets, with a parameterised load factor. C11 core with pthread parallelism for both build and batch query. No runtime dependencies beyond CPython.

A perfect hash function maps a known set of *n* keys to distinct integers in `[0, m)` with zero collisions. Build once, query O(1).

Scope is deliberately narrow:

- **In scope**: very-large-N builds, parameterised `load_factor`, parallel construction, parallel batch query, a portable mmap-friendly wire format.
- **Out of scope** (lives elsewhere): membership testing, fingerprints, Bloom filters, value retrieval, Bloomier maps, cipher-map plumbing, approximate / non-perfect modes. If a request adds any of those, push back or build it as a sibling package. The sibling research repo is `maph`.

## Build & Development

```bash
# Install in development mode (compiles C extension, links pthread)
pip install -e .

# Run fast tests (default; excludes scale tests via pyproject.toml addopts)
pytest

# Run scale tests (1M and 10M keys; ~30s for 10M parallel)
pytest -m slow

# Run a single test
pytest tests/test_phobic.py::test_basic_build_and_scalar_query

# Rebuild after C changes (Python-only changes don't need this)
pip install -e .
```

The C extension must be recompiled after any change to `_phobic.c`, `_phobic.h`, or `_module.c`. There is no separate `make` step; `pip install -e .` handles compilation via `setup.py`.

`pyproject.toml` registers a `slow` marker and sets `addopts = "-m 'not slow'"`, so `pytest` alone skips scale tests. This is a footgun when verifying "everything passes". Opt in with `-m slow` or `-m ''` to run them.

## Architecture

Three layers: **C core** (multi-shard, pthread) -> **C extension glue** -> **Python wrapper** (single class, single builder, single deserialiser).

There is exactly one type (`PHF`), one build function (`build`), one deserialiser (`from_bytes`), one wire format (magic `b"PHF3"`). The pre-0.3.0 design had a separate `PartitionedPHF` and a Python-level `partitioned.py` orchestrator; both were folded into the C core in 0.3.0.

### C Core (`src/phobic/_phobic.c` + `_phobic.h`)

The `phobic_phf` struct is shard-aware. Per-shard parallel arrays hold pilots, ranges, num_buckets, bucket_sizes, and seeds. Single-shard is the small-N degenerate (one extra `+0` indirection per query; no special-case branch).

**Seed derivation**: one user-visible `seed`. Internally derived via splitmix64:

- `shard_seed = splitmix64(seed ^ 0xA5A5A5A5...)` partitions keys to shards.
- `shard_seeds[s] = splitmix64(seed ^ s * 0xC2B2AE3D... ^ ...)` per-shard pilot seeds.
- On retry attempt `a`, the shard's effective seed is `splitmix64(base_seed ^ a * 0xD2B7...)`.

Same `seed` produces byte-identical `to_bytes()` output regardless of `num_threads`. Shard membership is independent of the per-shard pilot search seed, so a shard can re-run with new seeds without changing which keys belong to it.

**Build algorithm (per shard)**: keys are hashed (wyhash-style dual hash), assigned to buckets by `h1 % num_buckets`, buckets sorted by descending size, brute-force pilot search finds a `uint16_t` pilot per bucket such that `slot_with_pilot(h2, pilot, range_size)` produces no collisions against a global occupied bitset. Always strict: a shard either succeeds or returns NULL with a diagnostic.

**Auto sharding**: `auto_num_shards(num_keys, num_threads)` picks `num_shards`. Currently:
- N < 32K: single shard
- N >= 32K: `ceil(N / 16000)` shards (one shard's `bucket_size = ceil(log2(per_shard_N))` stays ≤ ~14)

`num_threads` is accepted but deliberately ignored: `num_shards` is a pure function of `N`, so the same `seed` yields byte-identical output regardless of CPU count. There is no thread-based cap.

See `.claude/SWEEP_BUCKET_SIZE.md` (0.2.0 era) and `.claude/SWEEP_0_3_0.md` (current) for measured trade-offs.

**Threading model**: `phobic_build_with_diag` allocates per-shard buffers, distributes keys, then dispatches per-shard build either to a pthread work queue (when `num_threads > 1` and `PHOBIC_HAVE_PTHREAD`) or to a serial loop. After join, results are checked for the first failure; a diagnostic is filled into the optional `phobic_build_diag` struct.

`phobic_query_batch` chunks the input range across `num_threads` workers when `n >= PHOBIC_BATCH_THREADING_THRESHOLD` (1024 today). Below the threshold, serial. Output slots are disjoint between workers, so no synchronisation is needed.

The pthread code is gated by `#ifndef _WIN32`. Windows currently falls back to single-threaded build and serial batch query. A pthread-w32 (or `<threads.h>`) port would lift that.

**Wire format v3** (`b"PHF3"`): single magic, mmap-friendly layout.
- 56-byte global header (num_keys, total_range, num_shards, seed, shard_seed)
- `40 * num_shards` bytes of fixed-size descriptor records (per-shard seed, bucket_size, num_buckets, range, absolute pilots offset)
- 8-byte aligned variable-size pilot blocks (`uint16` per bucket)

Absolute `pilots_offset` per descriptor lets a future `phobic.from_file(path)` mmap the blob and access pilots without parsing variable-size sections. The format pre-pays the alignment cost (~24 bytes total) for that future feature.

No backward read compat: 0.2.x `BOHP` and `PPHF` blobs are not readable. This is documented in `MIGRATION.md`.

### C Extension (`src/phobic/_module.c`)

Thin Python-to-C glue. `phobic_phf*` lives in a `PyCapsule` whose destructor calls `phobic_free`. Exposes 9 module-level functions: `build`, `query`, `query_batch`, `serialize`, `deserialize`, `num_keys`, `range_size`, `num_shards`, `bits_per_key`.

**GIL release invariant**: `Py_BEGIN_ALLOW_THREADS` brackets `phobic_build_with_diag` in `py_build` and `phobic_query_batch` in `py_query_batch`. Under the released GIL the C workers read key bytes directly through `PyBytes_AS_STRING` pointers. That is safe because `PyBytes` is immutable and the `keys` list (held alive by the call's argument tuple) keeps every key object alive for the whole call. The `key_ptrs` / `key_lens` arrays are populated *before* the GIL is released, since `PyList_GET_ITEM` and the type checks need the GIL. Invariant to preserve: never call the Python C-API under the released GIL. (Pre-0.3.1 also copied every key into a flat C buffer; that copy was removed for speed in 0.3.1 and should not be reintroduced.)

**Error formatting**: `PyErr_Format` does not support `%f` or `%zu`. Build-failure diagnostics use `snprintf` into a 512-byte buffer + `PyErr_SetString`. The diagnostic format is part of the documented user surface (it tells callers how to tune the failing build).

### Python Wrapper (`src/phobic/__init__.py`)

`PHF` class wraps the capsule handle. `build()` encodes str keys to UTF-8 bytes, validates `load_factor` is in `(0, 1]`, validates `seed` / `bucket_size` / `num_shards` / `num_threads` ranges, and forwards to the C `_build`. `from_bytes(data)` normalises bytes-like input to `bytes` (the C deserialiser rejects bytearray/memoryview), then dispatches to `PHF.from_bytes`.

`PHF.__reduce__` returns `(phobic.from_bytes, (self.to_bytes(),))`, which makes the type work with `copy.deepcopy` and `multiprocessing.Pool` worker hand-off. Equality is byte-for-byte via `to_bytes()`.

## Key Design Decisions

- **`load_factor` not `alpha`**: canonical hash-table semantics. `range_size = ceil(num_keys / load_factor)` per shard. Bounded `(0, 1]`. `1.0` = MPHF.
- **Always strict**: a non-perfect "PHF" is a category error. Build raises with diagnostics on failure; users tune and retry.
- **`uint16_t` pilots**: max 65535 candidates per bucket. Caps storage at 2 bytes/bucket. Bucket size ~14 is the empirical "always solvable" line; larger buckets exhaust the pilot space.
- **Per-shard retry**: each shard has its own retry budget (`max_retries`). One failing shard does not invalidate already-built shards in the parallel path; it does fail the whole build (no partial output).
- **Mmap-pre-paid wire format**: absolute pilot offsets, fixed-size descriptors. Costs ~24 bytes per blob; enables `from_file` later without breaking the format.
- **No membership test**: querying a key not in the build set returns an arbitrary valid slot. This is the contract; phobic does not promise to detect strangers.

## Test Layout

- `tests/test_phobic.py` (50+ tests): core API, load_factor semantics + validation, num_shards (auto/explicit), num_threads determinism, bucket_size, seed reproducibility, build-failure diagnostic message, wire format magic + truncation + alignment, equality/copy/`__reduce__`, distribution robustness across 4 key shapes, concurrent thread-safety.
- `tests/test_scale.py` (3 tests, all `@pytest.mark.slow`): builds at 1M and 10M, serialisation round-trip at 1M.

## Compiler Flags

Defined in `setup.py`: `-O2 -std=c11 -Wall -Wextra -pthread`. The `-pthread` flag handles both compilation (defines `_REENTRANT`) and linking. Uses `__uint128_t` for wyhash mixing (GCC/Clang, not MSVC).

## Session Continuity

- `.claude/SESSION_NOTES.md`: pre-0.3.0 narrative.
- `.claude/SWEEP_BUCKET_SIZE.md`: 0.2.0 era trade-offs that informed the auto bucket_size choice.
- `.claude/SWEEP_0_3_0.md`: post-rewrite parallel-build and batch-query measurements.
- `.claude/DESIGN_0_3_0.md`: the 0.3.0 design doc, including the deliberate cuts and the wire format v3 spec. Read this before any non-trivial change.
- `.claude/cipher-maps-v1a-asks.md`: the cipher-maps consumer asks that motivated the 0.3.0 unification (the asks themselves were subsumed by the rewrite, not implemented as discrete additions).
