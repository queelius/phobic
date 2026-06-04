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

## Reference docs

Before reconstructing context from scratch, check whether the answer already lives in one of these:

- **`README.md`**: the user-facing surface. Install steps, the `phobic.build` knob table, performance numbers (12-core machine, 16-byte uniform keys), wire format summary, cross-process transport example, "what phobic explicitly is not". Treat its API examples as authoritative for what users see; this CLAUDE.md explains the *internals* that README deliberately hides.
- **`MIGRATION.md`**: the canonical 0.2.x to 0.3.x delta. Rename table, `alpha` to `load_factor` conversion (`load_factor = 1 / (1 + alpha)`), retired methods (`build_partitioned`, `build_with_slots`, `strict`, `PartitionedPHF`, `phf.is_perfect`, `phf.collisions`, `phf.shards`). When a user mentions any pre-0.3 API, point them at `MIGRATION.md` rather than reconstructing the story.
- **`src/phobic/_phobic.h`**: the C ABI contract. Defines `phobic_phf`, `phobic_build_opts`, `phobic_build_diag`, the function signatures, and the wire format size constants. Read this before changing any C internals; the field order in `phobic_phf` is load-bearing for the per-shard parallel arrays layout.
- **`MANIFEST.in`**: a one-line file that ships `src/phobic/*.h` in the sdist. Without it, sdist installs compile-fail because `_phobic.h` is `#include`d but is not a "source" file from setuptools' perspective. Don't delete.
- **`demo.ipynb`**: predates 0.3.0; uses the 0.2.x `alpha` / `PartitionedPHF` API. Stale; do not treat as a reference. Either rebuild it or ignore.

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

**Auto sharding**: `auto_num_shards(num_keys, num_threads)` picks `num_shards`. The signature retains `num_threads` for ABI continuity, but the body does `(void)num_threads`. The heuristic is purely a function of `N`. This is deliberate: making the choice thread-dependent would break byte-identical `to_bytes()` output across machines with different CPU counts. Do not reintroduce a thread cap here.

Current schedule:
- N < 32K: single shard
- N >= 32K: `ceil(N / 16000)` shards (decimal 16000, not 16384; keeps per-shard `bucket_size = ceil(log2(per_shard_N))` at ~14 or below)

See `.claude/SWEEP_BUCKET_SIZE.md` (0.2.0 era) and `.claude/SWEEP_0_3_0.md` (current) for measured trade-offs.

**Threading model**: `phobic_build_with_diag` allocates per-shard buffers, distributes keys, then dispatches per-shard build either to a pthread work queue (when `num_threads > 1` and `PHOBIC_HAVE_PTHREAD`) or to a serial loop. After join, results are checked for the first failure; a diagnostic is filled into the optional `phobic_build_diag` struct.

`phobic_query_batch` chunks the input range across `num_threads` workers when `n >= PHOBIC_BATCH_THREADING_THRESHOLD` (1024 today). Below the threshold, serial. Output slots are disjoint between workers, so no synchronisation is needed.

The pthread code is gated by `PHOBIC_HAVE_PTHREAD` (defined as `1` on non-Windows and `0` on Windows in `_phobic.c`). On Windows the build falls back to single-threaded and `phobic_query_batch` is serial. A pthread-w32 (or `<threads.h>`) port would lift that.

**Wire format v3** (`b"PHF3"`): single magic, mmap-friendly layout.
- 56-byte global header (num_keys, total_range, num_shards, seed, shard_seed)
- `40 * num_shards` bytes of fixed-size descriptor records (per-shard seed, bucket_size, num_buckets, range, absolute pilots offset)
- 8-byte aligned variable-size pilot blocks (`uint16` per bucket)

Absolute `pilots_offset` per descriptor lets a future `phobic.from_file(path)` mmap the blob and access pilots without parsing variable-size sections. The format pre-pays the alignment cost (~24 bytes total) for that future feature.

No backward read compat: 0.2.x `BOHP` and `PPHF` blobs are not readable. This is documented in `MIGRATION.md`.

### C Extension (`src/phobic/_module.c`)

Thin Python-to-C glue. `phobic_phf*` lives in a `PyCapsule` whose destructor calls `phobic_free`. Exposes 9 module-level functions: `build`, `query`, `query_batch`, `serialize`, `deserialize`, `num_keys`, `range_size`, `num_shards`, `bits_per_key`.

**GIL release invariant**: `Py_BEGIN_ALLOW_THREADS` brackets `phobic_build_with_diag` in `py_build` and `phobic_query_batch` in `py_query_batch`. The safety contract for the released-GIL window relies on three things together: (1) input keys are normalised to `bytes` in Python before the C call, so the held list reference keeps every key alive; (2) `PyBytes` is immutable, so `PyBytes_AS_STRING` returns a stable pointer that workers can read without holding the GIL; (3) C allocates its own `key_ptrs` / `key_lens` arrays from those pointers, never touching the Python list during the parallel section. There is no flat-key-buffer memcpy: that was dropped in 0.3.1 (it was costing ~1.9s at 10M for no safety benefit). If you reintroduce a code path that derives a key buffer outside of "PyBytes held by an input list", re-justify the safety contract before releasing the GIL.

**Error formatting**: `PyErr_Format` does not support `%f` or `%zu`. Build-failure diagnostics use `snprintf` into a 512-byte buffer + `PyErr_SetString`. The diagnostic format is part of the documented user surface (it tells callers how to tune the failing build).

### Python Wrapper (`src/phobic/__init__.py`)

`PHF` class wraps the capsule handle. `build()` encodes str keys to UTF-8 bytes, validates `load_factor` is in `(0, 1]`, validates `seed` / `bucket_size` / `num_shards` / `num_threads` ranges, and forwards to the C `_build`. `from_bytes(data)` normalises bytes-like input to `bytes` (the C deserialiser rejects bytearray/memoryview), then dispatches to `PHF.from_bytes`.

The key normalisation in `build()` deliberately avoids the redundant `bytes(k)` copy when `k` is already `bytes`: the comprehension is `k if isinstance(k, bytes) else k.encode('utf-8') if isinstance(k, str) else bytes(k)`. At 10M keys this saves ~1.9s of unnecessary allocation. Do not "simplify" it back to a uniform `bytes(k)` call.

`assume_unique=False` (default) runs an `O(N) set(raw)` uniqueness check before the C call. At 10M keys that check is ~2s of single-threaded Python. Callers that already know their keys are unique by construction (e.g. cipher-maps, where keys are HMAC-SHA256 outputs) should pass `assume_unique=True`. If duplicates are present anyway, the C build will either fail with a confusing diagnostic or produce a non-PHF; that is the caller's responsibility, documented in the docstring.

`PHF.__reduce__` returns `(phobic.from_bytes, (self.to_bytes(),))`, which makes the type work with `copy.deepcopy` and `multiprocessing.Pool` worker hand-off. Equality is byte-for-byte via `to_bytes()`.

`PHF.__hash__ = None`: PHFs are explicitly unhashable. The 0.3.0 implementation was `hash(self.to_bytes())`, which forced full re-serialisation on every hash call (~10 MB at 10M keys, called silently inside any `set` or `dict` lookup). Setting `__hash__ = None` matches Python's "not naturally hashable" convention and prevents accidental O(N) blow-ups. Users who genuinely need a hash can call `hash(phf.to_bytes())` explicitly.

## Key Design Decisions

- **`load_factor` not `alpha`**: canonical hash-table semantics. `range_size = ceil(num_keys / load_factor)` per shard. Bounded `(0, 1]`. `1.0` = MPHF.
- **Always strict**: a non-perfect "PHF" is a category error. Build raises with diagnostics on failure; users tune and retry.
- **Strict MPHF**: at `load_factor == 1.0` the per-shard retry loop varies only the seed; it never relaxes the effective load factor (the `LF_BUMP` widening in `build_one_shard` is gated to `load_factor < 1.0`). So a successful MPHF build has `range_size == num_keys` exactly, or it raises. For `load_factor < 1.0`, retries still relax the factor slightly to rescue hard shards (the user asked for a target, not minimality). Do not re-enable the bump for `1.0`: it silently produced non-minimal "MPHFs".
- **Total query function**: `phobic_query` guards the empty-shard case (`num_buckets==0 || range==0`) and returns slot `0` (always valid for a non-empty build) instead of dividing by zero. Empty shards are reachable only by stranger keys (every built key lands in a non-empty shard) and by explicit `num_shards >> N`; the guard upholds the "arbitrary valid slot for unknown keys" contract without touching the wire format.
- **`uint16_t` pilots**: max 65535 candidates per bucket. Caps storage at 2 bytes/bucket. Bucket size ~14 is the empirical "always solvable" line; larger buckets exhaust the pilot space.
- **Per-shard retry**: each shard has its own retry budget (`max_retries`). One failing shard does not invalidate already-built shards in the parallel path; it does fail the whole build (no partial output).
- **Mmap-pre-paid wire format**: absolute pilot offsets, fixed-size descriptors. Costs ~24 bytes per blob; enables `from_file` later without breaking the format.
- **No membership test**: querying a key not in the build set returns an arbitrary valid slot. This is the contract; phobic does not promise to detect strangers.

## Test Layout

- `tests/test_phobic.py` (68 fast tests): core API, load_factor semantics + validation, num_shards (auto/explicit), num_threads determinism, bucket_size, seed reproducibility, build-failure diagnostic message, wire format magic + truncation + alignment, equality/copy/`__reduce__`, `assume_unique` opt-out behaviour, `__hash__ is None` (unhashable contract), distribution robustness across 4 key shapes, concurrent thread-safety, empty-shard stranger-query robustness (no SIGFPE), deserialize rejection of malformed-but-plausible blobs (zero range/buckets, overflowing pilot offset), key-domain consistency across `build`/`lookup`/`__getitem__`, and strict-MPHF minimality on the success path. One slow MPHF strict-fail regression lives here too (`@pytest.mark.slow`).
- `tests/test_scale.py` (3 tests, all `@pytest.mark.slow`): builds at 1M and 10M, serialisation round-trip at 1M.
- Slow suite total: 4 tests (`pytest -m slow`).

## Compiler Flags

Defined in `setup.py`: `-O2 -std=c11 -Wall -Wextra -pthread`. The `-pthread` flag handles both compilation (defines `_REENTRANT`) and linking. Uses `__uint128_t` for wyhash mixing (GCC/Clang, not MSVC).

Optional profiling build: `CFLAGS="-DPHOBIC_PROFILE=1" pip install -e .` enables per-phase timing prints in `_phobic.c` (key distribution, bucket sort, pilot search, serialise). Off by default; use when investigating a specific build bottleneck.

## Session Continuity

- `.claude/SESSION_NOTES.md`: pre-0.3.0 narrative.
- `.claude/SWEEP_BUCKET_SIZE.md`: 0.2.0 era trade-offs that informed the auto bucket_size choice.
- `.claude/SWEEP_0_3_0.md`: post-rewrite parallel-build and batch-query measurements.
- `.claude/DESIGN_0_3_0.md`: the 0.3.0 design doc, including the deliberate cuts and the wire format v3 spec. Read this before any non-trivial change.
- `.claude/cipher-maps-v1a-asks.md`: the cipher-maps consumer asks that motivated the 0.3.0 unification (the asks themselves were subsumed by the rewrite, not implemented as discrete additions).

## Version snapshot

Current version: 0.3.2 (`pyproject.toml`).

0.3.2 integrates two parallel review efforts: commit `4248043` (the
lookup-default-parallelism fix, the deserialize `s_poff` overflow fix, the
empty-shard guard, the `_encode_keys` dedup, dead-code removal) and the
remaining hardening below. The combined change set:

- **Fixed: `lookup()` never parallelised by default** (from `4248043`). It passed `num_threads=0` and `phobic_query_batch` only forked for `>1`, so batch query was silently always-serial. `phobic_query_batch` now resolves `<=0` to the online CPU count (same convention as build), so `lookup()` parallelises by default above the threshold. Output is unchanged (order-preserving, deterministic).
- **Fixed: empty-shard stranger query crashed with SIGFPE** (`bucket_for(h1, 0)`). `phobic_query` now guards `num_buckets==0 || range==0` and returns slot `0`. Reachable via stranger keys plus `num_shards >> N`, and via the batch path inside a GIL-released worker (the lookup-parallel-by-default change above makes that path the default, which is exactly why the batch uninitialised-chunk fix below matters). No wire-format change.
- **Fixed: batch-query `pthread_create` failure path ran uninitialised `chunks[]`** (UB / memory corruption). The fallback now fills each remaining chunk before running it on the calling thread.
- **Fixed: `phobic_deserialize` memory-safety + crash gaps** on malformed blobs: integer-overflow in the pilot-offset bounds check (`s_poff + pilot_bytes`) that allowed an attacker-controlled OOB read; acceptance of `range==0,buckets>0` and `buckets==0,range>0` descriptors that SIGFPE'd on first query; unchecked `cumulative += s_range` overflow. All now rejected with `ValueError`. `from_bytes` is a structural validator for trusted/locally-produced blobs (documented in README), not an authenticator.
- **Changed (strict MPHF)**: `load_factor==1.0` no longer silently relaxes under retry. A successful MPHF build is now `range_size == num_keys` exactly, or it raises. Behaviour change: hard MPHF builds that previously succeeded with a slightly-oversized range now fail (lower `load_factor` or raise `max_retries`).
- **Changed (key domain)**: `build`/`lookup`/`__getitem__` now accept a uniform key domain (str, bytes, bytearray, memoryview) and reject everything else (e.g. `int`) with `TypeError`, instead of `bytes(k)` silently mangling an int into a zero buffer. `__getitem__` previously rejected bytearray/memoryview that `lookup` accepted; now consistent.
- `lookup()` validates `num_threads >= 1` to match `build()`, and skips the redundant `bytes(k)` copy on already-bytes keys (mirrors `build()`'s fast path).
- Cleanup: removed the dead `copyreg` import; shared the v3 layout walk between `serialized_size` and `phobic_serialize` (`v3_layout`); shared the accessor capsule-unpacking boilerplate (`phf_from_args`).
- Docs: corrected the auto-shard divisor (`ceil(N / 16000)`, decimal, not 16384); added `assume_unique` to the module's public-surface docstring; de-versioned the module docstring; bumped C ABI header version comments.

What 0.3.2 did NOT change:
- Wire format (still `b"PHF3"`, byte-identical to 0.3.0/0.3.1 for any valid blob).
- Build determinism: same `seed` produces the same blob across machines and thread counts (verified after the `v3_layout` refactor).
- Public type/function surface (`PHF`, `build`, `from_bytes`).

What 0.3.1 changed on top of 0.3.0:
- Dropped the flat-key-buffer memcpy in `_module.c` (`py_build`, `py_query_batch`). C now reads pointers directly from `PyBytes`. Saved ~1.9s of build preamble at 10M keys.
- Avoided the redundant `bytes(k)` round-trip when `k is bytes` in the Python preamble. Saved another ~1.9s at 10M.
- Added `assume_unique=False` parameter to `build()` for callers with structurally-unique keys (HMAC outputs, UUIDs, etc). Skips the `O(N)` Python `set(...)` uniqueness check (~2s at 10M).
- Set `PHF.__hash__ = None` (was `hash(self.to_bytes())` in 0.3.0, which silently re-serialised the whole structure on every call).
- Added optional `PHOBIC_PROFILE` compile-time flag (`-DPHOBIC_PROFILE=1`) gating per-phase timing prints in `_phobic.c`. Off by default; turn on for build-bottleneck investigation.
- Net result on the cipher-maps benchmark (1M cipher keys, 12 threads, `assume_unique=True`): ~16x faster than 0.2.0 (~2.0s to ~125ms). Build is no longer on the cipher-maps critical path.

What 0.3.1 did NOT change:
- Wire format (still `b"PHF3"`, byte-identical to 0.3.0).
- Public API surface (`PHF`, `build`, `from_bytes`).
- Build determinism: same `seed` produces the same blob across machines and thread counts.
