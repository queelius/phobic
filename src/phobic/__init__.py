"""phobic: minimal-ish perfect hash functions for very large key sets.

phobic builds a perfect hash function over a known key set. Always shards
internally (one shard at small N is the degenerate case), always strict
(returns a perfect hash function or raises). Build is parallel in C; batch
query is parallel in C above a threshold.

Public surface
--------------

    phobic.build(keys, *, load_factor=0.5, seed=None,
                 num_shards=None, num_threads=None, bucket_size=None,
                 max_retries=100, assume_unique=False) -> PHF
    phobic.from_bytes(blob) -> PHF
    phobic.PHF        # the only exported type

    phf[key]                              # scalar query
    phf.lookup(keys, num_threads=None)    # batch query (list of str/bytes)
    phf.lookup_fixed(arr, num_threads=None)  # bulk query, numpy (N,W) uint8 -> uint64
    phf.to_bytes() -> bytes
    phf.range_size, phf.num_shards, phf.bits_per_key
    len(phf)
    repr(phf)
    phf == other     # via to_bytes()
    pickling / copy.deepcopy work via __reduce__
"""

from __future__ import annotations

__all__ = ['PHF', 'build', 'from_bytes']

from phobic._module import (
    build       as _c_build,
    query       as _c_query,
    query_batch as _c_query_batch,
    query_batch_fixed as _c_query_batch_fixed,
    serialize   as _c_serialize,
    deserialize as _c_deserialize,
    num_keys    as _c_num_keys,
    range_size  as _c_range_size,
    num_shards  as _c_num_shards,
    bits_per_key as _c_bits_per_key,
)


# The accepted key domain is uniform across build(), lookup(), and phf[key]:
# str (UTF-8 encoded), bytes (passed through), and the bytes-like buffers
# bytearray / memoryview (copied to bytes). Anything else is rejected loudly
# rather than silently mangled -- notably int, where bytes(5) would be a
# 5-byte zero buffer, not the integer 5 (a footgun that produced confusing
# downstream build failures or unqueryable PHFs).
def _reject_key(k):
    raise TypeError(
        f"keys must be str or bytes-like (bytes/bytearray/memoryview), "
        f"got {type(k).__name__}")


def _as_key(k):
    """Normalise a single key to bytes for the scalar query path."""
    if isinstance(k, bytes):
        return k
    if isinstance(k, str):
        return k.encode('utf-8')
    if isinstance(k, (bytearray, memoryview)):
        return bytes(k)
    _reject_key(k)


def _encode_keys(keys):
    """Normalise an iterable of keys to a ``list[bytes]`` for the C layer.

    ``str`` is encoded as UTF-8; ``bytes`` is passed through untouched;
    ``bytearray`` / ``memoryview`` are copied to ``bytes``; anything else
    raises ``TypeError``. Passing ``bytes`` keys straight through avoids a
    per-key ``bytes()`` constructor call (~1.9s for a 10M-key build) and
    avoids a real copy when a key is a ``bytes`` subclass.
    """
    return [
        k if isinstance(k, bytes)
        else k.encode('utf-8') if isinstance(k, str)
        else bytes(k) if isinstance(k, (bytearray, memoryview))
        else _reject_key(k)
        for k in keys
    ]


class PHF:
    """A perfect hash function mapping a known key set to distinct integers."""

    __slots__ = ('_handle',)

    def __init__(self, handle):
        self._handle = handle

    # ── query ────────────────────────────────────────────────────────

    def __getitem__(self, key):
        return _c_query(self._handle, _as_key(key))

    def lookup(self, keys, *, num_threads=None):
        """Batch query. Returns list[int] in the same order as input keys.

        Parallel in C above an internal size threshold; pass num_threads=1
        to force serial, or an explicit count to pin the worker count.
        Accepts the same key domain as build(): str / bytes / bytes-like.
        """
        if num_threads is not None and int(num_threads) < 1:
            raise ValueError(f"num_threads must be >= 1 or None, got {num_threads}")
        raw = _encode_keys(keys)
        nt = 0 if num_threads is None else int(num_threads)
        return _c_query_batch(self._handle, raw, nt)

    def lookup_fixed(self, keys, *, num_threads=None):
        """Bulk query over fixed-width keys, returning a numpy ``uint64`` array.

        ``keys`` is a C-contiguous ``(N, width)`` uint8 numpy array (or any
        2-D buffer of equal-length rows); row ``i`` is one ``width``-byte key.
        This skips all per-key Python objects (no bytes list in, no int list
        out), so it is much faster than ``lookup`` for large batches of
        uniform-width keys (e.g. cipher-maps' 16-byte HMAC keys). Requires
        numpy (optional dependency); ``lookup`` remains numpy-free.

        Returns ``numpy.ndarray`` of dtype ``uint64``, shape ``(N,)``.
        """
        import numpy as np
        if num_threads is not None and int(num_threads) < 1:
            raise ValueError(f"num_threads must be >= 1 or None, got {num_threads}")
        a = np.ascontiguousarray(keys, dtype=np.uint8)
        if a.ndim != 2:
            raise ValueError("keys must be a 2-D (N, width) uint8 array")
        width = a.shape[1]
        nt = 0 if num_threads is None else int(num_threads)
        raw = _c_query_batch_fixed(self._handle, a, width, nt)
        return np.frombuffer(raw, dtype=np.uint64)

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

    # Not hashable. The previous (0.3.0) implementation was
    # `hash(self.to_bytes())`, which forced a full re-serialization on every
    # call (10 MB at 10M keys). Hashing a PHF is rare enough that the cost
    # is unjustified; explicitly setting __hash__ = None matches the Python
    # convention for "not naturally hashable" types and prevents accidental
    # silent O(N) costs in set/dict operations. If a user genuinely needs a
    # hash, they can call hash(phf.to_bytes()) explicitly.
    __hash__ = None

    def __reduce__(self):
        """Cross-process transport: round-trip via from_bytes."""
        return (from_bytes, (self.to_bytes(),))


# ── module-level builders ───────────────────────────────────────────

def build(keys, *, load_factor=0.5, seed=None, num_shards=None,
          num_threads=None, bucket_size=None, max_retries=100,
          assume_unique=False):
    """Build a perfect hash function over *keys*.

    Parameters
    ----------
    keys : iterable of str or bytes-like
        str (UTF-8 encoded), bytes, bytearray, or memoryview. Must be unique
        and non-empty. Other types (e.g. int) raise TypeError.
    load_factor : float, optional
        ``num_keys / range_size``, in (0, 1]. 1.0 = exact MPHF: a successful
        build has range_size == num_keys, otherwise it raises (strict, never
        silently relaxed). 0.5 (default) = 2x range overhead, fast build.
    seed : int, optional
        Reproducibility knob. ``None`` uses a random 64-bit seed.
    num_shards : int, optional
        Number of internal shards. ``None`` picks a data-driven default:
        1 below ~32K keys, ``ceil(N / 16000)`` above (decimal 16000, not
        16384). Independent of ``num_threads``, so the same seed yields
        byte-identical output across machines with different CPU counts.
    num_threads : int, optional
        Worker thread count for parallel build. ``None`` uses CPU count.
    bucket_size : int, optional
        Average keys per bucket per shard. ``None`` picks
        ``ceil(log2(per_shard_N))``.
    max_retries : int, optional
        Per-shard retry budget. Default 100.
    assume_unique : bool, optional
        If True, skip the O(N) Python-side uniqueness check. Default False.
        At 10M keys the uniqueness check is ~2 seconds of single-threaded
        Python work; for callers that already know their keys are unique
        (e.g. cipher-maps, where keys are HMAC outputs guaranteed unique
        by construction) this is pure waste. If True and duplicates are
        present anyway, the build will fail with a confusing error or
        produce a non-PHF; that is the caller's responsibility.

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

    raw = _encode_keys(keys)
    if not raw:
        raise ValueError("keys must be non-empty")
    if not assume_unique and len(set(raw)) != len(raw):
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
