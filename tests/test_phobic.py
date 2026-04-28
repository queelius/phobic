import phobic
import pytest


def test_build_and_query():
    keys = [f"key_{i}" for i in range(1000)]
    phf = phobic.build(keys)
    slots = {phf[k] for k in keys}
    assert len(slots) == 1000
    assert all(0 <= s < phf.range_size for s in slots)


def test_alpha():
    keys = [f"k{i}" for i in range(100)]
    phf = phobic.build(keys, alpha=1.2)
    assert phf.range_size >= 120
    assert phf.num_keys == 100
    slots = {phf[k] for k in keys}
    assert len(slots) == 100


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
    assert phf2.num_keys == phf.num_keys
    assert phf2.range_size == phf.range_size


def test_bytes_keys():
    keys = [b"raw_bytes_0", b"raw_bytes_1", b"raw_bytes_2"]
    phf = phobic.build(keys)
    slots = {phf.slot(k) for k in keys}
    assert len(slots) == 3


def test_repr():
    phf = phobic.build(["a", "b", "c"])
    r = repr(phf)
    assert "PHF" in r
    assert "num_keys=3" in r


def test_len():
    phf = phobic.build(["x", "y", "z"])
    assert len(phf) == 3


def test_bits_per_key():
    keys = [f"k{i}" for i in range(10000)]
    phf = phobic.build(keys)
    assert 0 < phf.bits_per_key < 5.0


def test_large_key_set():
    keys = [f"large_key_{i:08d}" for i in range(100000)]
    phf = phobic.build(keys)
    sample = keys[:1000] + keys[-1000:]
    slots = {phf[k] for k in sample}
    assert len(slots) == len(sample)


def test_lookup_batch_matches_scalar():
    keys = [f"key_{i}" for i in range(500)]
    phf = phobic.build(keys, seed=42)
    batch = phf.lookup(keys)
    scalar = [phf[k] for k in keys]
    assert batch == scalar


def test_lookup_accepts_bytes_and_str():
    phf = phobic.build([b"a", b"b", b"c"], seed=1)
    a = phf.lookup(["a", "b", "c"])
    b = phf.lookup([b"a", b"b", b"c"])
    assert a == b


def test_partitioned_lookup_matches_scalar():
    keys = [f"k{i}".encode() for i in range(1000)]
    phf = phobic.build_partitioned(keys, num_shards=8, shard_seed=42)
    batch = phf.lookup(keys)
    scalar = [phf[k] for k in keys]
    assert batch == scalar


def test_empty_keys_raises():
    with pytest.raises((ValueError, RuntimeError)):
        phobic.build([])


def test_from_bytes_module_level():
    keys = ["a", "b", "c"]
    phf = phobic.build(keys)
    data = phf.to_bytes()
    phf2 = phobic.from_bytes(data)
    assert all(phf[k] == phf2[k] for k in keys)


def test_duplicate_keys_raises():
    with pytest.raises((ValueError, RuntimeError)):
        phobic.build(["a", "b", "a"])


def test_from_bytes_truncated_raises():
    phf = phobic.build(["x", "y", "z"])
    data = phf.to_bytes()
    with pytest.raises((ValueError, RuntimeError)):
        phobic.from_bytes(data[:10])


def test_from_bytes_corrupted_magic_raises():
    phf = phobic.build(["x", "y", "z"])
    data = bytearray(phf.to_bytes())
    data[0] ^= 0xFF  # corrupt magic byte
    with pytest.raises((ValueError, RuntimeError)):
        phobic.from_bytes(bytes(data))


def test_seed_out_of_range_raises():
    with pytest.raises(ValueError):
        phobic.build(["a", "b"], seed=2**64)


def test_negative_alpha_raises():
    with pytest.raises(ValueError):
        phobic.build(["a", "b"], alpha=-0.5)


def test_perfect_build_is_perfect():
    keys = [f"key_{i}" for i in range(500)]
    phf = phobic.build(keys)
    assert phf.is_perfect
    assert phf.collisions == 0


def test_strict_false_returns_result():
    """Non-strict mode should always return a PHF (never raise on failure)."""
    keys = [f"k{i}" for i in range(200)]
    phf = phobic.build(keys, strict=False, max_retries=5)
    assert phf is not None
    assert len(phf) == 200


def test_strict_false_perfect_when_easy():
    """Non-strict with plenty of headroom should still find a perfect PHF."""
    keys = [f"key_{i}" for i in range(100)]
    phf = phobic.build(keys, alpha=1.0, strict=False, max_retries=50)
    assert phf.is_perfect


def test_strict_false_collision_count_type():
    """PHF.collisions must be a non-negative integer."""
    keys = [f"x{i}" for i in range(50)]
    phf = phobic.build(keys, strict=False, max_retries=10)
    assert isinstance(phf.collisions, int)
    assert phf.collisions >= 0


def test_max_retries_param():
    """max_retries=1 should succeed for an easy build."""
    keys = [f"k{i}" for i in range(50)]
    phf = phobic.build(keys, alpha=1.0, max_retries=1, seed=0)
    assert phf.is_perfect


def test_repr_includes_collisions_when_nonzero():
    """repr should mention collisions only when > 0."""
    keys = [f"k{i}" for i in range(50)]
    phf = phobic.build(keys, alpha=1.0)
    assert "collisions" not in repr(phf)


def test_serialization_preserves_collisions():
    """Round-trip serialization must preserve collisions field."""
    keys = [f"key_{i}" for i in range(200)]
    phf = phobic.build(keys, strict=False, max_retries=50)
    data = phf.to_bytes()
    phf2 = phobic.from_bytes(data)
    assert phf2.collisions == phf.collisions
    assert phf2.is_perfect == phf.is_perfect


def test_build_with_slots_returns_pair():
    keys = [f"k{i}" for i in range(100)]
    result = phobic.build_with_slots(keys, seed=0)
    assert isinstance(result, tuple) and len(result) == 2
    phf, slots = result
    assert isinstance(phf, phobic.PHF)
    assert isinstance(slots, list)
    assert len(slots) == len(keys)


def test_build_with_slots_matches_scalar():
    keys = [f"key_{i}" for i in range(500)]
    phf, slots = phobic.build_with_slots(keys, seed=42)
    assert slots == [phf[k] for k in keys]


def test_build_with_slots_bytes_keys():
    keys = [f"k{i}".encode() for i in range(200)]
    phf, slots = phobic.build_with_slots(keys, seed=0)
    assert slots == [phf[k] for k in keys]
    assert len(set(slots)) == len(slots)


def test_build_with_slots_str_bytes_equivalence():
    """str keys are encoded as UTF-8, so they should yield the same slots
    as the equivalent bytes keys at the same seed."""
    str_keys = ["alpha", "beta", "gamma"]
    byte_keys = [k.encode("utf-8") for k in str_keys]
    _, slots_str = phobic.build_with_slots(str_keys, seed=7)
    _, slots_bytes = phobic.build_with_slots(byte_keys, seed=7)
    assert slots_str == slots_bytes


def test_build_with_slots_empty_raises():
    with pytest.raises((ValueError, RuntimeError)):
        phobic.build_with_slots([])


def test_build_with_slots_duplicate_raises():
    with pytest.raises((ValueError, RuntimeError)):
        phobic.build_with_slots(["a", "b", "a"])


def test_build_with_slots_negative_alpha_raises():
    with pytest.raises(ValueError):
        phobic.build_with_slots(["a", "b"], alpha=-0.1)


def test_build_with_slots_seed_out_of_range_raises():
    with pytest.raises(ValueError):
        phobic.build_with_slots(["a", "b"], seed=2**64)


def test_build_with_slots_strict_false_consistent():
    """With strict=False the build may produce collisions, but the slot
    list must still agree with scalar queries on the returned PHF.
    Slots are unique iff phf.is_perfect."""
    keys = [f"k{i}" for i in range(200)]
    phf, slots = phobic.build_with_slots(keys, strict=False, max_retries=5)
    assert slots == [phf[k] for k in keys]
    assert (len(set(slots)) == len(slots)) == phf.is_perfect


# ── bucket_size parameter ──────────────────────────────────────────────


def test_bucket_size_explicit_small_builds():
    """Small fixed buckets always build at any reasonable N."""
    keys = [f"k{i}" for i in range(1000)]
    phf = phobic.build(keys, bucket_size=4, seed=0)
    slots = {phf[k] for k in keys}
    assert len(slots) == 1000


def test_bucket_size_default_matches_none():
    """Passing bucket_size=None must behave identically to the default."""
    keys = [f"k{i}" for i in range(200)]
    phf_none = phobic.build(keys, seed=42)
    phf_default = phobic.build(keys, bucket_size=None, seed=42)
    assert all(phf_none[k] == phf_default[k] for k in keys)


def test_bucket_size_changes_mapping():
    """Different bucket_size values (with the same seed) must produce
    different slot assignments. The mapping depends on num_buckets,
    which depends on bucket_size."""
    keys = [f"k{i}" for i in range(200)]
    phf4 = phobic.build(keys, bucket_size=4, seed=0)
    phf8 = phobic.build(keys, bucket_size=8, seed=0)
    differing = sum(1 for k in keys if phf4[k] != phf8[k])
    assert differing > 0


def test_bucket_size_smaller_uses_more_bits_per_key():
    """Smaller buckets have less pilot amortization, so bits/key rises."""
    keys = [f"k{i}" for i in range(10000)]
    bpk_small = phobic.build(keys, bucket_size=2, seed=1).bits_per_key
    bpk_large = phobic.build(keys, bucket_size=10, seed=1).bits_per_key
    assert bpk_small > bpk_large


def test_bucket_size_zero_raises():
    """bucket_size=0 is reserved as the C-side auto sentinel; Python rejects it."""
    with pytest.raises(ValueError):
        phobic.build(["a", "b"], bucket_size=0)


def test_bucket_size_negative_raises():
    with pytest.raises(ValueError):
        phobic.build(["a", "b"], bucket_size=-1)


def test_bucket_size_serialization_roundtrip():
    """Serialized PHF must round-trip the bucket_size invisibly: queries
    on the deserialized PHF must match the original."""
    keys = [f"k{i}" for i in range(500)]
    phf = phobic.build(keys, bucket_size=4, seed=7)
    phf2 = phobic.from_bytes(phf.to_bytes())
    assert all(phf[k] == phf2[k] for k in keys)
    assert phf2.range_size == phf.range_size


def test_bucket_size_flows_through_build_with_slots():
    """build_with_slots must accept and honour bucket_size."""
    keys = [f"k{i}" for i in range(300)]
    phf, slots = phobic.build_with_slots(keys, bucket_size=4, seed=3)
    assert slots == [phf[k] for k in keys]
    assert len(set(slots)) == len(slots)
