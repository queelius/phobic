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


def test_threads():
    keys = [f"key_{i}" for i in range(10000)]
    phf = phobic.build(keys, threads=4)
    slots = {phf[k] for k in keys}
    assert len(slots) == len(keys)


def test_single_thread():
    keys = [f"key_{i}" for i in range(10000)]
    phf = phobic.build(keys, threads=1)
    slots = {phf[k] for k in keys}
    assert len(slots) == len(keys)


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
    phf = phobic.build(keys, threads=0)
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
