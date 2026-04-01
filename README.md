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
# Non-minimal: extra slots (faster build)
phf = phobic.build(keys, alpha=1.05)

# Parallel build (auto-detect cores)
phf = phobic.build(keys, threads=0)

# Fixed seed for reproducibility
phf = phobic.build(keys, seed=42)
```

## Performance

Implemented in C11 with parallel construction via pthreads. Typically ~2-3 bits per key.

## License

MIT
