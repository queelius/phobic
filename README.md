# phobic

Fast minimal perfect hash functions for Python.

A **perfect hash function** maps a known set of *n* keys to distinct integers in [0, *m*) with no collisions. Build it once from your key set, then every lookup is O(1).

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
phf.range_size    # 4
phf.bits_per_key  # ~2.8

# Persistence
data = phf.to_bytes()
phf2 = phobic.from_bytes(data)
```

### Options

```python
# Closer to minimal: 5% overhead instead of 100% (slower build)
phf = phobic.build(keys, alpha=0.05)

# Fixed seed for reproducibility
phf = phobic.build(keys, seed=42)
```

## Space Efficiency

PHOBIC achieves ~2.7 bits/key for the hash structure. When used as a filter (PHF + n-bit fingerprints), total space is `2.7 + log2(1/e)` bits/key for false positive rate `e`. This beats Bloom filters (`1.44 * log2(1/e)` bits/key) whenever `e < 1.4%`.

## Performance

Implemented in C11 with no runtime dependencies. The GIL is released during construction so other Python threads can run concurrently.

## License

MIT
