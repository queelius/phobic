"""Scale regression tests.

phobic's README advertises ~1.8 us/key at n=100K. These tests confirm
that the build succeeds at 1M and 10M keys, and that bits/key stays
in the expected regime (~1 b/k for a minimal PHF with uint16 pilots
and bucket_size = ceil(log2(N))).

Marked slow because 10M takes minutes. Run with:

    pytest tests/test_scale.py -v
    pytest -m "slow" -v   (if slow marker is registered)
"""
import pytest
import phobic


def _keys(n):
    return [f"key_{i:010d}".encode() for i in range(n)]


@pytest.mark.slow
def test_build_1m():
    keys = _keys(1_000_000)
    phf = phobic.build(keys, seed=42)
    assert phf.num_keys == 1_000_000
    assert phf.is_perfect
    # Sample-verify rather than full-sweep (slow at 1M).
    for k in keys[::10_000]:
        assert 0 <= phf[k] < phf.range_size
    # Space: should be near ~1 b/k at this scale.
    assert phf.bits_per_key < 2.0


@pytest.mark.slow
def test_build_10m():
    keys = _keys(10_000_000)
    phf = phobic.build(keys, seed=42)
    assert phf.num_keys == 10_000_000
    assert phf.is_perfect
    for k in keys[::100_000]:
        assert 0 <= phf[k] < phf.range_size
    assert phf.bits_per_key < 2.0


@pytest.mark.slow
def test_serialization_1m():
    keys = _keys(1_000_000)
    phf = phobic.build(keys, seed=42)
    data = phf.to_bytes()
    phf2 = phobic.from_bytes(data)
    assert phf2.num_keys == phf.num_keys
    # Sample check that slot assignments match.
    for k in keys[::10_000]:
        assert phf[k] == phf2[k]
