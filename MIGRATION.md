# 0.4.0 to 0.4.1

A review-driven patch. No wire-format change (PHF4 blobs are unchanged and
interoperable), no API surface change. One behaviour change to be aware of:

- **`lookup_fixed` now rejects non-uint8 arrays** with `TypeError` instead of
  silently casting them mod 256. If you were passing an `int`/other-dtype numpy
  array, you were getting wrong (truncated) slots; pass a `uint8` array of the
  raw key bytes (e.g. `np.frombuffer(b"".join(keys), np.uint8).reshape(n, w)`).
- Free: `lookup_fixed` is now 2-4x faster for 32K-256K batches (it parallelises
  from ~32K instead of ~256K). No code change needed.

---

# Migrating from phobic 0.3.x to 0.4.0

0.4.0 is a space + query efficiency release. The public API is unchanged; the
one breaking change is the wire format.

## What changed

- **Wire format break: `PHF3` -> `PHF4`.** Pilots are now bit-packed at the
  minimum fixed width per shard instead of `uint16`, cutting serialized size by
  **~24%** (bits/key ~1.16 -> ~0.89). Blobs from 0.3.x (`PHF3`) are **not
  readable** by 0.4.0; rebuild any persisted PHFs from the source key set.
  `to_bytes()` / `from_bytes()` calls are unchanged; only the bytes differ
  (smaller, and `b"PHF4..."` magic). Build, query, and determinism are unchanged.
- **`PHF.lookup_fixed(arr)` (new, optional).** A numpy bulk-query path: pass a
  C-contiguous `(N, width)` uint8 array, get back a `uint64` numpy array. Skips
  all per-key Python objects, so it is up to ~16x faster than `lookup(list)` for
  large batches of uniform-width keys (when keys are already in a buffer).
  Requires numpy (optional dependency); `lookup` / `build` stay numpy-free.
- **Batch-query threshold raised (perf fix).** `lookup()` now stays serial below
  ~256K keys, where per-call thread spawn made the 0.3.2 parallel-by-default path
  1.4x-63x slower for moderate batches. No API change; moderate-batch `lookup()`
  is now much faster. Pass an explicit `num_threads` to override.

No code changes are required beyond rebuilding any persisted blobs. The
`alpha`/`PartitionedPHF`/etc. notes below are for the older 0.2 -> 0.3 jump.

---

# Migrating from phobic 0.2.x to 0.3.0

phobic 0.3.0 is an aggressive rewrite that collapses the dual `PHF` / `PartitionedPHF` design into a single unified type. Most calling sites need small mechanical changes; some uses need the rewrite to think differently. This guide covers both.

## What changed at a glance

- **One type**: `phobic.PHF` is the only class. `PartitionedPHF` is gone.
- **One builder**: `phobic.build(...)` handles both small and large key sets. `build_partitioned`, `build_with_slots`, `build_partitioned_with_slots` are gone.
- **One wire format**: magic `b"PHF3"`. Old `BOHP` (PHF) and `PPHF` (Partitioned) blobs from 0.2.x are not readable. Rebuild any persisted PHFs.
- **`alpha` is now `load_factor`**: canonical hash-table semantics, in `(0, 1]`. See conversion below.
- **`strict` is gone**: builds always produce a perfect hash function or raise. Non-perfect outputs were a category error and are removed.
- **Parallelism moved into C**: `phobic.build` and `phf.lookup` use pthread internally. The Python-level `ThreadPoolExecutor` orchestration in 0.2.x is no longer needed.

## API rename table

| 0.2.x | 0.3.0 |
|---|---|
| `phobic.build(keys, alpha=0.05)` | `phobic.build(keys, load_factor=1.0/(1+0.05))` ≈ `load_factor=0.95` |
| `phobic.build(keys, alpha=1.0)` | `phobic.build(keys)` (default `load_factor=0.5`) |
| `phobic.build(keys, alpha=0.0)` | `phobic.build(keys, load_factor=1.0)` (MPHF) |
| `phobic.build(keys, strict=False)` | not supported. Adjust `load_factor` / `max_retries` and let the build raise on failure. |
| `phobic.build_partitioned(keys, num_shards=N)` | `phobic.build(keys, num_shards=N)` |
| `phobic.build_partitioned(keys, threads=N)` | `phobic.build(keys, num_threads=N)` |
| `phobic.build_with_slots(keys)` | `phf = phobic.build(keys); slots = phf.lookup(keys)` (now parallel C) |
| `phobic.build_partitioned_with_slots(keys, ...)` | same as above |
| `phf.is_perfect` | always `True`; remove the check |
| `phf.collisions` | always `0`; remove the check |
| `phf.num_keys` | `len(phf)` |
| `phf.slot(key)` | `phf[key]` |
| `phf.shards` | gone (implementation detail) |
| `phobic.PartitionedPHF.from_bytes(blob)` | `phobic.from_bytes(blob)` (one type now) |

## load_factor conversion

The relationship is `load_factor = 1 / (1 + alpha)`, equivalently `alpha = 1/load_factor - 1`.

| 0.2.x `alpha` | 0.3.0 `load_factor` |
|---:|---:|
| 0.05 | 0.95 |
| 0.10 | 0.91 |
| 0.50 | 0.67 |
| 1.00 | 0.50 (default) |
| 2.00 | 0.33 |

cipher-maps' `phf_cipher_map.py` previously did this conversion at the call site (`alpha = max(0.01, 1.0/load_factor - 1.0)`); it can now pass `load_factor` straight through.

## Common patterns

### Small build, simple usage

**Before (0.2.x):**
```python
phf = phobic.build(keys, alpha=1.0)
slot = phf[key]
```

**After (0.3.0):**
```python
phf = phobic.build(keys)             # load_factor=0.5 by default = same as alpha=1.0
slot = phf[key]
```

### Large build (was partitioned)

**Before (0.2.x):**
```python
phf = phobic.build_partitioned(keys, num_shards=32, threads=8)
slots = phf.lookup(keys)             # Python-level shard routing
```

**After (0.3.0):**
```python
phf = phobic.build(keys, num_shards=32, num_threads=8)   # auto-shards if omitted
slots = phf.lookup(keys, num_threads=8)                   # C-level parallel batch
```

### Build + slot scatter (cipher-maps pattern)

**Before (0.2.x):**
```python
phf, slots = phobic.build_with_slots(keys)
for i, s in enumerate(slots):
    table[s] = encode(values[i])
```

**After (0.3.0):**
```python
phf = phobic.build(keys)
slots = phf.lookup(keys)             # one parallel C call; matches build_with_slots' speed
for i, s in enumerate(slots):
    table[s] = encode(values[i])
```

### Polymorphic deserialisation

**Before (0.2.x):**
```python
if blob[:4] == b"BOHP":              # peeking at internal magic
    phf = phobic.PHF.from_bytes(blob)
elif blob[:4] == b"PPHF":
    phf = phobic.PartitionedPHF.from_bytes(blob)
```

**After (0.3.0):**
```python
phf = phobic.from_bytes(blob)        # one type, no dispatch
```

### Cross-process transport

**0.3.0 only** (was not available in 0.2.x):
```python
import multiprocessing as mp
with mp.Pool(8) as pool:
    pool.map(worker, [phf] * 8)      # PHF survives via __reduce__
```

## Wire format

0.2.x serialised PHFs with two different magics:

- `b"BOHP"` for plain `PHF` (little-endian "PHOB")
- `b"PPHF"` for `PartitionedPHF`

0.3.0 uses `b"PHF3"` exclusively. **Old blobs are not readable**. Any persisted PHFs must be rebuilt from the original key set.

If you need cross-version migration, do it before upgrading: under 0.2.x, run `phobic.PHF.from_bytes(old_blob)` to get the keys back via your own metadata layer, then rebuild under 0.3.0.

## What stays the same

These survived the rewrite and behave the same:

- `seed` for reproducibility (same seed yields byte-identical output, including across thread counts)
- `bucket_size` build knob (same name, same semantics)
- `max_retries` build knob (same name, same semantics; now per-shard)
- `phf.range_size` property
- `phf.bits_per_key` property
- str/bytes auto-encoding for keys
- The GIL is released during build and batch query

## When a build fails

0.3.0 raises a `RuntimeError` with a diagnostic naming the failing shard and the effective parameters, with concrete tuning suggestions:

```
PHOBIC build failed: shard 17 could not place all keys after 100 retries
(best attempt left 4 unplaceable at load_factor=0.9750, bucket_size=14).
Try lower load_factor, more shards, or higher max_retries.
```

This replaces 0.2.x's generic `RuntimeError("PHOBIC build failed")`. If you were catching that exception, no change is needed; if you were inspecting the message, the new format is structured.
