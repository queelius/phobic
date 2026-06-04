"""Core API tests for phobic."""
from __future__ import annotations

import copy
import random

import phobic
import pytest


# ── small helpers ─────────────────────────────────────────────────────


def make_keys(n: int, prefix: bytes = b"k_") -> list[bytes]:
    return [prefix + f"{i:010d}".encode() for i in range(n)]


# ── basic build and query ─────────────────────────────────────────────


def test_basic_build_and_scalar_query():
    keys = make_keys(200)
    phf = phobic.build(keys, seed=0)
    slots = {phf[k] for k in keys}
    assert len(slots) == 200
    assert all(0 <= s < phf.range_size for s in slots)


def test_str_keys_auto_encoded():
    keys = ["alice", "bob", "charlie", "diana"]
    phf = phobic.build(keys, seed=0)
    slots = {phf[k] for k in keys}
    assert len(slots) == len(keys)


def test_str_and_bytes_equivalent_at_same_seed():
    str_keys = ["a", "b", "c"]
    byte_keys = [k.encode("utf-8") for k in str_keys]
    p_str = phobic.build(str_keys, seed=7)
    p_byt = phobic.build(byte_keys, seed=7)
    assert p_str.to_bytes() == p_byt.to_bytes()


def test_batch_lookup_matches_scalar():
    keys = make_keys(500)
    phf = phobic.build(keys, seed=42)
    assert phf.lookup(keys) == [phf[k] for k in keys]


def test_lookup_accepts_str_and_bytes_mix():
    phf = phobic.build([b"a", b"b", b"c"], seed=1)
    a = phf.lookup(["a", "b", "c"])
    b = phf.lookup([b"a", b"b", b"c"])
    assert a == b


def test_lookup_num_threads_does_not_change_results():
    keys = make_keys(20_000)
    phf = phobic.build(keys, seed=0, num_shards=8, num_threads=8)
    assert phf.lookup(keys, num_threads=1) == phf.lookup(keys, num_threads=8)


# ── load_factor semantics ─────────────────────────────────────────────


def test_load_factor_default_is_0_5():
    keys = make_keys(1000)
    phf = phobic.build(keys, seed=0)
    # range_size = ceil(num_keys / load_factor) per shard, summed.
    # At default load_factor=0.5 with single-shard, range_size == 2 * N.
    # For multi-shard the per-shard range may have rounding, so allow a
    # tiny slack at the per-shard level.
    assert 0.45 <= len(phf) / phf.range_size <= 0.55


def test_load_factor_extremes():
    keys = make_keys(200)
    loose = phobic.build(keys, load_factor=0.1, seed=5)
    tight = phobic.build(keys, load_factor=0.95, seed=5)
    # loose has more headroom -> bigger range
    assert loose.range_size > tight.range_size


def test_load_factor_zero_rejected():
    with pytest.raises(ValueError, match="load_factor"):
        phobic.build(["a", "b"], load_factor=0.0)


def test_load_factor_above_one_rejected():
    with pytest.raises(ValueError, match="load_factor"):
        phobic.build(["a", "b"], load_factor=1.1)


def test_load_factor_negative_rejected():
    with pytest.raises(ValueError, match="load_factor"):
        phobic.build(["a", "b"], load_factor=-0.5)


def test_load_factor_one_mphf_at_easy_n():
    """load_factor=1.0 (MPHF) is hardest to build but should succeed for
    small N with enough retries."""
    keys = make_keys(50)
    phf = phobic.build(keys, load_factor=1.0, seed=0, max_retries=200)
    assert phf.range_size == 50  # exactly minimal


# ── num_shards ────────────────────────────────────────────────────────


def test_num_shards_explicit():
    keys = make_keys(2000)
    phf = phobic.build(keys, num_shards=8, seed=0)
    assert phf.num_shards == 8


def test_num_shards_auto_small_n_is_one():
    """Below ~32K keys, auto picks single shard."""
    keys = make_keys(1000)
    phf = phobic.build(keys, seed=0)
    assert phf.num_shards == 1


def test_num_shards_auto_large_n_is_multi():
    """Above ~32K keys, auto picks multiple shards."""
    keys = make_keys(50_000)
    phf = phobic.build(keys, seed=0)
    assert phf.num_shards > 1


def test_num_shards_zero_rejected():
    with pytest.raises(ValueError, match="num_shards"):
        phobic.build(["a"], num_shards=0)


def test_num_shards_one_is_legal():
    keys = make_keys(100)
    phf = phobic.build(keys, num_shards=1, seed=0)
    assert phf.num_shards == 1
    slots = {phf[k] for k in keys}
    assert len(slots) == len(keys)


# ── num_threads ───────────────────────────────────────────────────────


def test_num_threads_does_not_affect_output():
    keys = make_keys(20_000)
    p1 = phobic.build(keys, seed=42, num_shards=8, num_threads=1).to_bytes()
    p4 = phobic.build(keys, seed=42, num_shards=8, num_threads=4).to_bytes()
    p8 = phobic.build(keys, seed=42, num_shards=8, num_threads=8).to_bytes()
    assert p1 == p4 == p8


def test_auto_num_shards_thread_independent():
    """The auto-shard heuristic must be a pure function of N, not of
    num_threads. Otherwise the same `phobic.build(keys, seed=...)` call
    produces different output on machines with different CPU counts:
    a real cross-machine reproducibility hazard."""
    keys = make_keys(100_000)
    p1 = phobic.build(keys, seed=42, num_threads=1)
    p4 = phobic.build(keys, seed=42, num_threads=4)
    p12 = phobic.build(keys, seed=42, num_threads=12)
    assert p1.num_shards == p4.num_shards == p12.num_shards
    assert p1.to_bytes() == p4.to_bytes() == p12.to_bytes()


def test_num_threads_zero_rejected():
    with pytest.raises(ValueError, match="num_threads"):
        phobic.build(["a"], num_threads=0)


# ── bucket_size ───────────────────────────────────────────────────────


def test_bucket_size_explicit_small_builds():
    keys = make_keys(2000)
    phf = phobic.build(keys, bucket_size=4, seed=0)
    slots = {phf[k] for k in keys}
    assert len(slots) == len(keys)


def test_bucket_size_zero_rejected():
    with pytest.raises(ValueError, match="bucket_size"):
        phobic.build(["a"], bucket_size=0)


# ── seed semantics ────────────────────────────────────────────────────


def test_same_seed_byte_identical_output():
    keys = make_keys(500)
    a = phobic.build(keys, seed=12345, num_shards=4)
    b = phobic.build(keys, seed=12345, num_shards=4)
    assert a.to_bytes() == b.to_bytes()


def test_different_seeds_differ():
    keys = make_keys(500)
    a = phobic.build(keys, seed=1, num_shards=4)
    b = phobic.build(keys, seed=2, num_shards=4)
    assert a.to_bytes() != b.to_bytes()


def test_seed_out_of_range_rejected():
    with pytest.raises(ValueError, match="seed"):
        phobic.build(["a", "b"], seed=2**64)


# ── input validation ──────────────────────────────────────────────────


def test_empty_keys_rejected():
    with pytest.raises((ValueError, RuntimeError)):
        phobic.build([])


def test_duplicate_keys_rejected():
    with pytest.raises(ValueError, match="unique"):
        phobic.build(["a", "b", "a"])


def test_assume_unique_skips_uniqueness_check():
    """assume_unique=True must NOT raise on a duplicate-key input.
    The build will fail or produce a non-PHF, but that's the caller's
    responsibility; phobic just doesn't validate."""
    keys = [b"a", b"b", b"a"]   # duplicate
    # We don't assert success here; with duplicates, the build may fail
    # in the C pilot search. We only assert that the duplicate-detection
    # error is *not* raised.
    try:
        phobic.build(keys, assume_unique=True)
    except ValueError as e:
        assert "unique" not in str(e), "assume_unique=True must skip the uniqueness check"
    except RuntimeError:
        pass  # the C build can fail; that's acceptable


def test_assume_unique_default_still_validates():
    """Without assume_unique, the duplicate check still fires."""
    with pytest.raises(ValueError, match="unique"):
        phobic.build([b"a", b"b", b"a"], assume_unique=False)


def test_assume_unique_correctness_on_unique_keys():
    """assume_unique=True on a genuinely unique key set produces an
    identical PHF to the default mode (the only difference is whether
    the uniqueness check is run)."""
    keys = make_keys(2000)
    p_default = phobic.build(keys, seed=42).to_bytes()
    p_assumed = phobic.build(keys, seed=42, assume_unique=True).to_bytes()
    assert p_default == p_assumed


def test_max_retries_below_one_rejected():
    with pytest.raises(ValueError, match="max_retries"):
        phobic.build(["a"], max_retries=0)


def test_max_retries_one_succeeds_for_easy_build():
    """Single attempt should be enough for an obviously easy build."""
    keys = make_keys(100)
    phf = phobic.build(keys, max_retries=1, seed=0, load_factor=0.25)
    assert len(phf) == 100


# ── build-failure error message ───────────────────────────────────────


def test_build_failure_message_mentions_diagnostics():
    """Asking for an unrealistic build (MPHF with one retry on a tough seed)
    should fail with a message containing useful tuning hints."""
    keys = make_keys(5000)
    with pytest.raises(RuntimeError) as exc:
        phobic.build(keys, load_factor=1.0, seed=0, max_retries=1, num_shards=1,
                     bucket_size=20)
    msg = str(exc.value)
    assert "PHOBIC" in msg
    assert "load_factor" in msg or "shard" in msg


# ── introspection ─────────────────────────────────────────────────────


def test_len_returns_num_keys():
    keys = make_keys(123)
    phf = phobic.build(keys, seed=0)
    assert len(phf) == 123


def test_repr_includes_key_metrics():
    keys = make_keys(50)
    phf = phobic.build(keys, seed=0)
    r = repr(phf)
    assert "PHF" in r
    assert "num_keys=50" in r
    assert "num_shards=" in r
    assert "bits_per_key=" in r


def test_bits_per_key_is_positive():
    keys = make_keys(10_000)
    phf = phobic.build(keys, seed=0)
    assert phf.bits_per_key > 0
    assert phf.bits_per_key < 50  # sanity


def test_range_size_matches_to_bytes():
    """range_size should match what serialize/deserialize report."""
    keys = make_keys(200)
    phf = phobic.build(keys, seed=0)
    blob = phf.to_bytes()
    phf2 = phobic.from_bytes(blob)
    assert phf.range_size == phf2.range_size
    assert phf.num_shards == phf2.num_shards
    assert len(phf) == len(phf2)


# ── serialization (wire format v3) ────────────────────────────────────


def test_serialization_round_trip_single_shard():
    keys = make_keys(100)
    phf = phobic.build(keys, num_shards=1, seed=42)
    phf2 = phobic.from_bytes(phf.to_bytes())
    for k in keys:
        assert phf[k] == phf2[k]


def test_serialization_round_trip_multi_shard():
    keys = make_keys(5000)
    phf = phobic.build(keys, num_shards=8, seed=42)
    phf2 = phobic.from_bytes(phf.to_bytes())
    for k in keys:
        assert phf[k] == phf2[k]


def test_wire_magic_is_phf3():
    """Wire format v3 magic on the wire is b'PHF3'."""
    phf = phobic.build([b"x"], seed=0)
    assert phf.to_bytes()[:4] == b"PHF3"


def test_wire_format_rejects_unknown_magic():
    with pytest.raises(ValueError):
        phobic.from_bytes(b"XXXX" + b"\x00" * 100)


def test_wire_format_rejects_truncated():
    phf = phobic.build([b"x", b"y", b"z"], seed=0)
    blob = phf.to_bytes()
    with pytest.raises(ValueError):
        phobic.from_bytes(blob[:8])


def test_wire_format_rejects_old_v2_phob():
    """0.2.0 'BOHP' magic is not readable by 0.3.0 (clean break)."""
    with pytest.raises(ValueError):
        phobic.from_bytes(b"BOHP" + b"\x02\x00\x00\x00" + b"\x00" * 100)


def test_pilots_offset_is_8_byte_aligned():
    """The wire format guarantees pilot blocks start on 8-byte boundaries
    (mmap-friendly). Decode the descriptor table and verify."""
    import struct
    keys = make_keys(500)
    phf = phobic.build(keys, num_shards=4, seed=0)
    blob = phf.to_bytes()
    num_shards = struct.unpack_from("<Q", blob, 24)[0]
    for s in range(num_shards):
        pilots_off = struct.unpack_from("<Q", blob, 56 + 40 * s + 32)[0]
        assert pilots_off % 8 == 0, f"shard {s} pilots_offset={pilots_off} not 8-byte aligned"


# ── equality, copy, cross-process protocol ───────────────────────────


def test_equality_via_bytes():
    keys = make_keys(50)
    a = phobic.build(keys, seed=42)
    b = phobic.build(keys, seed=42)
    c = phobic.build(keys, seed=43)
    assert a == b
    assert not (a == c)


def test_phf_is_not_hashable():
    """PHF is intentionally unhashable: the prior implementation forced
    a full re-serialization on every call. If a user needs a hash, they
    can hash phf.to_bytes() explicitly."""
    keys = make_keys(50)
    phf = phobic.build(keys, seed=42)
    with pytest.raises(TypeError, match="unhashable"):
        hash(phf)
    with pytest.raises(TypeError, match="unhashable"):
        {phf}  # set construction calls hash


def test_copy_deepcopy():
    keys = make_keys(50)
    phf = phobic.build(keys, seed=42)
    phf2 = copy.deepcopy(phf)
    assert phf == phf2
    for k in keys:
        assert phf[k] == phf2[k]


def test_reduce_protocol_round_trip():
    """__reduce__ is the protocol used by copy/deepcopy and cross-process
    transport (multiprocessing.Pool worker hand-off)."""
    keys = make_keys(50)
    phf = phobic.build(keys, seed=42)
    factory, args = phf.__reduce__()
    restored = factory(*args)
    assert restored == phf
    for k in keys:
        assert phf[k] == restored[k]


# ── from_bytes accepts bytes-like ─────────────────────────────────────


def test_from_bytes_accepts_bytearray():
    phf = phobic.build([b"a", b"b", b"c"], seed=0)
    blob = phf.to_bytes()
    phf2 = phobic.from_bytes(bytearray(blob))
    assert phf == phf2


def test_from_bytes_accepts_memoryview():
    phf = phobic.build([b"a", b"b", b"c"], seed=0)
    blob = phf.to_bytes()
    phf2 = phobic.from_bytes(memoryview(blob))
    assert phf == phf2


# ── distribution robustness ───────────────────────────────────────────


@pytest.mark.parametrize("gen", [
    pytest.param(lambda n: [bytes(random.Random(i).randbytes(16)) for i in range(n)],
                 id="random_bytes_16"),
    pytest.param(lambda n: [f"{i:010d}".encode() for i in range(n)],
                 id="sequential_int"),
    pytest.param(lambda n: [f"https://example.com/path/{i}".encode() for i in range(n)],
                 id="url_like"),
    pytest.param(lambda n: [(b"x" * (i % 32)) + str(i).encode() for i in range(n)],
                 id="variable_length"),
])
def test_builds_across_distributions(gen):
    keys = gen(10_000)
    keys = list(dict.fromkeys(keys))  # drop accidental duplicates
    phf = phobic.build(keys, seed=0)
    slots = {phf[k] for k in keys[:500]}
    assert len(slots) == 500


# ── parallel batch query thread safety ────────────────────────────────


def test_concurrent_lookups_are_consistent():
    """The same PHF queried concurrently from multiple threads must yield
    the same per-key answers (PHF is read-only after construction)."""
    import threading
    keys = make_keys(2000)
    phf = phobic.build(keys, seed=0)
    expected = phf.lookup(keys, num_threads=1)
    results = [None] * 4
    def worker(idx):
        results[idx] = phf.lookup(keys, num_threads=2)
    ts = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for t in ts: t.start()
    for t in ts: t.join()
    for r in results:
        assert r == expected


# ── regression: deserialize hardening & empty-shard query ─────────────


def test_wire_format_rejects_overflowing_pilot_offset():
    """A crafted pilots_offset near UINT64_MAX must not slip past the
    deserializer's bounds check by integer overflow (s_poff + pilot_bytes
    wrapping past 2**64). Regression for an OOB read in phobic_deserialize."""
    import struct
    phf = phobic.build(make_keys(100), num_shards=1, seed=0)
    blob = bytearray(phf.to_bytes())
    num_buckets = struct.unpack_from("<Q", blob, 56 + 16)[0]
    assert num_buckets >= 4  # need pilot_bytes >= 8 so the offset can wrap
    # pilots_offset is the 5th u64 of shard descriptor 0 (descriptor at 56).
    struct.pack_into("<Q", blob, 56 + 32, (1 << 64) - 8)
    with pytest.raises(ValueError):
        phobic.from_bytes(bytes(blob))


def test_query_on_empty_shard_does_not_crash():
    """A PHF built with more shards than keys has empty shards. Querying a
    key not in the build set may hash to an empty shard; that must return a
    valid slot, not crash (SIGFPE / NULL deref). Runs in a subprocess
    because the pre-fix bug aborts the interpreter."""
    import subprocess
    import sys
    code = (
        "import phobic\n"
        "phf = phobic.build([b'a', b'b'], num_shards=64, seed=0)\n"
        "for i in range(200):\n"
        "    s = phf[b'miss-%d' % i]\n"
        "    assert 0 <= s < phf.range_size, s\n"
        "print('OK')\n"
    )
    r = subprocess.run([sys.executable, "-c", code],
                       capture_output=True, text=True)
    assert r.returncode == 0, (
        f"empty-shard query crashed: rc={r.returncode} "
        f"stdout={r.stdout!r} stderr={r.stderr!r}"
    )
    assert "OK" in r.stdout


def test_lookup_default_threads_matches_scalar_large_batch():
    """lookup() with default num_threads auto-parallelises above the internal
    threading threshold; the parallel path must agree with scalar queries."""
    keys = make_keys(5000)  # well above the 1024-key threading threshold
    phf = phobic.build(keys, seed=7)
    assert phf.lookup(keys) == [phf[k] for k in keys]


def test_build_and_lookup_accept_bytes_subclass():
    """A bytes subclass works as a key through both build() and lookup():
    the shared key-encoding helper passes such instances straight to C."""
    class MyKey(bytes):
        pass
    keys = [MyKey(f"k{i}".encode()) for i in range(50)]
    phf = phobic.build(keys, seed=0)
    assert phf.lookup(keys) == [phf[k] for k in keys]
    plain = [bytes(k) for k in keys]
    assert phf.lookup(plain) == phf.lookup(keys)
    assert phf.to_bytes() == phobic.build(plain, seed=0).to_bytes()


# ── empty-shard robustness: batch path + round-trip ───────────────────


def test_query_on_empty_shard_batch_does_not_crash():
    """Same empty-shard hazard through the parallel batch path, where a crash
    would fire inside a GIL-released pthread worker and abort the interpreter.
    Subprocess-isolated like the scalar variant."""
    import subprocess, sys
    code = (
        "import phobic\n"
        "phf = phobic.build([b'a', b'b'], num_shards=64, seed=0)\n"
        "strangers = [b'miss-%d' % i for i in range(4000)]\n"  # > 1024 threshold
        "slots = phf.lookup(strangers, num_threads=4)\n"
        "assert all(0 <= s < phf.range_size for s in slots)\n"
        "print('OK')\n"
    )
    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert r.returncode == 0, (
        f"empty-shard batch query crashed: rc={r.returncode} stderr={r.stderr!r}")
    assert "OK" in r.stdout


def test_empty_shard_build_round_trips():
    """A build with empty shards must serialize and deserialize cleanly
    (determinism preserved) and agree on in-set keys."""
    keys = [f"k{i}".encode() for i in range(50)]
    phf = phobic.build(keys, num_shards=400, seed=1)
    phf2 = phobic.from_bytes(phf.to_bytes())
    assert phf == phf2
    for k in keys:
        assert phf[k] == phf2[k]


# ── deserialize rejects malformed-but-structurally-plausible blobs ────


def _tamper(blob, *edits):
    """Apply (offset, u64_value) edits to a copy of blob. Header is 56 bytes;
    shard-0 descriptor starts at 56 (seed@+0, bsize@+8, nbck@+16, range@+24,
    poff@+32)."""
    import struct
    b = bytearray(blob)
    for off, val in edits:
        struct.pack_into("<Q", b, off, val)
    return bytes(b)


def test_deserialize_rejects_zero_range_with_buckets():
    """A populated shard (num_buckets>0) with range==0 would divide by zero in
    slot_with_pilot at query time; deserialize must reject it."""
    phf = phobic.build([b"a", b"b", b"c", b"d"], load_factor=0.9, seed=42)
    bad = _tamper(phf.to_bytes(), (56 + 24, 0), (16, 0))  # shard0 range, total_range
    with pytest.raises(ValueError):
        phobic.from_bytes(bad)


def test_deserialize_rejects_zero_buckets_with_range():
    """A populated shard mislabeled with num_buckets==0 would divide by zero in
    bucket_for at query time; deserialize must reject it."""
    phf = phobic.build([b"a", b"b", b"c", b"d"], load_factor=0.9, seed=42)
    bad = _tamper(phf.to_bytes(), (56 + 16, 0))  # shard0 num_buckets
    with pytest.raises(ValueError):
        phobic.from_bytes(bad)


# ── key-domain consistency across build / lookup / __getitem__ ────────


def test_build_rejects_int_keys():
    """int keys are a footgun (bytes(5) is a 5-byte zero buffer, not 5);
    reject them loudly rather than silently mangling."""
    with pytest.raises(TypeError):
        phobic.build([1, 2, 3])


def test_getitem_accepts_bytearray_and_memoryview():
    """phf[key] accepts the same bytes-like domain as build()/lookup()."""
    keys = [b"alpha", b"beta", b"gamma"]
    phf = phobic.build(keys, seed=1)
    for k in keys:
        expected = phf[k]
        assert phf[bytearray(k)] == expected
        assert phf[memoryview(k)] == expected


def test_lookup_validates_num_threads():
    """lookup() enforces the same num_threads >= 1 contract as build()."""
    phf = phobic.build([b"a", b"b", b"c"], seed=1)
    with pytest.raises(ValueError, match="num_threads"):
        phf.lookup([b"a"], num_threads=-5)


# ── strict MPHF: load_factor=1.0 is minimal-or-fails ──────────────────


@pytest.mark.parametrize("n", [50, 100])
def test_mphf_success_is_always_minimal(n):
    """A successful load_factor=1.0 build must have range_size == num_keys
    exactly. The retry path must never relax the range for an exact MPHF."""
    keys = [f"k{i}".encode() for i in range(n)]
    succeeded = 0
    for seed in range(6):
        try:
            phf = phobic.build(keys, load_factor=1.0, seed=seed, max_retries=200)
        except RuntimeError:
            continue  # MPHF can legitimately fail; strict contract
        assert phf.range_size == n, (
            f"non-minimal MPHF: n={n} seed={seed} range={phf.range_size}")
        succeeded += 1
    assert succeeded > 0, f"no MPHF build succeeded for n={n} (test inconclusive)"


@pytest.mark.slow
def test_mphf_never_silently_non_minimal():
    """Regression: load_factor=1.0 must never return range_size > num_keys.
    This multi-shard config previously relaxed to range_size=2197 for 2000
    keys after the per-shard retry budget crossed the LF-bump interval. A
    failed (raised) build is the acceptable strict outcome; a non-minimal
    success is not. Marked slow: the strict-fail path exhausts the full
    per-shard retry budget, which is intentionally expensive."""
    keys = [f"k{i}".encode() for i in range(2000)]
    for seed in range(8):
        try:
            phf = phobic.build(keys, load_factor=1.0, seed=seed,
                               num_shards=4, max_retries=300)
        except RuntimeError:
            continue
        assert phf.range_size == len(phf), (
            f"MPHF silently non-minimal: range_size={phf.range_size} "
            f"num_keys={len(phf)} (seed={seed})")
