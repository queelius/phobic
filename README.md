# phobic

Fast minimal perfect hash functions for Python.

A **perfect hash function** maps a known set of *n* keys to distinct integers in [0, *m*) with no collisions. Build it once from your key set, then every lookup is O(1). Implemented in C11 with no runtime dependencies.

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
phf["alice"]    # e.g., 2
phf["bob"]      # e.g., 0

# Properties
phf.num_keys      # 4
phf.range_size    # 8 (with default alpha=1.0)
phf.bits_per_key  # ~1.0
phf.collisions    # 0
phf.is_perfect    # True

# Persistence
data = phf.to_bytes()
phf2 = phobic.from_bytes(data)
```

### Options

```python
# Closer to minimal: 5% overhead instead of 100% (slower build, may need more retries)
phf = phobic.build(keys, alpha=0.05)

# Fixed seed for reproducibility
phf = phobic.build(keys, seed=42)

# Control retry budget (default 100)
phf = phobic.build(keys, max_retries=200)
```

### Non-perfect builds

By default, `build()` raises `RuntimeError` if no perfect hash is found within the retry budget. With `strict=False`, it always returns the best result found:

```python
phf = phobic.build(keys, alpha=0.05, strict=False)
phf.is_perfect   # False if no perfect build was found
phf.collisions   # number of keys placed in already-occupied slots
```

The builder tries multiple seeds and gradually widens `alpha` across retries, keeping the attempt with the fewest collisions.

## Space Efficiency

PHOBIC achieves ~1 bit/key for the hash structure at 100k keys. When used as a filter (PHF + n-bit fingerprints), total space is `bpk + log2(1/e)` bits/key for false positive rate `e`. This beats Bloom filters (`1.44 * log2(1/e)`) whenever `e` is small enough that the PHF overhead is absorbed.

## Performance

C11 core, single-threaded. The GIL is released during construction so other Python threads can run concurrently. Typical build times: ~0.5 us/key at n=1k, ~1.8 us/key at n=100k. Query throughput: ~2M queries/sec from Python (~500 ns/query including str-to-bytes encoding).

## Demo

See [`demo.ipynb`](demo.ipynb) for an interactive walkthrough covering the API, alpha trade-offs, space efficiency, serialization, and a matplotlib plot of how collisions decrease with retry budget.

## License

MIT
