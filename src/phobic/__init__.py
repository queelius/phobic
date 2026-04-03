"""phobic: Fast minimal perfect hash functions."""

__all__ = ['PHF', 'build', 'from_bytes']

from phobic._module import (
    build as _build,
    query as _query,
    serialize as _serialize,
    deserialize as _deserialize,
    num_keys as _num_keys,
    range_size as _range_size,
    bits_per_key as _bits_per_key,
    collisions as _collisions,
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

    @property
    def collisions(self):
        """Number of collisions (0 for a perfect hash function)."""
        return _collisions(self._handle)

    @property
    def is_perfect(self):
        """True if this is a perfect hash function (zero collisions)."""
        return self.collisions == 0

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
        c = self.collisions
        collision_str = f", collisions={c}" if c else ""
        return (f"PHF(num_keys={self.num_keys}, "
                f"range_size={self.range_size}, "
                f"bits_per_key={self.bits_per_key:.2f}"
                f"{collision_str})")


def build(keys, *, alpha=1.0, seed=None, max_retries=100, strict=True):
    """Build a perfect hash function from a list of keys.

    Args:
        keys: List of str or bytes keys.
        alpha: Overhead fraction. range_size = ceil(n * (1 + alpha)). Default
               1.0 gives range_size = 2n (fast build). Use smaller values
               (e.g. 0.05) for closer-to-minimal output at the cost of slower
               construction.
        seed: Random seed for reproducibility. None = random.
        max_retries: Number of seed/alpha attempts before giving up. Default 100.
        strict: If True (default), raise RuntimeError if a perfect build cannot
                be found within max_retries. If False, fall back to a non-perfect
                hash; PHF.collisions will be > 0. The best result across all
                attempts (fewest collisions) is returned.

    Returns:
        PHF object. Check PHF.is_perfect / PHF.collisions when strict=False.
    """
    if seed is None:
        import random
        seed = random.getrandbits(64)
    seed = int(seed)
    if not (0 <= seed < 2**64):
        raise ValueError(f"seed must be in [0, 2**64), got {seed}")
    raw_keys = [k.encode('utf-8') if isinstance(k, str) else k for k in keys]
    handle = _build(raw_keys, float(alpha), seed, int(max_retries), int(strict))
    return PHF(handle)


def from_bytes(data):
    """Deserialize a PHF from bytes."""
    return PHF.from_bytes(data)
