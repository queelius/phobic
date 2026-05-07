"""phobic 0.3.0: minimal-ish perfect hash functions for very large key sets.

phobic builds a perfect hash function over a known key set. Always shards
internally (one shard at small N is the degenerate case), always strict
(returns a perfect hash function or raises). Build is parallel in C; batch
query is parallel in C above a threshold.

Public surface
--------------

    phobic.build(keys, *, load_factor=0.5, seed=None,
                 num_shards=None, num_threads=None,
                 bucket_size=None, max_retries=100) -> PHF
    phobic.from_bytes(blob) -> PHF
    phobic.PHF        # the only exported type

    phf[key]                              # scalar query
    phf.lookup(keys, num_threads=None)    # batch query
    phf.to_bytes() -> bytes
    phf.range_size, phf.num_shards, phf.bits_per_key
    len(phf)
    repr(phf)
    phf == other     # via to_bytes()
    pickling / copy.deepcopy work via __reduce__
"""

from __future__ import annotations

import copyreg as _copyreg  # noqa: F401  (referenced indirectly via __reduce__)

__all__ = ['PHF', 'build', 'from_bytes']

from phobic._module import (
    build       as _c_build,
    query       as _c_query,
    query_batch as _c_query_batch,
    serialize   as _c_serialize,
    deserialize as _c_deserialize,
    num_keys    as _c_num_keys,
    range_size  as _c_range_size,
    num_shards  as _c_num_shards,
    bits_per_key as _c_bits_per_key,
)


class PHF:
    """A perfect hash function mapping a known key set to distinct integers."""

    __slots__ = ('_handle',)

    def __init__(self, handle):
        self._handle = handle

    # ── query ────────────────────────────────────────────────────────

    def __getitem__(self, key):
        if isinstance(key, str):
            key = key.encode('utf-8')
        return _c_query(self._handle, key)

    def lookup(self, keys, *, num_threads=None):
        """Batch query. Returns list[int] in the same order as input keys.

        Parallel in C above an internal threshold; otherwise serial.
        """
        raw = [k.encode('utf-8') if isinstance(k, str) else bytes(k) for k in keys]
        nt = 0 if num_threads is None else int(num_threads)
        return _c_query_batch(self._handle, raw, nt)

    # ── introspection ────────────────────────────────────────────────

    @property
    def range_size(self):
        return _c_range_size(self._handle)

    @property
    def num_shards(self):
        return _c_num_shards(self._handle)

    @property
    def bits_per_key(self):
        return _c_bits_per_key(self._handle)

    def __len__(self):
        return _c_num_keys(self._handle)

    def __repr__(self):
        return (f"PHF(num_keys={len(self)}, range_size={self.range_size}, "
                f"num_shards={self.num_shards}, bits_per_key={self.bits_per_key:.2f})")

    # ── persistence ──────────────────────────────────────────────────

    def to_bytes(self):
        return _c_serialize(self._handle)

    @classmethod
    def from_bytes(cls, data):
        if not isinstance(data, bytes):
            data = bytes(data)
        return cls(_c_deserialize(data))

    # ── equality / cross-process transport ───────────────────────────

    def __eq__(self, other):
        if not isinstance(other, PHF):
            return NotImplemented
        return self.to_bytes() == other.to_bytes()

    def __hash__(self):
        return hash(self.to_bytes())

    def __reduce__(self):
        """Cross-process transport: round-trip via from_bytes."""
        return (from_bytes, (self.to_bytes(),))


# ── module-level builders ───────────────────────────────────────────

def build(keys, *, load_factor=0.5, seed=None, num_shards=None,
          num_threads=None, bucket_size=None, max_retries=100):
    """Build a perfect hash function over *keys*.

    Parameters
    ----------
    keys : iterable of str or bytes
        Must be unique and non-empty.
    load_factor : float, optional
        ``num_keys / range_size``, in (0, 1]. 1.0 = MPHF (hardest to build);
        0.5 (default) = 2x range overhead, fast build.
    seed : int, optional
        Reproducibility knob. ``None`` uses a random 64-bit seed.
    num_shards : int, optional
        Number of internal shards. ``None`` picks a data-driven default
        (1 below ~32K keys; up to ``4 * num_threads`` above).
    num_threads : int, optional
        Worker thread count for parallel build. ``None`` uses CPU count.
        (Phase 3a is single-threaded; threading lands in 3b.)
    bucket_size : int, optional
        Average keys per bucket per shard. ``None`` picks
        ``ceil(log2(per_shard_N))``.
    max_retries : int, optional
        Per-shard retry budget. Default 100.

    Returns
    -------
    PHF
    """
    if not (0.0 < load_factor <= 1.0):
        raise ValueError(f"load_factor must be in (0, 1], got {load_factor}")
    if max_retries < 1:
        raise ValueError(f"max_retries must be >= 1, got {max_retries}")

    if seed is None:
        import os
        seed = int.from_bytes(os.urandom(8), 'little')
    seed = int(seed)
    if not (0 <= seed < 2**64):
        raise ValueError(f"seed must be in [0, 2**64), got {seed}")

    if bucket_size is None:
        bucket_size_c = 0
    else:
        bucket_size_c = int(bucket_size)
        if bucket_size_c < 1:
            raise ValueError(f"bucket_size must be >= 1 or None, got {bucket_size}")

    if num_shards is None:
        num_shards_c = 0
    else:
        num_shards_c = int(num_shards)
        if num_shards_c < 1:
            raise ValueError(f"num_shards must be >= 1 or None, got {num_shards}")

    if num_threads is None:
        num_threads_c = 0
    else:
        num_threads_c = int(num_threads)
        if num_threads_c < 1:
            raise ValueError(f"num_threads must be >= 1 or None, got {num_threads}")

    raw = [k.encode('utf-8') if isinstance(k, str) else bytes(k) for k in keys]
    if not raw:
        raise ValueError("keys must be non-empty")
    if len(set(raw)) != len(raw):
        raise ValueError("keys must be unique")

    handle = _c_build(
        raw,
        float(load_factor),
        seed,
        int(max_retries),
        bucket_size_c,
        num_shards_c,
        num_threads_c,
    )
    return PHF(handle)


def from_bytes(data):
    """Deserialize a PHF from bytes."""
    return PHF.from_bytes(data)
