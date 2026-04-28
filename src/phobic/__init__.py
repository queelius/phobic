"""phobic: Fast minimal perfect hash functions."""

__all__ = [
    'PHF',
    'build',
    'build_with_slots',
    'from_bytes',
    'PartitionedPHF',
    'build_partitioned',
]

from phobic._module import (
    build as _build,
    query as _query,
    query_batch as _query_batch,
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

    def lookup(self, keys):
        """Batch query: keys -> list of slots.

        Roughly 2-3x faster than [phf[k] for k in keys] for typical
        workloads because the C extension amortizes per-call overhead
        (capsule lookup, argument unpacking) across the whole batch.

        Args:
            keys: iterable of str or bytes.

        Returns:
            list of int, parallel to the input.
        """
        raw = [k.encode('utf-8') if isinstance(k, str) else k for k in keys]
        return _query_batch(self._handle, raw)

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
        """Number of keys that landed on an already-occupied slot (0 = perfect)."""
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


def _prepare_build(keys, alpha, seed, max_retries, strict, bucket_size):
    """Validate args, encode keys to bytes, run the C build.

    Shared by build() and build_with_slots(). Returns (handle, raw_keys)
    so callers can either wrap the handle alone or use raw_keys for a
    follow-up batch query without re-encoding.
    """
    if seed is None:
        import random
        seed = random.getrandbits(64)
    seed = int(seed)
    if not (0 <= seed < 2**64):
        raise ValueError(f"seed must be in [0, 2**64), got {seed}")
    if alpha < 0:
        raise ValueError(f"alpha must be >= 0, got {alpha}")
    if bucket_size is None:
        bucket_size_c = 0  # 0 = let C pick the auto default
    else:
        bucket_size_c = int(bucket_size)
        if bucket_size_c < 1:
            raise ValueError(
                f"bucket_size must be >= 1 or None for auto, got {bucket_size}"
            )
    raw_keys = [k.encode('utf-8') if isinstance(k, str) else k for k in keys]
    if len(set(raw_keys)) != len(raw_keys):
        raise ValueError("keys must be unique")
    handle = _build(
        raw_keys, float(alpha), seed,
        int(max_retries), int(strict), bucket_size_c,
    )
    return handle, raw_keys


def build(keys, *, alpha=1.0, seed=None, max_retries=100, strict=True,
          bucket_size=None):
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
        bucket_size: Average keys per bucket. None (default) picks
                ceil(log2(num_keys)), which minimises bits/key. Smaller values
                build dramatically faster at small N at the cost of more
                bits/key. Larger values rarely help and often fail to build.

    Returns:
        PHF object. Check PHF.is_perfect / PHF.collisions when strict=False.
    """
    handle, _ = _prepare_build(keys, alpha, seed, max_retries, strict, bucket_size)
    return PHF(handle)


def build_with_slots(keys, *, alpha=1.0, seed=None, max_retries=100, strict=True,
                     bucket_size=None):
    """Build a PHF and return both the PHF and the slot for each input key.

    Equivalent to (but faster than):

        phf = phobic.build(keys, ...)
        slots = phf.lookup(keys)
        return phf, slots

    Useful when the caller is going to immediately index a slot array by
    key (e.g. cipher_maps PHFCipherMap fills slot[phf[k]] = encode(value)
    for every key right after construction). Skipping the redundant
    Python-side encode pass and one round of validation roughly halves the
    per-key overhead at construction.

    Args:
        Same as build().

    Returns:
        (PHF, list[int]) where slots[i] == phf[keys[i]] for every i. With
        strict=False the slot list may contain duplicates whenever
        phf.collisions > 0.
    """
    handle, raw_keys = _prepare_build(
        keys, alpha, seed, max_retries, strict, bucket_size,
    )
    slots = _query_batch(handle, raw_keys)
    return PHF(handle), slots


def from_bytes(data):
    """Deserialize a PHF from bytes."""
    return PHF.from_bytes(data)


# Partitioned (parallel) build. Defined in a separate module to keep
# the core API small; re-exported here for discoverability.
from phobic.partitioned import PartitionedPHF, build_partitioned  # noqa: E402
