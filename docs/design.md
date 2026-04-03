# phobic: Perfect Hash Function Python Package

## Overview

A Python package providing PHOBIC (pilot-based) perfect hash functions. Core implementation in C11 for speed. No runtime dependencies.

A perfect hash function maps a known set of n keys to distinct integers in [0, m) with no collisions. `phobic.build(keys)` constructs one; `phf[key]` queries it.

## Goals

1. Clean, fast C11 implementation of the PHOBIC algorithm
2. Python API via C extension module (setuptools)
3. Configurable load factor (alpha parameter: range_size = ceil(n * alpha))
4. Serialization (to/from bytes)
5. Publish to PyPI as `phobic`

## Non-Goals

- Membership verification / fingerprinting (pure PHF only)
- C public API / installable C library (implementation detail of the Python package)
- C++ anything

## Python API

```python
import phobic

# Build
phf = phobic.build(["key1", "key2", "key3"])
phf = phobic.build(keys, alpha=1.05)        # 5% extra slots
phf = phobic.build(keys, seed=42)            # reproducible

# Query
slot = phf[key]          # int in [0, range_size)
slot = phf.slot(key)     # same

# Metadata
phf.num_keys             # number of keys in build set
phf.range_size           # size of output range (>= num_keys)
phf.bits_per_key         # space efficiency of the hash structure

# Persistence
data = phf.to_bytes()
phf2 = phobic.from_bytes(data)
```

## Architecture

### File Layout

```
~/github/released/phobic/
    pyproject.toml              -- package metadata, build config
    LICENSE                     -- MIT
    README.md                   -- usage docs
    src/
        phobic/
            __init__.py         -- Python API (PHF class, build, from_bytes)
            _phobic.c           -- C11: PHOBIC algorithm (build, query, serialize)
            _module.c           -- C11: Python C extension glue
    tests/
        test_phobic.py          -- pytest tests
```

### C Core (`_phobic.c`)

Pure C11 implementation. No Python dependency. Defines:

```c
typedef struct {
    uint16_t *pilots;          // pilot per bucket
    size_t    num_keys;
    size_t    range_size;
    size_t    num_buckets;
    size_t    bucket_size;     // average keys per bucket
    uint64_t  seed;
} phobic_phf;

// Build
phobic_phf *phobic_build(const char **keys, const size_t *key_lens,
                          size_t num_keys, double alpha, uint64_t seed);

// Query
size_t phobic_query(const phobic_phf *phf, const char *key, size_t key_len);

// Lifecycle
void phobic_free(phobic_phf *phf);

// Serialization
size_t phobic_serialize(const phobic_phf *phf, uint8_t *buf, size_t buf_len);
phobic_phf *phobic_deserialize(const uint8_t *buf, size_t buf_len);

// Metadata
double phobic_bits_per_key(const phobic_phf *phf);
```

### Hash Function

Two independent hashes from one key (for bucket assignment and slot derivation):

```c
// wyhash-style: fast, good distribution, no external dependency
static uint64_t phobic_hash(const char *key, size_t len, uint64_t seed);
```

Single hash function, derive h1 (bucket) and h2 (slot) by mixing with different constants. Same approach as the maph C++ version but in C.

### Build Algorithm

1. Hash all keys (dual wyhash-style hash: h1 for bucket, h2 for slot derivation)
2. Assign keys to buckets via h1, sort buckets by descending size
3. For each bucket (largest first), brute-force search for a pilot value (0..65535) such that `slot_with_pilot(h2, pilot, range_size)` produces no collisions against the global occupied bitset
4. Mark occupied slots, store winning pilot

The GIL is released during `phobic_build` so other Python threads can run concurrently.

### Retry Logic

If pilot search exhausts 65535 pilots for any bucket, the build retries with a different seed (up to 50 attempts). If alpha=1.0 (minimal), retries may bump alpha slightly (by 0.005) to give more room.

### Python Extension (`_module.c`)

Thin glue between Python and the C core:
- `_module.build(keys, alpha, seed)` -> PyCapsule wrapping `phobic_phf*`
- `_module.query(capsule, key)` -> PyLong
- `_module.serialize(capsule)` -> PyBytes
- `_module.deserialize(data)` -> PyCapsule
- `_module.num_keys(capsule)` -> PyLong
- `_module.range_size(capsule)` -> PyLong
- `_module.bits_per_key(capsule)` -> PyFloat

PyCapsule destructor calls `phobic_free()`.

### Python Wrapper (`__init__.py`)

```python
from phobic._module import (
    build as _build, query as _query,
    serialize as _serialize, deserialize as _deserialize,
    num_keys as _num_keys, range_size as _range_size,
    bits_per_key as _bits_per_key,
)

class PHF:
    __slots__ = ('_handle',)

    def __init__(self, handle):
        self._handle = handle

    def __getitem__(self, key):
        if isinstance(key, str):
            key = key.encode('utf-8')
        return _query(self._handle, key)

    def slot(self, key):
        return self[key]

    @property
    def num_keys(self):
        return _num_keys(self._handle)

    @property
    def range_size(self):
        return _range_size(self._handle)

    @property
    def bits_per_key(self):
        return _bits_per_key(self._handle)

    def to_bytes(self):
        return _serialize(self._handle)

    @classmethod
    def from_bytes(cls, data):
        return cls(_deserialize(data))

    def __len__(self):
        return self.num_keys

    def __repr__(self):
        return f"PHF(num_keys={self.num_keys}, range_size={self.range_size}, bits_per_key={self.bits_per_key:.2f})"

def build(keys, *, alpha=1.0, seed=None):
    if seed is None:
        import random
        seed = random.getrandbits(64)
    raw_keys = [k.encode('utf-8') if isinstance(k, str) else k for k in keys]
    handle = _build(raw_keys, float(alpha), int(seed))
    return PHF(handle)

def from_bytes(data):
    return PHF.from_bytes(data)
```

### Build Configuration (`pyproject.toml`)

```toml
[build-system]
requires = ["setuptools>=68.0"]
build-backend = "setuptools.build_meta"

[project]
name = "phobic"
version = "0.1.0"
description = "Fast minimal perfect hash functions"
readme = "README.md"
license = "MIT"
authors = [{ name = "Alexander Towell", email = "lex@metafunctor.com" }]
requires-python = ">=3.9"
classifiers = [
    "Programming Language :: Python :: 3",
    "Programming Language :: C",
    "License :: OSI Approved :: MIT License",
    "Operating System :: POSIX",
    "Topic :: Scientific/Engineering",
]

[project.urls]
Repository = "https://github.com/queelius/phobic"

[tool.setuptools]
packages = ["phobic"]
package-dir = {"" = "src"}

[tool.setuptools.extension-modules]
"phobic._module" = { sources = ["src/phobic/_module.c", "src/phobic/_phobic.c"] }
```

Note: the extension module config may need a `setup.py` or `setup.cfg` for C extensions, since `pyproject.toml` native extension support varies. Fallback:

```python
# setup.py
from setuptools import setup, Extension

setup(
    ext_modules=[
        Extension(
            "phobic._module",
            sources=["src/phobic/_module.c", "src/phobic/_phobic.c"],
            extra_compile_args=["-O2", "-std=c11", "-Wall", "-Wextra"],
            extra_link_args=[],
        ),
    ],
)
```

## Testing (`test_phobic.py`)

```python
import phobic
import pytest

def test_build_and_query():
    keys = [f"key_{i}" for i in range(1000)]
    phf = phobic.build(keys)
    slots = {phf[k] for k in keys}
    assert len(slots) == 1000  # all distinct
    assert all(0 <= s < phf.range_size for s in slots)

def test_alpha():
    keys = [f"k{i}" for i in range(100)]
    phf = phobic.build(keys, alpha=1.2)
    assert phf.range_size >= 120
    assert phf.num_keys == 100

def test_deterministic():
    keys = ["a", "b", "c"]
    phf1 = phobic.build(keys, seed=42)
    phf2 = phobic.build(keys, seed=42)
    assert all(phf1[k] == phf2[k] for k in keys)

def test_serialization():
    keys = [f"key_{i}" for i in range(500)]
    phf = phobic.build(keys)
    data = phf.to_bytes()
    phf2 = phobic.from_bytes(data)
    assert all(phf[k] == phf2[k] for k in keys)

def test_bytes_keys():
    keys = [b"raw_bytes_0", b"raw_bytes_1", b"raw_bytes_2"]
    phf = phobic.build(keys)
    assert phf[b"raw_bytes_0"] != phf[b"raw_bytes_1"]

def test_repr():
    phf = phobic.build(["a", "b", "c"])
    assert "PHF" in repr(phf)
    assert "num_keys=3" in repr(phf)

def test_len():
    phf = phobic.build(["x", "y", "z"])
    assert len(phf) == 3

def test_bits_per_key():
    keys = [f"k{i}" for i in range(10000)]
    phf = phobic.build(keys)
    assert 0 < phf.bits_per_key < 5.0  # should be ~2-3
```

## Success Criteria

- `pip install .` works
- All tests pass with `pytest`
- Bijectivity verified at 10K, 100K, 1M keys
- bits_per_key < 3.5 at 10K+ keys
- Serialization round-trip preserves all slot assignments
- No memory leaks (valgrind clean)
