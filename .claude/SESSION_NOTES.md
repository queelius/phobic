# Recent Session Notes (2026-04-23 to 2026-04-26)

This file captures what was added to phobic in the most recent maph
research session and how to resume related work in this directory.

## What landed (5 commits, all on master)

```
f53f3d7 bench: add partitioned build and lookup-batch benchmarks
51e7699 feat: batch query API (PHF.lookup, PartitionedPHF.lookup)
f8e52ce docs: document build_partitioned in README
320eafa feat: PartitionedPHF and build_partitioned for scale
f9c9744 test: add distribution and scale regression tests
```

## Big additions

### `phobic.build_partitioned` and `PartitionedPHF`

Pure-Python sharded build implemented in `src/phobic/partitioned.py`.

Each shard is an independent `phobic.PHF` built on a disjoint subset of
the keys. At query time, a deterministic FNV-1a mix selects the shard;
slot is `prefix_sum_offsets[shard] + shard_phf[key]`.

Build parallelism comes from `concurrent.futures.ThreadPoolExecutor`:
phobic.build releases the GIL during its C pilot search, so Python
threads deliver real CPU parallelism without multiprocessing.

Measured on the dev machine (8 cores):

| n | serial build | partitioned build | speedup |
|---:|-------------:|------------------:|--------:|
| 100K | 0.10 s | 0.18 s | 0.6x (overhead at small n) |
| 500K | 1.08 s | 0.94 s | 1.2x |
| 1M | 133.5 s | 2.0 s | **67.7x** |
| 2M | FAILS | 4.0 s | unbuildable serially |
| 10M | not feasible | 20.6 s | - |

The serial-failure regime above ~1M keys is structural: phobic's
`bucket_size = ceil(log2(N))` grows with N, making each bucket's
pilot search exponentially harder. Sharded builds keep per-shard
bucket_size around 14 (one shard ~15K keys), which always solves.

Space cost: bits/key grows from ~0.80 (serial) to ~1.18 (partitioned)
because each shard carries its own 56-byte phobic header plus a
partition-level wrapper. At 10M, total structure is ~14.7 MB.

### Batch query (`PHF.lookup`, `PartitionedPHF.lookup`)

`src/phobic/_module.c` exposes a `query_batch` C function. Python
wrapper at `PHF.lookup(keys)`. Roughly 2-3x faster than
`[phf[k] for k in keys]` because the C extension amortizes
`PyArg_ParseTuple`, capsule lookup, and PyObject churn over the batch.

Measured at 100K random keys: 241 ns/q scalar -> 90 ns/q batch
(2.69x). PartitionedPHF.lookup additionally groups keys by shard so
each shard's batch query runs in tight inner loops.

### Test infrastructure

- `tests/test_distributions.py`: builds at 10K across random,
  sequential, url, and variable-length keys. Catches regressions
  where a future hash change introduces structural bias.
- `tests/test_scale.py`: builds at 1M and 10M, marked
  `@pytest.mark.slow`. Default test runs exclude these (configured
  in pyproject.toml's addopts). Run with `pytest -m slow` to opt in.
- `tests/test_partitioned.py`: 11 cases covering basic build/query,
  shard counts, single-shard edge case, serialization round-trip,
  determinism, str+bytes keys, empty/duplicate rejection, shard
  balance, and bits/key reasonableness.
- `tests/bench_phobic.py`: extended with `test_build_partitioned_scaling`
  (parametrized at 100K/500K/1M), `test_query_lookup_batch_10k`, and
  `test_query_partitioned_lookup_500k`.

42 tests pass (39 fast + 3 slow). Run `pytest tests/` to verify.

## Things deliberately not done, with rationale

These came up during planning but were skipped:

**Multiplicative alpha back-off**. phobic's current `ALPHA_BUMP * (attempt / 10)`
gives a 0.05 alpha bump over 100 attempts: already substantial for
typical operating points (alpha=0.05 to alpha=1.0). Multiplicative
schedules would over-shoot at high alpha and under-shoot at low alpha.
No change without an evidence-based win.

**C-level fat-bucket parallelism**. With `build_partitioned` already
delivering 67x at 1M, adding pthread/atomic complexity to `_phobic.c`
for marginal speedup in the 100K-500K range (where serial is already
< 1.5s) was not worth it.

**ShockHash port**. This is a separate algorithm (bucketed 2-choice
cuckoo + ribbon retrieval). The companion maph repo has a simplified
implementation at ~1.54 b/k. phobic is *already at ~1 b/k* due to its
log(N) bucket_size with uint16 pilots, so ShockHash is not a space
improvement here. If it ever happens, treat it as a sibling package.

## Where research from maph touches this code

The maph research playground (../maph) has four conceptual layers
that are explicitly out of scope for phobic:

- **Retrieval** (key -> M-bit value with GIGO semantics)
- **Membership oracles** (xor_filter, ribbon_filter, binary_fuse_filter)
- **Codecs** (logical value alphabet ↔ M-bit pattern)
- **Compositions** (perfect_filter, bloomier, encoded_retrieval)

phobic stays narrowly in pure PHF territory. If a future request asks
for filtering / value retrieval / cipher maps, the natural answer is
"that lives in maph" or "build a sibling package".

## What's still worth doing in phobic

Practical follow-ups that build on what just landed:

1. **NumPy interop**. `phf.lookup(np.array(keys))` returning a
   `np.ndarray[int64]` would help workflows that already use numpy.
   Could be done in C with `PyArray_*` or via a thin adapter.

2. **Streaming serialize/deserialize**. Currently `to_bytes()` builds
   a single `bytes` object. For large structures (10M+ keys), a
   file-like API (`phf.save(fp)`, `phobic.load(fp)`) would avoid
   double-allocation.

3. **Smarter shard count**. The default `target_shard_size = 15_000`
   is empirical. A tuning study at 100K-100M keys could find the
   sweet spot for build time vs bits/key trade-off, perhaps as a
   function of N.

4. **Parallel batch query**. The existing `lookup` releases the GIL
   never (the C function runs while holding it). A truly parallel
   batch path would shard work across threads at query time. Probably
   only matters at very high QPS regimes.

5. **Python type stubs**. A `phobic.pyi` file would help users with
   IDE autocomplete and static type checkers. Easy win.

6. **CI matrix**. Currently no CI. A GitHub Actions workflow that
   runs pytest on Linux/macOS for Python 3.9-3.13 would catch
   portability issues.

## File layout summary

```
phobic/
├── pyproject.toml           # registers `slow` marker; addopts excludes by default
├── README.md                # documents build_partitioned with measured speedup
├── src/phobic/
│   ├── __init__.py          # re-exports PartitionedPHF, build_partitioned, lookup
│   ├── _phobic.c            # core C, unchanged from before this session
│   ├── _phobic.h            # core C header, unchanged
│   ├── _module.c            # +query_batch
│   └── partitioned.py       # NEW: PartitionedPHF + build_partitioned
└── tests/
    ├── test_phobic.py       # +lookup tests
    ├── test_distributions.py  # NEW: distribution regression tests
    ├── test_scale.py          # NEW: 1M/10M scale tests (slow marker)
    ├── test_partitioned.py    # NEW: PartitionedPHF tests
    └── bench_phobic.py        # +partitioned scaling, batch lookup
```

## How to resume

To pick up where this session left off:

```bash
cd ~/github/released/phobic
pip install -e . --quiet
pytest tests/                    # 39 fast tests
pytest tests/ -m slow            # add 1M and 10M scale (~5 min for 1M)
pytest tests/bench_phobic.py -v  # benchmark suite
```

The companion maph research repo is at `~/github/released/maph`. See
its `docs/BENCHMARK_RESULTS.md` for the broader algorithm-comparison
context that motivated this session's phobic additions.
