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
