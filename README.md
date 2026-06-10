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
phf.lookup(keys[:100])          # batch -> list[int]
phf.lookup_fixed(arr)           # bulk: numpy (N, W) uint8 -> uint64 array (up to ~16x)

# Persist (wire format v4, mmap-friendly layout)
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
    assume_unique=False, # skip the O(N) Python-side uniqueness check (opt-in)
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
- **Batch query** chunks across `num_threads` above an internal threshold, else serial (per-call thread-spawn cost makes serial faster for small batches). The thresholds differ by path: the list `lookup()` parallelises above ~256K keys (per-key Python overhead caps its parallel speedup), while the numpy `lookup_fixed` path parallelises from ~32K (pure C, ~4-5x). For the fastest bulk path use `lookup_fixed`.

Determinism is preserved: same `seed` and same `num_shards` produce byte-identical `to_bytes()` regardless of `num_threads`.

## Performance

Measured on a 12-core machine, default `load_factor=0.5`, 16-byte uniform keys (cipher-maps shape):

| N | num_shards | bits/key | build @ 12 threads (`assume_unique=True`) |
|---:|---:|---:|---:|
| 100K | 7 | 0.89 | ~25 ms |
| 1M | 63 | 0.89 | ~250 ms |
| 10M | 625 | **0.89** | **~1.2 s** |

bits/key dropped from ~1.16 (0.3.x) to **~0.89 in 0.4.0** (about 24% smaller blobs) via the PHF4 bit-packed pilot format. `assume_unique=True` skips the Python-side uniqueness check (a 2-second `set(raw)` hash at 10M keys); for callers whose keys are unique by construction (e.g. cipher-maps, HMAC outputs), this is pure win.

Bulk query: `lookup_fixed` (numpy) reaches ~9 ns/key at 1M (up to ~16x faster than `lookup(list)`) when keys are already in a contiguous buffer, by skipping per-key Python objects.

vs maph's `partitioned_phf<phobic4>` (the reference in the sibling research repo): at 10M, phobic is several x faster and uses ~3x fewer bits per key. See `.claude/EXPERIMENTS_0_4_0.md` for the 0.4.0 efficiency study.

## Wire format

`phf.to_bytes()` produces a versioned, mmap-friendly v4 blob (magic `b"PHF4"`):

```
56 bytes  global header   (num_keys, total_range, num_shards, seed, shard_seed)
48 bytes  per shard       (descriptor table, fixed-size, indexable; incl. pilot_bits)
variable  per shard       (bit-packed pilot blocks at pilot_bits/shard, 8-byte aligned)
```

Pilots are packed at the minimum fixed bit width per shard (typically ~13 bits, vs the old 16), which is where the ~24% size reduction comes from; they are unpacked on load, so query speed is unchanged. Fixed-size descriptor table + absolute pilot offsets means a future `phobic.from_file(path)` can mmap the blob directly without parsing variable-size sections. The 0.3.x `PHF3` format is not readable by 0.4.0 (rebuild persisted blobs; see `MIGRATION.md`).

`from_bytes` validates structure (magic, version, descriptor/pilot bounds, shard-shape consistency) and raises `ValueError` on a malformed blob rather than crashing. It does not *authenticate* blobs: it is built for round-tripping trusted, locally-produced output (`to_bytes`, `multiprocessing` hand-off), not as a hardened parser for adversarial input. A structurally-valid blob from an untrusted source can still encode a wrong (but safe) mapping; treat deserialized data with the usual caution.

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
