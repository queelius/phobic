"""
Benchmarks for phobic. Run with:

    pytest tests/bench_phobic.py -v

Skip during normal test runs:

    pytest tests/ --benchmark-skip
"""
import pytest
import phobic


def _keys(n):
    return [f"key_{i:010d}" for i in range(n)]


# ── pre-built fixtures for query / serialization benchmarks ──────────────

@pytest.fixture(scope="session")
def keys_10k():
    return _keys(10_000)


@pytest.fixture(scope="session")
def keys_100k():
    return _keys(100_000)


@pytest.fixture(scope="session")
def phf_10k(keys_10k):
    return phobic.build(keys_10k, seed=42)


@pytest.fixture(scope="session")
def phf_100k(keys_100k):
    return phobic.build(keys_100k, seed=42)


# ── build: scaling ────────────────────────────────────────────────────────

@pytest.mark.parametrize("n", [100, 1_000, 10_000, 100_000])
def test_build_scaling(benchmark, n):
    """Build time vs key count."""
    keys = _keys(n)
    phf = benchmark(phobic.build, keys, seed=42)
    assert phf.num_keys == n



# ── query: single-key latency and bulk throughput ─────────────────────────

def test_query_latency(benchmark, phf_10k, keys_10k):
    """Single-key lookup latency (includes Python str→bytes overhead)."""
    key = keys_10k[5_000]
    slot = benchmark(phf_10k.__getitem__, key)
    assert 0 <= slot < phf_10k.range_size


def test_query_bulk_10k(benchmark, phf_10k, keys_10k):
    """Bulk throughput: query all 10K keys in a tight loop."""
    def _run():
        for k in keys_10k:
            _ = phf_10k[k]
    benchmark(_run)


# ── serialization round-trip ──────────────────────────────────────────────

def test_serialize_100k(benchmark, phf_100k):
    data = benchmark(phf_100k.to_bytes)
    assert len(data) > 0


def test_deserialize_100k(benchmark, phf_100k):
    data = phf_100k.to_bytes()
    phf2 = benchmark(phobic.from_bytes, data)
    assert phf2.num_keys == phf_100k.num_keys


# ── space efficiency: design-doc success criterion ────────────────────────
# "bits_per_key < 3.5 at 10K+ keys"

@pytest.mark.parametrize("n", [10_000, 100_000])
def test_bits_per_key(n):
    phf = phobic.build(_keys(n), seed=42)
    bpk = phf.bits_per_key
    assert bpk < 3.5, f"bits_per_key={bpk:.2f} exceeds 3.5 at n={n}"
