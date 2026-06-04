#!/usr/bin/env python3
"""Phase 1: zero-recompile measurements against the installed phobic.

S1  shard-granularity + bucket_size sweep (bpk <-> build-time curve)
S2  pilot-value distribution + entropy bound (gates whether S3/PHF4 is worth it)
Q2  batch serial-vs-parallel crossover (the right threading threshold)

All run against the currently-installed build; no C changes.
"""
from __future__ import annotations
import math, random, statistics, struct, time, json
import phobic

SEED = 12345

def make_keys(n, seed=SEED):
    rng = random.Random(seed)
    out, seen = [], set()
    while len(out) < n:
        b = rng.randbytes(16)
        if b not in seen:
            seen.add(b); out.append(b)
    return out

def med_build_ms(keys, *, num_shards=None, bucket_size=None, threads=12, reps=3):
    ts = []
    phf = None
    for _ in range(reps):
        t0 = time.perf_counter()
        phf = phobic.build(keys, seed=SEED, num_shards=num_shards,
                           bucket_size=bucket_size, num_threads=threads,
                           assume_unique=True)
        ts.append(time.perf_counter() - t0)
    return statistics.median(ts) * 1e3, phf


# ── S1: shard granularity (vary keys/shard) ───────────────────────────

def s1_shard_granularity(n=1_000_000):
    keys = make_keys(n)
    print(f"\n## S1 shard-granularity sweep, N={n:,}")
    print("target_kps | num_shards | bpk | build@12t(ms) | range/keys")
    rows = []
    for target in (8_000, 16_000, 32_000, 64_000, 128_000):
        ns = max(1, math.ceil(n / target))
        try:
            ms, phf = med_build_ms(keys, num_shards=ns)
            rows.append((target, ns, phf.bits_per_key, ms, phf.range_size / n))
            print(f"{target:>10,} | {ns:>10} | {phf.bits_per_key:.3f} | "
                  f"{ms:>11.1f} | {phf.range_size/n:.3f}")
        except RuntimeError as e:
            print(f"{target:>10,} | {ns:>10} | FAILED: {str(e)[:50]}")
    return rows

def s1_bucket_size(n=1_000_000):
    keys = make_keys(n)
    print(f"\n## S1b explicit bucket_size sweep (auto shards), N={n:,}")
    print("bucket_size | bpk | build@12t(ms) | range/keys")
    rows = []
    for bs in (8, 10, 12, 14, 16, 18, 20):
        try:
            ms, phf = med_build_ms(keys, bucket_size=bs)
            rows.append((bs, phf.bits_per_key, ms))
            print(f"{bs:>11} | {phf.bits_per_key:.3f} | {ms:>11.1f} | "
                  f"{phf.range_size/n:.3f}")
        except RuntimeError as e:
            print(f"{bs:>11} | FAILED: {str(e)[:50]}")
    return rows


# ── S2: pilot value distribution + entropy bound ──────────────────────

def parse_pilots(blob):
    """Yield pilot uint16 values from a PHF3 blob, per shard."""
    assert blob[:4] == b"PHF3", "not a PHF3 blob"
    num_shards = struct.unpack_from("<Q", blob, 24)[0]
    per_shard = []
    for s in range(num_shards):
        d = 56 + 40 * s
        num_buckets = struct.unpack_from("<Q", blob, d + 16)[0]
        poff = struct.unpack_from("<Q", blob, d + 32)[0]
        pilots = list(struct.unpack_from(f"<{num_buckets}H", blob, poff)) if num_buckets else []
        per_shard.append(pilots)
    return per_shard

def s2_pilot_entropy(n=1_000_000):
    keys = make_keys(n)
    print(f"\n## S2 pilot distribution + entropy bound, N={n:,} (load_factor=0.5)")
    phf = phobic.build(keys, seed=SEED, assume_unique=True)
    blob = phf.to_bytes()
    per_shard = parse_pilots(blob)
    allp = [p for shard in per_shard for p in shard]
    total_buckets = len(allp)

    mx = max(allp); mn = min(allp)
    mean = statistics.mean(allp)
    median = statistics.median(allp)
    pct = lambda q: sorted(allp)[min(len(allp)-1, int(q*len(allp)))]

    # current cost: 16 bits/pilot
    cur_bits = 16
    # (a) global fixed bit-width
    gw = max(1, (mx).bit_length())
    # (b) per-shard fixed bit-width
    per_shard_bits = sum(len(s) * max(1, (max(s) if s else 0).bit_length())
                         for s in per_shard)
    # (c) 0-order entropy (ideal arithmetic coding floor)
    from collections import Counter
    cnt = Counter(allp)
    H = -sum((c/total_buckets) * math.log2(c/total_buckets) for c in cnt.values())

    bpk_cur = total_buckets * cur_bits / n
    bpk_global = total_buckets * gw / n
    bpk_pershard = per_shard_bits / n
    bpk_entropy = total_buckets * H / n

    print(f"buckets={total_buckets:,}  pilots/key={total_buckets/n:.3f}")
    print(f"pilot min/median/mean/p90/p99/max = "
          f"{mn}/{median}/{mean:.0f}/{pct(0.90)}/{pct(0.99)}/{mx}")
    print(f"distinct pilot values: {len(cnt)}")
    print(f"  current  16-bit pilots:       bpk_pilots = {bpk_cur:.3f}")
    print(f"  (a) global fixed {gw}-bit:      bpk_pilots = {bpk_global:.3f}  "
          f"({100*(1-bpk_global/bpk_cur):.0f}% smaller)")
    print(f"  (b) per-shard fixed-width:     bpk_pilots = {bpk_pershard:.3f}  "
          f"({100*(1-bpk_pershard/bpk_cur):.0f}% smaller)")
    print(f"  (c) 0-order entropy floor:     bpk_pilots = {bpk_entropy:.3f}  "
          f"({100*(1-bpk_entropy/bpk_cur):.0f}% smaller)  H={H:.2f} bits")
    print(f"  NOTE total bpk now {phf.bits_per_key:.3f}; pilots are the bulk.")
    return {"n": n, "buckets": total_buckets, "max": mx, "mean": mean,
            "H": H, "bpk_cur": bpk_cur, "bpk_global": bpk_global,
            "bpk_pershard": bpk_pershard, "bpk_entropy": bpk_entropy}


# ── Q2: batch serial-vs-parallel crossover ────────────────────────────

def q2_threshold(n=1_000_000):
    keys = make_keys(n)
    phf = phobic.build(keys, seed=SEED, assume_unique=True)
    print(f"\n## Q2 batch serial-vs-parallel crossover (PHF over N={n:,})")
    print("batch | serial ns/key | parallel(12) ns/key | speedup")
    rows = []
    for bs in (256, 512, 1024, 2048, 4096, 16384, 65536, 262144):
        sample = keys[:bs]
        reps = 20 if bs <= 4096 else 8
        def t(nt):
            xs = []
            for _ in range(reps):
                t0 = time.perf_counter()
                phf.lookup(sample, num_threads=nt)
                xs.append((time.perf_counter() - t0) / bs)
            return statistics.median(xs) * 1e9
        ser = t(1); par = t(12)
        rows.append((bs, ser, par, ser/par))
        print(f"{bs:>6} | {ser:>13.0f} | {par:>19.0f} | {ser/par:>6.2f}x")
    return rows


if __name__ == "__main__":
    out = {}
    out["s1_shard_100k"] = s1_shard_granularity(100_000)
    out["s1_shard_1m"] = s1_shard_granularity(1_000_000)
    out["s1_bucket_1m"] = s1_bucket_size(1_000_000)
    out["s2_100k"] = s2_pilot_entropy(100_000)
    out["s2_1m"] = s2_pilot_entropy(1_000_000)
    out["q2_1m"] = q2_threshold(1_000_000)
    with open(".claude/exp/phase1.json", "w") as f:
        json.dump(out, f, indent=2, default=list)
    print("\nwrote .claude/exp/phase1.json")
