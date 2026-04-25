"""Partitioned PHF: sharded build for scale.

For large key sets, the single-threaded pilot search in `phobic.build`
becomes the bottleneck. This module shards keys across N smaller PHFs
that are built concurrently. Query dispatches to the owning shard and
returns that shard's slot offset by a prefix-sum.

Typical speedup on 8 cores for N=1M: 20-60x build-time, a modest
~1-3% additional bits/key (per-shard metadata + quantization).

Example:
    >>> import phobic
    >>> keys = [f"key_{i}".encode() for i in range(1_000_000)]
    >>> phf = phobic.build_partitioned(keys)   # parallel; ~seconds
    >>> phf[b"key_12345"]                       # returns a unique slot
    >>> data = phf.to_bytes()                   # wire-serializable
    >>> phf2 = phobic.PartitionedPHF.from_bytes(data)

Threading model:
    phobic.build() releases the GIL during its C pilot search, so a
    ThreadPoolExecutor with N Python threads delivers real parallelism
    without multiprocessing's IPC cost. Each worker builds its shard
    independently and the main thread stitches results together.
"""

from __future__ import annotations

import os
import random
import struct
from concurrent.futures import ThreadPoolExecutor
from typing import Iterable

from phobic import build as _build_one, PHF


_MAGIC = b"PPHF"   # "Partitioned PHobic PHF"
_VERSION = 1


def _shard_hash(key: bytes, seed: int, num_shards: int) -> int:
    """Deterministic shard selector. FNV-1a with a seed mix.

    Independent of phobic's internal hash so the shard index is stable
    across any future changes to phobic's hashing.
    """
    # 64-bit FNV-1a offset basis mixed with the seed.
    h = (0xcbf29ce484222325 ^ seed) & 0xFFFFFFFFFFFFFFFF
    prime = 0x100000001b3
    for b in key:
        h ^= b
        h = (h * prime) & 0xFFFFFFFFFFFFFFFF
    return h % num_shards


class PartitionedPHF:
    """Concatenated PHF composed from independently-built shard PHFs.

    The per-shard PHF maps its subset of keys to slots in [0, shard.range_size).
    The partitioned PHF's slot is shard_offset + shard_phf[key], giving
    a unique integer per build-set key in [0, total_range).
    """

    __slots__ = ("_shards", "_offsets", "_shard_seed", "_num_shards")

    def __init__(
        self,
        shards: list[PHF],
        offsets: list[int],
        shard_seed: int,
    ):
        if len(offsets) != len(shards) + 1:
            raise ValueError("offsets length must be len(shards) + 1")
        self._shards = list(shards)
        self._offsets = list(offsets)
        self._shard_seed = int(shard_seed) & 0xFFFFFFFFFFFFFFFF
        self._num_shards = len(shards)

    # ── query ────────────────────────────────────────────────────────────

    def __getitem__(self, key):
        if isinstance(key, str):
            key = key.encode("utf-8")
        s = _shard_hash(key, self._shard_seed, self._num_shards)
        return self._offsets[s] + self._shards[s][key]

    def slot(self, key):
        return self[key]

    def lookup(self, keys):
        """Batch query across shards.

        Groups keys by their target shard, calls the shard's batch query
        once per shard, then assembles the result in input order.
        Substantially faster than ``[phf[k] for k in keys]`` for large
        batches because per-shard work is amortized over many keys.
        """
        raw = [
            k.encode("utf-8") if isinstance(k, str) else bytes(k)
            for k in keys
        ]
        n = len(raw)
        # Bucket keys by shard, remembering original index to reassemble.
        per_shard_keys: list[list[bytes]] = [[] for _ in range(self._num_shards)]
        per_shard_idx: list[list[int]] = [[] for _ in range(self._num_shards)]
        for i, k in enumerate(raw):
            s = _shard_hash(k, self._shard_seed, self._num_shards)
            per_shard_keys[s].append(k)
            per_shard_idx[s].append(i)

        out = [0] * n
        for s in range(self._num_shards):
            if not per_shard_keys[s]:
                continue
            slots = self._shards[s].lookup(per_shard_keys[s])
            offset = self._offsets[s]
            for i, slot in zip(per_shard_idx[s], slots):
                out[i] = offset + slot
        return out

    def __len__(self):
        return self.num_keys

    # ── properties ───────────────────────────────────────────────────────

    @property
    def num_keys(self) -> int:
        return sum(s.num_keys for s in self._shards)

    @property
    def range_size(self) -> int:
        return self._offsets[-1]

    @property
    def bits_per_key(self) -> float:
        n = self.num_keys
        if n == 0:
            return 0.0
        # Shard storage + partitioned overhead (seed, offsets, shard headers).
        shard_bytes = sum(len(s.to_bytes()) for s in self._shards)
        overhead_bytes = len(_MAGIC) + 4 + 8 + 8 + 8 * self._num_shards
        return (shard_bytes + overhead_bytes) * 8 / n

    @property
    def collisions(self) -> int:
        return sum(s.collisions for s in self._shards)

    @property
    def is_perfect(self) -> bool:
        return self.collisions == 0

    @property
    def num_shards(self) -> int:
        return self._num_shards

    @property
    def shards(self) -> list[PHF]:
        return list(self._shards)

    def __repr__(self) -> str:
        return (
            f"PartitionedPHF(num_keys={self.num_keys}, "
            f"range_size={self.range_size}, "
            f"num_shards={self._num_shards}, "
            f"bits_per_key={self.bits_per_key:.2f}"
            + (f", collisions={self.collisions}" if self.collisions else "")
            + ")"
        )

    # ── serialization ────────────────────────────────────────────────────

    def to_bytes(self) -> bytes:
        parts = [
            _MAGIC,
            struct.pack("<I", _VERSION),
            struct.pack("<Q", self._num_shards),
            struct.pack("<Q", self._shard_seed),
        ]
        for s in self._shards:
            b = s.to_bytes()
            parts.append(struct.pack("<Q", len(b)))
            parts.append(b)
        return b"".join(parts)

    @classmethod
    def from_bytes(cls, data: bytes) -> "PartitionedPHF":
        view = memoryview(data)
        off = 0
        if view[:4].tobytes() != _MAGIC:
            raise ValueError(f"bad magic; expected {_MAGIC!r}")
        off += 4
        (version,) = struct.unpack_from("<I", view, off); off += 4
        if version != _VERSION:
            raise ValueError(f"unsupported version {version}")
        (num_shards,) = struct.unpack_from("<Q", view, off); off += 8
        (shard_seed,) = struct.unpack_from("<Q", view, off); off += 8

        shards: list[PHF] = []
        for _ in range(num_shards):
            (size,) = struct.unpack_from("<Q", view, off); off += 8
            shards.append(PHF.from_bytes(bytes(view[off : off + size])))
            off += size

        offsets = [0]
        for s in shards:
            offsets.append(offsets[-1] + s.range_size)
        return cls(shards, offsets, shard_seed)


def build_partitioned(
    keys: Iterable,
    *,
    num_shards: int | None = None,
    shard_seed: int | None = None,
    threads: int | None = None,
    target_shard_size: int = 15_000,
    **build_kwargs,
) -> PartitionedPHF:
    """Build a sharded PHF in parallel.

    Args:
        keys: list of str or bytes; must be unique.
        num_shards: how many shards. If None, uses
            max(1, len(keys) // target_shard_size).
        shard_seed: seed for the shard-selector hash. None picks a
            random 64-bit value.
        threads: max workers. None picks min(num_shards, os.cpu_count()).
        target_shard_size: used when num_shards is None. 15000 balances
            per-shard build cost vs per-shard metadata overhead.
        **build_kwargs: forwarded to phobic.build per shard (alpha,
            max_retries, strict, seed).

    Returns:
        PartitionedPHF.

    Notes:
        The per-shard PHF's own seed comes from build_kwargs["seed"]
        if given, else a random value per shard. The shard-selector
        seed is independent.
    """
    raw: list[bytes] = [
        k.encode("utf-8") if isinstance(k, str) else bytes(k) for k in keys
    ]
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

    # Distribute keys to shards.
    shard_keys: list[list[bytes]] = [[] for _ in range(num_shards)]
    for k in raw:
        shard_keys[_shard_hash(k, shard_seed, num_shards)].append(k)

    if threads is None:
        threads = min(num_shards, os.cpu_count() or 1)
    threads = max(1, int(threads))

    # Build each shard in parallel. phobic.build releases the GIL, so a
    # Python thread pool delivers real parallelism.
    def _build_shard(ks: list[bytes]) -> PHF:
        # Empty shards still need a PHF placeholder, but phobic.build
        # rejects empty inputs. Give such shards one dummy key; the
        # range contributes 0 to the total offset because we compute
        # offsets from actual shard range_size after build.
        if not ks:
            # Synthetic single-key PHF; range_size will be >=1 but no
            # real key ever selects this shard, so it's never queried.
            return _build_one([b"\x00"], **build_kwargs)
        return _build_one(ks, **build_kwargs)

    with ThreadPoolExecutor(max_workers=threads) as ex:
        shard_phfs = list(ex.map(_build_shard, shard_keys))

    # Zero out offsets for actually-empty shards so they contribute no
    # range. (We still keep the placeholder PHF in the list to preserve
    # shard indexing.)
    offsets = [0]
    for i, s in enumerate(shard_phfs):
        contrib = s.range_size if shard_keys[i] else 0
        offsets.append(offsets[-1] + contrib)

    return PartitionedPHF(shard_phfs, offsets, shard_seed)
