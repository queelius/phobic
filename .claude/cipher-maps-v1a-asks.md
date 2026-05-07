# Asks from cipher-maps v1A

> **From**: cipher-maps v1A redesign brainstorming session at `~/github/trapdoor-computing/src/cipher-maps/`
> **To**: phobic maintainer (Alex Towell, plus any Claude session resuming phobic work)
> **Date**: 2026-05-06
> **Status**: API gaps identified. Two small additions plus an operational ask. None block cipher-maps v1A from shipping; all three would make v1A code less awkward.

## TL;DR

Two small phobic API additions and one operational item. Pure PHF API completeness, no scope creep into cipher-map territory:

1. **A**: `phobic.build_partitioned_with_slots(keys, ...) -> (PartitionedPHF, list[int])`. Mirrors the existing `phobic.build_with_slots` for the partitioned path.
2. **B**: Polymorphic `phobic.from_bytes(blob)` that dispatches on magic bytes (`PHOB` returns `PHF`, `PPHF` returns `PartitionedPHF`).
3. **F**: Push phobic to PyPI (currently editable-installed only, version 0.1.0). cipher-maps 1.0.0 needs to pin a stable version.

After A and B are added, cut a phobic 0.2.0 release and push to PyPI.

---

## Why this is reaching phobic now

cipher-maps is being rewritten as v1A: a greenfield, phobic-only, monolithic-cipher-map-only API. The seed-search backend (`core.py`, about 1500 LOC) gets deleted; the PHF backend (`phf_cipher_map.py`, about 650 LOC) becomes the canonical engine. The brainstorming for this rewrite is captured at `~/github/trapdoor-computing/src/cipher-maps/another-session-claude-code-plan.md`.

During brainstorming, the cipher-maps session enumerated which phobic APIs v1A consumes. Almost everything we need is already in phobic 0.1.0. The two gaps below are minor but they each save tens of lines of awkward workaround code in cipher-maps' build path and serialization path. Adding them upstream is cleaner than working around them.

A reminder of phobic's stated scope (from phobic's own CLAUDE.md): "pure PHF only. Membership testing, value retrieval, fingerprinting, and cipher maps are explicitly out of scope and live in the sibling `maph` research repo. If a request adds filtering / retrieval / Bloomier-style behavior, push back or build it as a sibling package."

These asks respect that boundary. Both are PHF API completeness, mirroring patterns phobic already has for the serial path.

---

## Ask A: `build_partitioned_with_slots`

### What

Add a new public function `phobic.build_partitioned_with_slots(keys, ...) -> (PartitionedPHF, list[int])` that mirrors the existing `phobic.build_with_slots` for the partitioned path. It returns the slot index for each input key, computed inside the parallel build instead of via a follow-up `phf.lookup(keys)` pass.

### Why cipher-maps wants it

cipher-maps fills `slot[phf[k]] = encode(y) XOR mask` for every cipher key right after construction. For the serial path, `build_with_slots` already saves the redundant `lookup` pass: phobic's CLAUDE.md explicitly cites cipher-maps as the motivating consumer.

For the partitioned path, cipher-maps currently has to do:

```python
phf = phobic.build_partitioned(cipher_keys, ...)
slot_indices = phf.lookup(cipher_keys)   # second hash + shard-route pass
```

That second pass adds N hashes plus N shard-routes for a 1M-key build, on the order of 200 to 400 ms wall-clock. Not catastrophic, but visible in cipher-maps' bench suite. It also adds a small chunk of glue code (a `_build_partitioned_with_slots` helper in cipher-maps' build dispatcher) that wouldn't be needed if phobic had the matching API.

### Suggested signature

```python
def build_partitioned_with_slots(
    keys: Iterable,
    *,
    num_shards: int | None = None,
    shard_seed: int | None = None,
    threads: int | None = None,
    target_shard_size: int = 15_000,
    **build_kwargs,
) -> tuple[PartitionedPHF, list[int]]:
    """Build a sharded PHF in parallel and return the slot for each input key.

    Equivalent to (but faster than):

        phf = phobic.build_partitioned(keys, ...)
        slots = phf.lookup(keys)
        return phf, slots

    Useful when the caller will immediately fill a slot array right after
    construction (e.g., cipher-maps' PHFCipherMap). Saves a redundant
    hash-and-shard-route pass over the whole key set.

    Returns:
        (PartitionedPHF, list[int]) where slots[i] == phf[keys[i]] for every i.
        Slots are computed during the parallel shard build, then offset and
        re-assembled in input order.
    """
```

### Suggested implementation sketch

In `~/github/repos/phobic/src/phobic/partitioned.py`, alongside `build_partitioned`:

```python
from phobic import build_with_slots as _build_one_with_slots

def build_partitioned_with_slots(
    keys, *, num_shards=None, shard_seed=None, threads=None,
    target_shard_size=15_000, **build_kwargs,
):
    raw = [k.encode("utf-8") if isinstance(k, str) else bytes(k) for k in keys]
    if not raw:
        raise ValueError("keys must be non-empty")
    if len(set(raw)) != len(raw):
        raise ValueError("keys must be unique")

    n = len(raw)
    if num_shards is None:
        num_shards = max(1, n // target_shard_size)
    if num_shards < 1:
        raise ValueError("num_shards must be >= 1")

    if shard_seed is None:
        shard_seed = random.getrandbits(64)
    shard_seed = int(shard_seed) & 0xFFFFFFFFFFFFFFFF

    # Distribute keys to shards; remember each key's original input index
    # so we can reassemble the slot vector in input order.
    shard_keys = [[] for _ in range(num_shards)]
    shard_idx = [[] for _ in range(num_shards)]
    for i, k in enumerate(raw):
        s = _shard_hash(k, shard_seed, num_shards)
        shard_keys[s].append(k)
        shard_idx[s].append(i)

    if threads is None:
        threads = min(num_shards, os.cpu_count() or 1)
    threads = max(1, int(threads))

    def _build_shard_with_slots(ks):
        if not ks:
            placeholder = _build_one([b"\x00"], **build_kwargs)
            return placeholder, []
        return _build_one_with_slots(ks, **build_kwargs)

    with ThreadPoolExecutor(max_workers=threads) as ex:
        shard_results = list(ex.map(_build_shard_with_slots, shard_keys))

    shard_phfs = [r[0] for r in shard_results]
    per_shard_slots = [r[1] for r in shard_results]

    # Build the offsets prefix sum, zeroing empty shards (they have a
    # placeholder PHF but contribute no range).
    offsets = [0]
    for i, s in enumerate(shard_phfs):
        contrib = s.range_size if shard_keys[i] else 0
        offsets.append(offsets[-1] + contrib)

    # Reassemble the slot vector in input order, applying shard offsets.
    slots = [0] * n
    for s_idx in range(num_shards):
        offset = offsets[s_idx]
        for orig_i, shard_slot in zip(shard_idx[s_idx], per_shard_slots[s_idx]):
            slots[orig_i] = offset + shard_slot

    return PartitionedPHF(shard_phfs, offsets, shard_seed), slots
```

In `__init__.py`, add to `__all__` and re-export:

```python
__all__ = [
    'PHF',
    'build',
    'build_with_slots',
    'from_bytes',
    'PartitionedPHF',
    'build_partitioned',
    'build_partitioned_with_slots',   # NEW
]

from phobic.partitioned import (
    PartitionedPHF,
    build_partitioned,
    build_partitioned_with_slots,     # NEW
)
```

### Tests

Add to `tests/test_partitioned.py`:

```python
def test_build_partitioned_with_slots_matches_lookup():
    keys = _keys(1000)
    phf, slots = phobic.build_partitioned_with_slots(keys, shard_seed=42)
    assert len(slots) == len(keys)
    assert slots == phf.lookup(keys)

def test_build_partitioned_with_slots_unique_slots():
    keys = _keys(5000)
    phf, slots = phobic.build_partitioned_with_slots(keys, shard_seed=42)
    assert len(set(slots)) == len(keys)

def test_build_partitioned_with_slots_str_keys():
    keys = [f"key_{i}" for i in range(500)]
    phf, slots = phobic.build_partitioned_with_slots(keys, shard_seed=42)
    assert slots == phf.lookup(keys)

def test_build_partitioned_with_slots_explicit_shards():
    keys = _keys(2000)
    phf, slots = phobic.build_partitioned_with_slots(
        keys, num_shards=8, shard_seed=42
    )
    assert phf.num_shards == 8
    assert slots == phf.lookup(keys)

def test_build_partitioned_with_slots_empty_input():
    with pytest.raises(ValueError, match="non-empty"):
        phobic.build_partitioned_with_slots([])

def test_build_partitioned_with_slots_duplicate_keys():
    with pytest.raises(ValueError, match="unique"):
        phobic.build_partitioned_with_slots([b"a", b"b", b"a"])
```

The key invariant: `slots == phf.lookup(keys)`. Anything else is implementation detail.

---

## Ask B: Polymorphic `phobic.from_bytes`

### What

Make `phobic.from_bytes(blob)` polymorphic: it dispatches to either `PHF.from_bytes` or `PartitionedPHF.from_bytes` based on the leading magic bytes of the input.

### Why cipher-maps wants it

cipher-maps' v1A binary format (CMAP v3) embeds a phobic blob as part of its body. On load, cipher-maps reads that blob and needs to reconstruct whichever PHF kind was serialized. With v1A's auto-routing serial vs partitioned, the same cipher-maps load path will encounter both kinds. Without polymorphic dispatch, cipher-maps has to peek at the magic bytes itself:

```python
if blob[:4] == b"PHOB":
    phf = phobic.PHF.from_bytes(blob)
elif blob[:4] == b"PPHF":
    phf = phobic.PartitionedPHF.from_bytes(blob)
else:
    raise ValueError(...)
```

The magic bytes are an internal phobic detail. cipher-maps hard-coding them creates a coupling between cipher-maps and phobic's wire format that should not exist. A polymorphic `phobic.from_bytes(blob)` hides this:

```python
phf = phobic.from_bytes(blob)   # returns PHF or PartitionedPHF
```

### Background: the magic bytes

- `PHF` writes magic `0x50484F42` ("PHOB") via `write_u32` at the start of its blob (see `_phobic.c:358`). This is the C-side `PHOBIC_MAGIC` constant. The byte order produced depends on `write_u32`'s endianness (likely little-endian, in which case the byte sequence at offset 0 is `0x42 0x4F 0x48 0x50`, i.e., `b"BOHP"` when read as 4 ASCII bytes). **A phobic maintainer should confirm the actual byte sequence emitted by inspecting a real serialized blob (e.g., `phobic.build([b"x"]).to_bytes()[:4]`)** before implementing the dispatch. The dispatch must compare against the actual bytes phobic produces, not against `b"PHOB"` literally.
- `PartitionedPHF` writes magic `b"PPHF"` directly via `parts = [_MAGIC, ...]` in `partitioned.py:174`. So this is unambiguous: the first 4 bytes of a partitioned blob are literally `0x50 0x50 0x48 0x46`.

### Suggested implementation

In `~/github/repos/phobic/src/phobic/__init__.py`, replace the current `from_bytes`:

```python
def from_bytes(data):
    """Deserialize a PHF or PartitionedPHF from bytes.

    Dispatches on the leading magic bytes of the blob:

      * b"BOHP"  (or whatever the LE-encoded form of PHOBIC_MAGIC is)
        -> PHF.from_bytes

      * b"PPHF"  -> PartitionedPHF.from_bytes

    The exact PHF magic depends on the C-side serialization byte order
    (see write_u32 in _phobic.c). This implementation should be written
    AFTER inspecting a real PHF blob to confirm the byte sequence.

    Args:
        data: bytes-like object containing a serialized PHF or PartitionedPHF.

    Returns:
        PHF or PartitionedPHF instance.

    Raises:
        ValueError: if the magic bytes don't match either format.
    """
    if not isinstance(data, (bytes, bytearray, memoryview)):
        data = bytes(data)
    head = bytes(memoryview(data)[:4])

    PHF_MAGIC = ...   # Set this from inspecting an actual PHF blob.
    PARTITIONED_MAGIC = b"PPHF"

    if head == PHF_MAGIC:
        return PHF.from_bytes(data)
    if head == PARTITIONED_MAGIC:
        return PartitionedPHF.from_bytes(data)
    raise ValueError(
        f"unrecognized magic {head!r}: expected {PHF_MAGIC!r} (PHF) "
        f"or {PARTITIONED_MAGIC!r} (PartitionedPHF)"
    )
```

The current `from_bytes` is just `return PHF.from_bytes(data)`, which silently produces a corrupted PHF (or raises an opaque deserialization error) when given a partitioned blob. The polymorphic version surfaces the actual format mismatch.

### Tests

Add to `tests/test_phobic.py`:

```python
def test_from_bytes_round_trip_phf():
    keys = [f"k_{i}".encode() for i in range(100)]
    phf = phobic.build(keys, seed=42)
    blob = phf.to_bytes()
    phf2 = phobic.from_bytes(blob)
    assert isinstance(phf2, phobic.PHF)
    assert phf2.num_keys == phf.num_keys
    assert all(phf[k] == phf2[k] for k in keys)

def test_from_bytes_round_trip_partitioned():
    keys = [f"k_{i}".encode() for i in range(1000)]
    phf = phobic.build_partitioned(keys, shard_seed=42)
    blob = phf.to_bytes()
    phf2 = phobic.from_bytes(blob)
    assert isinstance(phf2, phobic.PartitionedPHF)
    assert phf2.num_keys == phf.num_keys
    assert all(phf[k] == phf2[k] for k in keys)

def test_from_bytes_rejects_unknown_magic():
    with pytest.raises(ValueError, match="unrecognized magic"):
        phobic.from_bytes(b"XXXX" + b"\x00" * 100)

def test_from_bytes_truncated():
    with pytest.raises(ValueError):
        phobic.from_bytes(b"\x00\x00\x00")   # less than 4 bytes
```

---

## Ask F: PyPI release

cipher-maps 1.0.0 will pin a phobic version in `pyproject.toml`. Today phobic 0.1.0 is editable-installed from `~/github/repos/phobic/`, not on PyPI.

After A and B land (or independently if A and B are deferred), please cut a release:

1. If A and B land: bump version to **0.2.0**, update `__init__.py` `__all__`, write a `CHANGELOG.md` entry, push to PyPI.
2. If only the existing 0.1.0 surface is shipped: push **0.1.0** as-is so cipher-maps can pin it.

cipher-maps' `pyproject.toml` will then have:

```toml
[project]
dependencies = [
    "phobic>=0.2,<0.3",   # or >=0.1,<0.2 if no A/B yet
]
```

This is operational, not code. The user (Alex) can do it directly via `python -m build` plus `twine upload`, or via the standard PyPI release workflow.

---

## What we are NOT asking for

To be explicit about respecting phobic's stated scope:

- **Cipher-map / Bloom / membership testing**: stays out of phobic. cipher-maps owns it.
- **XOR slot masking**: stays in cipher-maps. No phobic awareness needed.
- **Homophonic encoding** (K(x) representations per element): stays in cipher-maps. cipher-maps generates K hashed cipher keys per element and feeds them to phobic as ordinary keys. Phobic doesn't need to know about K.
- **Value storage / Bloomier-style retrieval**: stays in `maph` (per phobic's CLAUDE.md). cipher-maps does not use maph; it has its own slot machinery.
- **Streaming serialize / NumPy interop / type stubs**: nice-to-haves mentioned in phobic's session notes. Not required for cipher-maps v1A.

If a future cipher-maps ask creeps into any of the above, push back; that's our problem to solve, not phobic's.

---

## Read for context

- This document.
- `~/github/trapdoor-computing/src/cipher-maps/another-session-claude-code-plan.md`: the cipher-maps v1A brainstorming handoff.
- `~/github/trapdoor-computing/src/cipher-maps/cipher_maps/phf_cipher_map.py`: the v0 cipher-maps PHF integration; the `_to_bytes`-then-build-then-fill-slots pattern is what motivates ask A. See lines 158-188.
- `~/github/repos/phobic/CLAUDE.md`: phobic's own session notes, including the GIL-release invariant and the bucket_size sweep data.
- `~/github/repos/phobic/src/phobic/__init__.py`: the current public surface.
- `~/github/repos/phobic/src/phobic/partitioned.py`: where ask A lives.

---

## End of asks
