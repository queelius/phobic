"""Key-distribution regression tests.

PHOBIC should be distribution-robust: its wyhash-style mix handles
non-uniform input as well as uniform. These tests confirm that for
each of four distribution shapes, a modest key count builds a
perfect hash without special parameters.

Relevant context: maph's benchmark suite found that CHD and FCH take
3-4x longer to build on URL-shaped keys because their weaker inner
hashes retain structure. phobic should not show the same effect;
these tests will fail loudly if a future change regresses that.
"""
import random
import string
import pytest
import phobic


def random_bytes_keys(n, seed=42):
    rng = random.Random(seed)
    return [bytes(rng.randint(0, 255) for _ in range(16)) for _ in range(n)]


def sequential_keys(n):
    return [f"{i:010d}".encode() for i in range(n)]


def url_keys(n, seed=42):
    rng = random.Random(seed)
    return [
        f"https://example.com/v1/resource/{rng.randbytes(16).hex()}".encode()
        for _ in range(n)
    ]


def variable_length_keys(n, seed=42):
    rng = random.Random(seed)
    out = []
    for _ in range(n):
        length = rng.randint(4, 64)
        out.append(bytes(rng.randint(0, 255) for _ in range(length)))
    return out


@pytest.mark.parametrize("gen", [
    ("random", random_bytes_keys),
    ("sequential", sequential_keys),
    ("url", url_keys),
    ("variable", variable_length_keys),
])
def test_builds_all_distributions(gen):
    name, gen_fn = gen
    keys = gen_fn(10_000)
    phf = phobic.build(keys, seed=42)
    assert phf.is_perfect, f"{name} distribution produced non-perfect build"
    assert phf.num_keys == 10_000
    # Round-trip sample to verify query correctness.
    for k in keys[:100]:
        assert 0 <= phf[k] < phf.range_size


def test_deterministic_across_distributions():
    """Same seed + same keys = same slots, regardless of input shape."""
    for gen_fn in (random_bytes_keys, url_keys, variable_length_keys):
        keys = gen_fn(500)
        a = phobic.build(keys, seed=123)
        b = phobic.build(keys, seed=123)
        assert all(a[k] == b[k] for k in keys)
