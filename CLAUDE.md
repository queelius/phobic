# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

**phobic** is a Python package providing PHOBIC (pilot-based) minimal perfect hash functions. Core algorithm is C11, single-threaded per build. No runtime dependencies.

A perfect hash function maps a known set of *n* keys to distinct integers in `[0, m)` with zero collisions. Build once, query O(1).

Scope is deliberately narrow: pure PHF only. Membership testing, value retrieval, fingerprinting, and cipher maps are explicitly out of scope and live in the sibling `maph` research repo. If a request adds filtering / retrieval / Bloomier-style behavior, push back or build it as a sibling package.

## Build & Development

```bash
# Install in development mode (compiles C extension)
pip install -e .

# Run fast tests (default; excludes scale tests via pyproject.toml addopts)
pytest

# Run scale tests (1M and 10M keys; ~5 min for 1M)
pytest -m slow

# Run a single test
pytest tests/test_phobic.py::test_build_and_query

# Run benchmarks
pytest tests/bench_phobic.py -v

# Rebuild after C changes (Python-only changes don't need this)
pip install -e .
```

The C extension must be recompiled after any change to `_phobic.c`, `_phobic.h`, or `_module.c`. There is no separate `make` step; `pip install -e .` handles compilation via `setup.py`.

`pyproject.toml` registers a `slow` marker and sets `addopts = "-m 'not slow'"`, so `pytest` alone skips scale tests. This is a footgun when verifying "everything passes". Opt in with `-m slow` or `-m ''` to run them.

## Architecture

Four-layer design: **C core** -> **C extension glue** -> **Python wrapper** -> **Python sharding layer**.

### C Core (`src/phobic/_phobic.c` + `_phobic.h`)

Pure C11, no Python dependency. The `phobic_phf` struct stores:
- `uint16_t *pilots`: one pilot value per bucket (the entire hash structure)
- Metadata: `num_keys`, `range_size`, `num_buckets`, `bucket_size`, `seed`, `collisions`

**Build algorithm**: Keys are hashed (wyhash-style dual hash), assigned to buckets via `h1`, buckets sorted by descending size, then a brute-force pilot search finds a pilot per bucket such that `slot_with_pilot(h2, pilot, range_size)` produces no collisions against the global occupied bitset.

**Bucket-size invariant**: default `bucket_size = ceil(log2(N))` (resolved once at the top of `phobic_build`, not in the retry loop). This grows with N, making each bucket's pilot search exponentially harder as N rises. Above ~1M keys, the default parameters can take ~100 s; above ~2M, serial build often fails entirely. This is the structural reason `PartitionedPHF` exists: sharding caps per-shard bucket_size around 14, which always solves.

`bucket_size` is overridable via the `bucket_size` build parameter (0 to the C call = auto). See `.claude/SWEEP_BUCKET_SIZE.md` for measured trade-offs at N in [1K, 10K, 100K]: at 100K keys, `bucket_size=8` builds ~6x faster than the auto default for ~2x bits/key, which is usually the right pick for callers that build many PHFs (e.g. cipher-maps).

**Retry logic**: Up to `max_retries` attempts (default 100) with seed variation; every 10 failures bumps alpha by 0.005 to increase range_size headroom.

**Strict vs. non-strict mode**: `strict=1` returns NULL immediately on any unsolvable bucket. `strict=0` falls back to pilot 0 for unsolvable buckets and counts the affected keys as collisions. The outer loop tracks the best result (fewest collisions) across all attempts.

**Serialization**: Binary format with magic bytes `0x50484F42` ("PHOB"), version 2 header (56 bytes): magic(4) + version(4) + num_keys(8) + range_size(8) + num_buckets(8) + bucket_size(8) + seed(8) + collisions(8), then raw pilot array (uint16 per bucket).

### C Extension (`src/phobic/_module.c`)

Thin Python-to-C glue. The `phobic_phf*` is wrapped in a `PyCapsule` with a destructor calling `phobic_free()`. Exposes 9 module-level functions: `build`, `query`, `query_batch`, `serialize`, `deserialize`, `num_keys`, `range_size`, `bits_per_key`, `collisions`.

**GIL release invariant**: `Py_BEGIN_ALLOW_THREADS` brackets the call to `phobic_build` (`_module.c:70`). This single line is load-bearing: it's what lets `ThreadPoolExecutor` deliver real CPU parallelism in `build_partitioned` without multiprocessing IPC. Do not "simplify" this away; if you change build to need Python state mid-loop, the partitioned path silently degrades to serial.

`query_batch` runs synchronously under the GIL (no Py_BEGIN_ALLOW_THREADS); its win is amortizing per-call PyObject churn over the batch, not parallelism. Roughly 2-3x scalar query speed at 100K keys.

### Python Wrapper (`src/phobic/__init__.py`)

`PHF` class wraps the capsule handle. `build()` encodes str keys to UTF-8 bytes before passing to C. `from_bytes()` is available both as `PHF.from_bytes()` and module-level `phobic.from_bytes()`. `PHF.collisions` and `PHF.is_perfect` expose collision metadata. `PHF.lookup(keys)` is the batch query path, returning a list of slots parallel to the input.

`build_with_slots(keys)` returns `(PHF, list[int])` where `slots[i] == phf[keys[i]]`. Useful for callers (e.g. cipher_maps' `PHFCipherMap.__init__`) that immediately fill a slot array right after construction. Saves the redundant Python-side encode pass plus K calls of per-call overhead vs. running `build()` and then `phf.lookup(keys)` separately. Validation lives in `_prepare_build()`; both entry points share it so `seed`, `alpha`, and uniqueness checks stay consistent.

### Python Sharding (`src/phobic/partitioned.py`)

`PartitionedPHF` and `build_partitioned` provide a sharded build for scale. Each shard is an independent `phobic.PHF` built on a disjoint subset of keys. At query time, a deterministic 64-bit FNV-1a mix selects the shard; the slot is `prefix_sum_offsets[shard] + shard_phf[key]`.

Build parallelism comes from `concurrent.futures.ThreadPoolExecutor` over the GIL-releasing C build. Default `target_shard_size = 15_000` keeps per-shard `bucket_size` around 14. Measured speedup at 1M keys on 8 cores: ~67x (133 s serial -> 2 s partitioned). At 2M+ keys, serial often fails; partitioned still builds.

The shard-selector hash is intentionally independent of phobic's internal hash. Keep it that way so the shard index is stable across any future change to phobic's hashing.

**Wire format**: magic `b"PPHF"`, little-endian: u32 version, u64 num_shards, u64 shard_seed, then for each shard: u64 size + raw `PHF.to_bytes()` payload. Empty shards still get a placeholder PHF (built from a single dummy key) but contribute zero to the offset table; they preserve shard indexing without consuming range.

`PartitionedPHF.lookup(keys)` groups keys by their target shard, calls each shard's batch query once, and assembles results in input order, substantially faster than scalar lookup for large batches.

## Key Design Decisions

- **Pilot values are `uint16_t`**: max 65535 pilot candidates per bucket. Caps space at ~2 bytes/bucket and bounds inner-loop work.
- **`alpha` parameter**: controls `range_size = ceil(n * (1 + alpha))`. Default 1.0 = minimal PHF in the loose sense. Higher = faster build at the cost of more slots.
- **`strict=False`**: returns the best approximation found across all retry attempts. Useful when a perfect build is too expensive. `PHF.collisions` tells you how many keys were placed incorrectly.
- **No membership test**: pure PHF. Querying a key not in the build set returns an arbitrary slot, not an error. Membership verification (fingerprinting) is explicitly a non-goal. That lives in `maph`.
- **Sharded builds trade ~0.4 bits/key for parallelism**: serial is ~0.80 bpk at 1M; partitioned is ~1.18 bpk at 10M because each shard carries its own 56-byte header plus a partition wrapper.

## Test Layout

- `tests/test_phobic.py`: core PHF API, including batch lookup and serialization edge cases.
- `tests/test_partitioned.py`: 11 cases covering build/query, shard counts, single-shard edge case, serialization round-trip, determinism, str+bytes keys, empty/duplicate rejection, shard balance, bits/key reasonableness.
- `tests/test_distributions.py`: builds at 10K across random, sequential, url, and variable-length keys. Catches regressions where a future hash change introduces structural bias.
- `tests/test_scale.py`: builds at 1M and 10M, marked `@pytest.mark.slow`. Excluded by default.
- `tests/bench_phobic.py`: pytest-benchmark suite covering serial build, batch lookup, partitioned scaling.

When adding a regression test for a bug fix, prefer extending an existing file over creating a new one unless the new behavior warrants its own grouping (as `test_partitioned.py` does for the sharding layer).

## Compiler Flags

Defined in `setup.py`: `-O2 -std=c11 -Wall -Wextra`. Uses `__uint128_t` for wyhash mixing (GCC/Clang, not MSVC).

## Session Continuity

`.claude/SESSION_NOTES.md` carries narrative context from past sessions: what was added, what was deliberately *not* done and why, and follow-ups still worth doing (NumPy interop, streaming serialize, smarter shard count, type stubs, CI matrix). Read it before starting non-trivial work to avoid relitigating decided questions.
