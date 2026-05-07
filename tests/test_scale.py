"""Scale tests: 1M and 10M keys. Marked slow; opt in with `pytest -m slow`."""
from __future__ import annotations

import phobic
import pytest


@pytest.mark.slow
def test_build_1m_parallel():
    keys = [f"k_{i:010d}".encode() for i in range(1_000_000)]
    phf = phobic.build(keys, seed=0, num_threads=8)
    assert len(phf) == 1_000_000
    # Spot-check uniqueness on a sample (full 1M check is fine but slow).
    sample = keys[:5000] + keys[-5000:]
    slots = {phf[k] for k in sample}
    assert len(slots) == len(sample)


@pytest.mark.slow
def test_build_10m_parallel():
    keys = [f"big_{i:09d}".encode() for i in range(10_000_000)]
    phf = phobic.build(keys, seed=0, num_threads=8)
    assert len(phf) == 10_000_000
    sample = keys[:1000] + keys[-1000:]
    slots = {phf[k] for k in sample}
    assert len(slots) == len(sample)


@pytest.mark.slow
def test_serialization_1m_round_trip():
    keys = [f"k_{i:010d}".encode() for i in range(1_000_000)]
    phf = phobic.build(keys, seed=0, num_threads=8)
    blob = phf.to_bytes()
    phf2 = phobic.from_bytes(blob)
    sample = keys[:1000]
    for k in sample:
        assert phf[k] == phf2[k]
