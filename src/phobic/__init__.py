"""phobic: Fast minimal perfect hash functions."""

from phobic._module import (
    build as _build,
    query as _query,
    serialize as _serialize,
    deserialize as _deserialize,
    num_keys as _num_keys,
    range_size as _range_size,
    bits_per_key as _bits_per_key,
)


class PHF:
    """A perfect hash function mapping keys to distinct integers."""

    __slots__ = ('_handle',)

    def __init__(self, handle):
        self._handle = handle

    def __getitem__(self, key):
        if isinstance(key, str):
            key = key.encode('utf-8')
        return _query(self._handle, key)

    def slot(self, key):
        """Return the slot index for a key."""
        return self[key]

    @property
    def num_keys(self):
        """Number of keys in the build set."""
        return _num_keys(self._handle)

    @property
    def range_size(self):
        """Size of the output range [0, range_size)."""
        return _range_size(self._handle)

    @property
    def bits_per_key(self):
        """Space efficiency of the hash structure."""
        return _bits_per_key(self._handle)

    def to_bytes(self):
        """Serialize to bytes."""
        return _serialize(self._handle)

    @classmethod
    def from_bytes(cls, data):
        """Deserialize from bytes."""
        return cls(_deserialize(data))

    def __len__(self):
        return self.num_keys

    def __repr__(self):
        return (f"PHF(num_keys={self.num_keys}, "
                f"range_size={self.range_size}, "
                f"bits_per_key={self.bits_per_key:.2f})")


def build(keys, *, alpha=1.0, seed=None, threads=0):
    """Build a perfect hash function from a list of keys.

    Args:
        keys: List of str or bytes keys.
        alpha: Load factor (>= 1.0). Higher = faster build, more memory.
        seed: Random seed for reproducibility. None = random.
        threads: Number of build threads (0 = auto-detect, 1 = single).

    Returns:
        PHF object.
    """
    if seed is None:
        import random
        seed = random.getrandbits(64)
    raw_keys = [k.encode('utf-8') if isinstance(k, str) else k for k in keys]
    handle = _build(raw_keys, float(alpha), int(seed), int(threads))
    return PHF(handle)


def from_bytes(data):
    """Deserialize a PHF from bytes."""
    return PHF.from_bytes(data)
