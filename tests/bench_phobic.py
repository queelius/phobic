"""
Benchmarks for phobic. Run with:

    pytest tests/bench_phobic.py -v

Skip during normal test runs:

    pytest tests/ --benchmark-skip
"""
import random
import pytest
import phobic


# ── key generators: four distribution shapes ──────────────────────────────
#
# Borrowed from maph's benchmark suite. Each generator is deterministic
# given the seed and returns unique bytes keys.

def _keys_random(n, seed=42):
    """Uniform 16-byte random keys."""
    rng = random.Random(seed)
    return [bytes(rng.randint(0, 255) for _ in range(16)) for _ in range(n)]


def _keys_sequential(n):
    """Zero-padded decimal strings. Low entropy; tests hash quality."""
    return [f"{i:010d}".encode() for i in range(n)]


def _keys_url(n, seed=42):
    """Synthetic URL-shaped keys with a common prefix + random suffix."""
    rng = random.Random(seed)
    prefix = "https://example.com/v1/resource/"
    return [(prefix + rng.randbytes(16).hex()).encode() for _ in range(n)]


def _keys_variable(n, seed=42):
    """Random-length keys in [4, 64] bytes. Amortized hashing cost."""
    rng = random.Random(seed)
    return [
        bytes(rng.randint(0, 255) for _ in range(rng.randint(4, 64)))
        for _ in range(n)
    ]


def _keys(n):
    """Legacy default (used by existing tests). Low-entropy string keys."""
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


# ── build: distribution sensitivity ───────────────────────────────────────
#
# PHOBIC should be robust to non-uniform key shapes (wyhash-style mix
# folds low-entropy input well). These benchmarks quantify the effect
# so a regression would be obvious.

@pytest.mark.parametrize("dist,gen", [
    ("random",     lambda n: _keys_random(n)),
    ("sequential", lambda n: _keys_sequential(n)),
    ("url",        lambda n: _keys_url(n)),
    ("variable",   lambda n: _keys_variable(n)),
])
def test_build_by_distribution(benchmark, dist, gen):
    """Build time across four key-distribution shapes at n=10K."""
    keys = gen(10_000)
    phf = benchmark(phobic.build, keys, seed=42)
    assert phf.num_keys == 10_000



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


def test_query_lookup_batch_10k(benchmark, phf_10k, keys_10k):
    """Batch query throughput: phf.lookup(keys_list) in one call."""
    benchmark(phf_10k.lookup, keys_10k)


# ── partitioned build: parallel scaling ───────────────────────────────────

@pytest.mark.parametrize("n", [100_000, 500_000, 1_000_000])
def test_build_partitioned_scaling(benchmark, n):
    """Build time vs key count for the partitioned (parallel) path.

    Compare to test_build_scaling at the same n: at 100K the
    serial build is faster (per-shard overhead dominates); from
    500K up, partitioned wins; at 1M+ partitioned is the only
    practical option.
    """
    keys = _keys(n)
    phf = benchmark(phobic.build_partitioned, keys, shard_seed=42)
    assert phf.num_keys == n


def test_query_partitioned_lookup_500k(benchmark):
    """Batch query through a partitioned PHF: shards each batch internally."""
    keys = _keys(500_000)
    phf = phobic.build_partitioned(keys, shard_seed=42)
    benchmark(phf.lookup, keys[:50_000])


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
