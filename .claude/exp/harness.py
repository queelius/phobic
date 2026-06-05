#!/usr/bin/env python3
"""phobic efficiency harness (E0).

Reusable measurement substrate for the 0.4.0 efficiency study. Every variant
(baseline and prototypes) runs THIS file unchanged against its own compiled
`phobic`, emitting a JSON record tagged with a --label. Synthesis diffs the
JSONs across variants.

Metrics per (key-shape x N): build time at 1/4/12 threads (median of reps),
bits/key, scalar query ns/key, batch query ns/key, and a determinism gate
(byte-identical to_bytes across thread counts).

Usage:
    python harness.py --label baseline --out baseline.json
    python harness.py --label baseline --out b.json --max-n 100000 --reps 3
"""
from __future__ import annotations

import argparse
import json
import random
import statistics
import sys
import time

import phobic


# ── key generators (deterministic, unique by construction) ────────────

def keys_uniform16(n, seed):
    rng = random.Random(seed)
    # 16 random bytes; collision-free at these N. dedup defensively.
    out, seen = [], set()
    while len(out) < n:
        b = rng.randbytes(16)
        if b not in seen:
            seen.add(b); out.append(b)
    return out

def keys_short_str(n, seed):
    return [f"k{i}".encode() for i in range(n)]

def keys_long_str(n, seed):
    return [f"namespace/long/key/path/segment_{i:020d}/suffix".encode()
            for i in range(n)]

def keys_int_bytes(n, seed):
    return [i.to_bytes(8, "little") for i in range(n)]

def keys_skewed(n, seed):
    # shared prefixes (clustered): stresses the hash distribution
    return [f"user/{i % 1000}/bucket/{i // 1000}/item/{i}".encode()
            for i in range(n)]

SHAPES = {
    "uniform16": keys_uniform16,
    "short_str": keys_short_str,
    "long_str":  keys_long_str,
    "int_bytes": keys_int_bytes,
    "skewed":    keys_skewed,
}


def adaptive_reps(n, base):
    if n <= 1_000:      return base + 2
    if n <= 100_000:    return base
    if n <= 1_000_000:  return max(3, base - 2)
    return max(2, base - 3)


def time_build(keys, seed, num_threads, reps):
    ts = []
    phf = None
    for _ in range(reps):
        t0 = time.perf_counter()
        phf = phobic.build(keys, seed=seed, num_threads=num_threads,
                           assume_unique=True)
        ts.append(time.perf_counter() - t0)
    return statistics.median(ts), phf


def time_query(phf, sample, reps):
    # scalar
    scal = []
    for _ in range(reps):
        t0 = time.perf_counter()
        for k in sample:
            phf[k]
        scal.append((time.perf_counter() - t0) / len(sample))
    # batch
    bat = []
    for _ in range(reps):
        t0 = time.perf_counter()
        phf.lookup(sample)
        bat.append((time.perf_counter() - t0) / len(sample))
    return statistics.median(scal) * 1e9, statistics.median(bat) * 1e9


def measure(shape, n, seed, threads, base_reps):
    gen = SHAPES[shape]
    keys = gen(n, seed)
    reps = adaptive_reps(n, base_reps)

    build_ms = {}
    phf_ref = None
    blobs = {}
    for t in threads:
        med, phf = time_build(keys, seed, t, reps)
        build_ms[str(t)] = med * 1e3
        blobs[t] = phf.to_bytes()
        if t == threads[0]:
            phf_ref = phf

    # determinism gate: byte-identical across thread counts
    determinism_ok = len(set(blobs.values())) == 1

    bpk = phf_ref.bits_per_key
    range_size = phf_ref.range_size
    num_shards = phf_ref.num_shards

    sample = keys[: min(n, 5000)]
    scalar_ns, batch_ns = time_query(phf_ref, sample, max(3, base_reps))

    return {
        "shape": shape, "n": n, "reps": reps,
        "build_ms": build_ms,
        "bits_per_key": bpk,
        "range_size": range_size,
        "num_shards": num_shards,
        "range_over_keys": range_size / n,
        "scalar_ns_per_key": scalar_ns,
        "batch_ns_per_key": batch_ns,
        "determinism_ok": determinism_ok,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-n", type=int, default=1_000_000)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--threads", default="1,4,12")
    ap.add_argument("--shapes", default="all")
    args = ap.parse_args()

    threads = [int(x) for x in args.threads.split(",")]
    shapes = list(SHAPES) if args.shapes == "all" else args.shapes.split(",")
    ns = [n for n in (1_000, 100_000, 1_000_000, 10_000_000) if n <= args.max_n]

    results = []
    for shape in shapes:
        for n in ns:
            r = measure(shape, n, args.seed, threads, args.reps)
            results.append(r)
            print(f"[{args.label}] {shape:10s} N={n:>9,} "
                  f"bpk={r['bits_per_key']:.3f} "
                  f"build@{threads[-1]}t={r['build_ms'][str(threads[-1])]:.1f}ms "
                  f"batch={r['batch_ns_per_key']:.0f}ns "
                  f"det={'OK' if r['determinism_ok'] else 'FAIL'}",
                  flush=True)

    out = {
        "label": args.label,
        "phobic_version": getattr(phobic, "__version__", "unknown"),
        "threads": threads,
        "results": results,
    }
    with open(args.out, "w") as f:
        json.dump(out, f, indent=2)
    print(f"[{args.label}] wrote {args.out} ({len(results)} rows)", flush=True)


if __name__ == "__main__":
    main()
