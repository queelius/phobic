# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

**phobic** is a Python package providing PHOBIC (pilot-based) minimal perfect hash functions. Core algorithm is C11, single-threaded. No runtime dependencies.

A perfect hash function maps a known set of *n* keys to distinct integers in `[0, m)` with zero collisions. Build once, query O(1).

## Build & Development

```bash
# Install in development mode (compiles C extension)
pip install -e .

# Run tests
pytest

# Run a single test
pytest tests/test_phobic.py::test_build_and_query

# Rebuild after C changes (required; Python-only changes don't need this)
pip install -e .
```

The C extension must be recompiled after any change to `_phobic.c`, `_phobic.h`, or `_module.c`. There is no separate `make` step; `pip install -e .` handles compilation via `setup.py`.

## Architecture

Three-layer design: **C core** -> **C extension glue** -> **Python wrapper**.

### C Core (`src/phobic/_phobic.c` + `_phobic.h`)

Pure C11, no Python dependency. The `phobic_phf` struct stores:
- `uint16_t *pilots`: one pilot value per bucket (the entire hash structure)
- Metadata: `num_keys`, `range_size`, `num_buckets`, `bucket_size`, `seed`, `collisions`

**Build algorithm**: Keys are hashed (wyhash-style dual hash), assigned to buckets via `h1`, buckets sorted by descending size, then a brute-force pilot search finds a pilot per bucket such that `slot_with_pilot(h2, pilot, range_size)` produces no collisions against the global occupied bitset.

**Retry logic**: Up to `max_retries` attempts (default 100) with seed variation; every 10 failures bumps alpha by 0.005 to increase range_size headroom.

**Strict vs. non-strict mode**: `strict=1` returns NULL immediately on any unsolvable bucket. `strict=0` falls back to pilot 0 for unsolvable buckets and counts the affected keys as collisions. The outer loop tracks the best result (fewest collisions) across all attempts.

**Serialization**: Binary format with magic bytes `0x50484F42` ("PHOB"), version 2 header (56 bytes): magic(4) + version(4) + num_keys(8) + range_size(8) + num_buckets(8) + bucket_size(8) + seed(8) + collisions(8), then raw pilot array (uint16 per bucket).

### C Extension (`src/phobic/_module.c`)

Thin Python-to-C glue. The `phobic_phf*` is wrapped in a `PyCapsule` with a destructor calling `phobic_free()`. Exposes 8 module-level functions: `build`, `query`, `serialize`, `deserialize`, `num_keys`, `range_size`, `bits_per_key`, `collisions`. GIL is released during `phobic_build` via `Py_BEGIN_ALLOW_THREADS`.

### Python Wrapper (`src/phobic/__init__.py`)

`PHF` class wraps the capsule handle. `build()` encodes str keys to UTF-8 bytes before passing to C. `from_bytes()` is available both as `PHF.from_bytes()` and module-level `phobic.from_bytes()`. `PHF.collisions` and `PHF.is_perfect` expose collision metadata.

## Key Design Decisions

- **Pilot values are `uint16_t`**: Max 65535 pilot candidates per bucket. This caps space at ~2 bytes/bucket.
- **`alpha` parameter**: Controls `range_size = ceil(n * (1 + alpha))`. Default 1.0 = minimal PHF. Higher = faster build at the cost of more slots.
- **`strict=False`**: Returns the best approximation found across all retry attempts. Useful when a perfect build is too expensive. `PHF.collisions` tells you how many keys were placed incorrectly.
- **No membership test**: This is a pure PHF. Querying a key not in the build set returns an arbitrary slot, not an error. Membership verification (fingerprinting) is explicitly a non-goal.

## Compiler Flags

Defined in `setup.py`: `-O2 -std=c11 -Wall -Wextra`. Uses `__uint128_t` for wyhash mixing (GCC/Clang, not MSVC).
