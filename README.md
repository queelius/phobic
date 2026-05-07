# phobic

**Minimal-ish perfect hash functions for very large key sets, with a parameterised load factor.**

A perfect hash function maps a known set of *n* keys to distinct integers in `[0, m)` with no collisions. Build it once from your key set, then every lookup is O(1). phobic builds in parallel C, queries in parallel C, and serialises to a portable wire format.

```
pip install phobic
```

## Usage

```python
import phobic

# Build
keys = [f"key_{i}".encode() for i in range(1_000_000)]
phf = phobic.build(keys)

# Query
phf[b"key_42"]                  # scalar -> int slot
phf.lookup(keys[:100])          # batch -> list[int] (parallel C above ~1K keys)

# Persist (wire format v3, mmap-friendly layout)
data = phf.to_bytes()
phf2 = phobic.from_bytes(data)
assert phf == phf2

# Inspect
len(phf)             # number of keys
phf.range_size       # output range, [0, range_size)
phf.num_shards       # internal sharding (1 at small N, more at scale)
phf.bits_per_key     # serialised size in bits, divided by num_keys
```

## Knobs

```python
phf = phobic.build(
    keys,
    load_factor=0.5,    # num_keys / range_size, in (0, 1]. 1.0 = MPHF.
    seed=None,          # one knob; reproducible builds
    num_shards=None,    # None = auto (1 below ~32K keys; scales above)
    num_threads=None,   # None = auto (CPU count)
    bucket_size=None,   # None = ceil(log2(per_shard_N))
    max_retries=100,    # per-shard retry budget on the pilot search
)
```

### `load_factor` (the central trade-off)

`load_factor` is the canonical hash-table sense: keys per slot, in `(0, 1]`.

| `load_factor` | meaning | build cost | space |
|---|---|---|---|
| `0.1` | very loose, ~10x range | trivial | wasteful |
| `0.5` (default) | 2x range | fast | moderate |
| `0.95` | nearly minimal | slow | tight |
| `1.0` | MPHF (range == num_keys) | hardest | optimal |

If a build can't find a perfect hash within `max_retries`, phobic raises a `RuntimeError` that *names the failing shard* and *suggests how to relax the constraints*.

### Parallelism

Both the build and `lookup()` release the GIL and fan out across pthreads:

- **Build** parallelism is per-shard. At `num_shards=1` the build is single-threaded; above that it scales nearly linearly to `num_threads`.
- **Batch query** is chunked across `num_threads` above an internal threshold (~1024 keys). Below the threshold the call is serial.

Determinism is preserved: same `seed` and same `num_shards` produce byte-identical `to_bytes()` regardless of `num_threads`.

## Performance

Measured on a 12-core machine, default `load_factor=0.5`, 16-byte uniform keys (cipher-maps shape):

| N | num_shards | bpk | build @ 1 thread | build @ 12 threads | speedup |
|---:|---:|---:|---:|---:|---:|
| 1K | 1 | 2.37 | <1 ms | <1 ms | (tiny) |
| 100K | 7 | 1.17 | 57 ms | 26 ms | 2.2x |
| 1M | 63 | 1.17 | 636 ms | 306 ms | 2.1x |
| 10M | 625 | 1.16 | 7.42 s | 3.72 s | 2.0x |

vs phobic 0.2.0: 1M parallel went from 2.0 s to 0.34 s (5.9x), and 10M from "would not finish" serially to 7.4 s serial / 3.7 s parallel.

vs maph's `partitioned_phf<phobic4>` (the reference in the sibling research repo): at 10M, phobic 0.3.0 is 2x faster and uses 2.4x fewer bits per key. See `.claude/SWEEP_0_3_0.md` for the full sweep.

## Wire format

`phf.to_bytes()` produces a versioned, mmap-friendly v3 blob (magic `b"PHF3"`):

```
56 bytes  global header   (num_keys, total_range, num_shards, seed, shard_seed)
40 bytes  per shard       (descriptor table, fixed-size, indexable)
variable  per shard       (uint16 pilot blocks, 8-byte aligned)
```

Fixed-size descriptor table + absolute pilot offsets means a future `phobic.from_file(path)` can mmap the blob directly without parsing variable-size sections. (Not in 0.3.0; the format pre-pays for it.)

## Cross-process transport

`PHF` instances support equality (`==`), `copy.deepcopy`, and the `__reduce__` protocol used by `multiprocessing.Pool`:

```python
import multiprocessing as mp

phf = phobic.build(keys)
with mp.Pool(8) as p:
    p.map(worker_using_phf, work_items)   # phf serialises through to_bytes / from_bytes
```

## What phobic explicitly is not

phobic is a pure PHF builder. The following live elsewhere:

- **Membership testing / Bloom filters / fingerprinting**: out of scope. Phobic does not promise that a key not in the build set will be detected; it returns an arbitrary slot for unknown keys. See `maph` for membership oracles.
- **Value retrieval / Bloomier maps / cipher maps**: out of scope. Build an external value array indexed by `phf[key]`.
- **Approximate / non-perfect modes**: a `PHF` is always perfect. Build failure raises with a diagnostic; non-perfect output is a category error.

## License

MIT
