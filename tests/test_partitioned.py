"""Tests for PartitionedPHF."""
import phobic
import pytest


def _keys(n, prefix=b"k_"):
    return [prefix + f"{i:010d}".encode() for i in range(n)]


def test_basic_build_and_query():
    keys = _keys(1000)
    phf = phobic.build_partitioned(keys, shard_seed=42)
    slots = {phf[k] for k in keys}
    assert len(slots) == 1000, "partitioned PHF must give distinct slots for all S"
    assert phf.num_keys == 1000
    assert phf.is_perfect
    assert 0 < phf.bits_per_key < 10


def test_num_shards_explicit():
    keys = _keys(1000)
    phf = phobic.build_partitioned(keys, num_shards=8, shard_seed=42)
    assert phf.num_shards == 8
    assert phf.num_keys == 1000
    slots = {phf[k] for k in keys}
    assert len(slots) == 1000


def test_num_shards_auto():
    keys = _keys(45_000)
    phf = phobic.build_partitioned(keys, shard_seed=42)
    # target_shard_size default 15K, so ~3 shards at 45K.
    assert 2 <= phf.num_shards <= 4


def test_single_shard():
    keys = _keys(100)
    phf = phobic.build_partitioned(keys, num_shards=1, shard_seed=42)
    assert phf.num_shards == 1
    slots = {phf[k] for k in keys}
    assert len(slots) == 100


def test_serialize_round_trip():
    keys = _keys(5000)
    phf = phobic.build_partitioned(keys, num_shards=8, shard_seed=42)
    data = phf.to_bytes()
    phf2 = phobic.PartitionedPHF.from_bytes(data)
    assert phf2.num_keys == phf.num_keys
    assert phf2.range_size == phf.range_size
    assert phf2.num_shards == phf.num_shards
    for k in keys:
        assert phf[k] == phf2[k]


def test_deterministic():
    keys = _keys(500)
    a = phobic.build_partitioned(keys, num_shards=4, shard_seed=42,
                                   seed=1234)
    b = phobic.build_partitioned(keys, num_shards=4, shard_seed=42,
                                   seed=1234)
    for k in keys:
        assert a[k] == b[k]


def test_str_keys_work():
    keys = [f"key_{i}" for i in range(200)]
    phf = phobic.build_partitioned(keys, num_shards=4, shard_seed=42)
    slots = {phf[k] for k in keys}
    assert len(slots) == 200


def test_empty_rejected():
    with pytest.raises(ValueError):
        phobic.build_partitioned([])


def test_duplicate_rejected():
    with pytest.raises(ValueError):
        phobic.build_partitioned([b"dup", b"dup"])


def test_shard_keys_balance():
    """Every shard index must be reachable when num_shards matches key count scale."""
    keys = _keys(10_000)
    phf = phobic.build_partitioned(keys, num_shards=10, shard_seed=42)
    # Each of 10 shards should have roughly 1000 keys. Empty shards are legal
    # but unlikely here.
    counts = [s.num_keys for s in phf.shards]
    assert sum(counts) == 10_000
    # No shard should be radically small (expected value 1000, stddev ~32).
    assert min(counts) > 600
    assert max(counts) < 1400


def test_bits_per_key_reasonable():
    keys = _keys(10_000)
    phf = phobic.build_partitioned(keys, num_shards=1, shard_seed=42)
    # Single-shard partitioned overhead should stay close to plain phobic.
    plain = phobic.build(keys, seed=1)
    # Allow 30% slack for per-shard metadata + partition header.
    assert phf.bits_per_key < plain.bits_per_key * 1.3
