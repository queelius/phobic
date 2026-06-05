# Efficiency experiments toward 0.4.0 (findings)

Study plan: `.claude/DESIGN_EXPERIMENTS_0_4_0.md`. Harness: `.claude/exp/harness.py`.
Baseline: 0.3.2, 12-core, capped at 1M for sweeps.

## Baseline (E0)

bits/key ~1.165 at 100K-1M (all 5 key shapes); build@12t@1M ~500-950 ms
(shape-dependent; long_str slowest). Determinism OK everywhere.

---

## Phase 1 (zero-recompile measurements)

### S1: shard granularity is a real Pareto curve (free, within v3)

N=1M, varying keys/shard (range/keys fixed at 2.0):

| keys/shard | num_shards | bits/key | build@12t (ms) |
|---:|---:|---:|---:|
| 8K  | 125 | 1.275 | 413 (fastest) |
| 16K (current default) | 63 | 1.165 | 623 |
| 32K | 32 | 1.078 | 842 |
| 64K | 16 | 1.006 | 941 |
| 128K | 8 | 0.944 | 1090 |

The current 16K/shard default is mid-curve. Smaller shards build faster (8K is
34% faster than the default) because per-shard `bucket_size` shrinks and the
pilot search is exponential in bucket size; bigger shards are smaller on disk.
Bucket-size sweep (auto shards, N=1M) is even sharper: bs=8 gives 2.02 bpk /
281 ms; bs=14 gives 1.165 / 628; bs=20 gives 0.823 / 2274.

Implication: the auto schedule is a policy choice on this curve. A
`optimize="speed"|"balanced"|"space"` hint (or a retuned default) is a free,
no-format-change lever. 32K/shard gives 7% smaller bpk at +35% build; 8K gives
34% faster build at +9% bpk.

### S2: pilots have large unused headroom (gates S3, verdict BUILD IT)

At load_factor=0.5, pilots are stored as `uint16` (16 bits) but the values are
small: N=1M, max pilot 4553 (fits in 13 bits), median 57, p99 1505. Pilots are
~0.071/key and dominate bits/key. Encoding alternatives:

| pilot encoding | bpk (pilots) | vs 16-bit |
|---|---:|---:|
| current uint16 | 1.143 | baseline |
| global fixed 13-bit | 0.929 | 19% smaller |
| per-shard fixed-width | 0.863 | 25% smaller |
| 0-order entropy floor | 0.598 | 48% (needs arithmetic/ANS decode) |

Per-shard fixed-width packing is the sweet spot: ~25% smaller pilots, so total
bpk goes 1.165 to ~0.88 (about 24% smaller blobs), and unpacking is a shift+mask
per query (O(1), negligible). Entropy coding wins more but costs a decode per
query, not worth it for a hash table. Verdict: S3 = PHF4 per-shard fixed-width
pilots.

### Q2: the batch-query parallel threshold is badly miscalibrated (SHIPPED REGRESSION)

PHF over 1M keys; warmed; ns/key by batch size:

| batch | serial(1t) | parallel(12t) | default `lookup()` (0.3.2) | serial vs default |
|---:|---:|---:|---:|---:|
| 1,024 | 110 | 4733 | 6945 | serial 63x faster |
| 8,192 | 222 | 857 | 944 | serial 4.2x faster |
| 65,536 | 247 | 311 | 345 | serial 1.4x faster |
| 262,144 | 322 | 238 | 286 | parallel 1.1x faster |
| 1,000,000 | 262 | 229 | 266 | tie |

Per-call `pthread_create` x12 + join + GIL cycling (hundreds of us) dwarfs the
~100 ns/key work until ~256K keys. The threshold is 1024, and 0.3.2 made
`lookup()` parallel-by-default (commit 4248043), so every moderate-batch lookup
(1K-64K keys) is currently 1.4x to 63x slower than serial. This is a regression
in shipped 0.3.2.

Fix (fast-follow, small): raise `PHOBIC_BATCH_THREADING_THRESHOLD` to ~256K
(parallel only helps above that here), and/or a persistent thread pool (followup)
to make parallel pay at smaller batches. Lowest-risk: raise the threshold.

---

## Phase 1 verdict, Phase 2 priorities

1. Q2 threshold fix: urgent, tiny, fixes a shipped regression. (recompile + clean re-measure)
2. S3 PHF4 per-shard fixed-width pilots: ~24% smaller blobs, cheap query. (big C change, gated-in by S2)
3. S1 schedule policy: free `optimize=` hint / retuned default. (no recompile; Python)
4. B1/B2/B3 build compute: arena, parallel distribution, inner loop. (C)
5. Q1 numpy bulk path: cut per-key PyObject overhead on the serial path (the fast path per Q2). (C+Py)
6. SKIP for now: entropy-coded pilots (query-cost), Q3 SIMD (low marginal value, serial query already ~100 ns/key).

---

## Phase 2 (prototypes, measured)

### Q2 fix: raise batch threshold 1024 -> 262144 (SHIP)

One-line change. Re-measured: default `lookup()` now tracks serial speed for
moderate batches (1.0-1.15x) instead of 1.4x-63x slower; still flips to parallel
past 256K. 69 tests pass. Fixes the shipped-0.3.2 regression.

### S3: PHF4 bit-packed pilots (SHIP, headline space win)

Wire format v4 (magic PHF4, 48-byte descriptors with per-shard `pilot_bits`)
packs pilots at the minimum fixed width per shard; unpacked to uint16 in memory
on load, so build/query/determinism are unchanged and the round-trip stays
byte-identical. Measured across 5 key shapes:

| metric | baseline (PHF3) | S3 (PHF4) |
|---|---:|---:|
| bits/key @1M | 1.165 | 0.890 (24% smaller) |
| bits/key @10M | 1.16 | 0.887 (validated) |
| build | unchanged | unchanged |
| query | unchanged | unchanged |
| determinism | OK | OK |

bits/key is deterministic (serialized size / N), so the 24% is timing-independent
and rock-solid. Clean wire break PHF3 -> PHF4 (old blobs rejected), consistent
with the 0.2 -> 0.3 BOHP/PPHF break.

### B2: parallel shard partition (NO-SHIP / optional, honest negative)

Parallelises the count + scatter passes and computes each key's shard once
(stored), preserving per-shard key order so output is byte-identical across
thread counts (determinism gate passes; 69 tests pass). Correct and sound.

BUT the end-to-end win is small. A single `PHOBIC_PROFILE` run suggested the
partition was 971 ms (36%) at 10M, but that run was under load from concurrent
activity. A clean back-to-back A/B (stash B2, rebuild, measure; same machine):

| N | B2 off | B2 on | speedup |
|---:|---:|---:|---:|
| 1M | 243 ms | 226 ms | 1.08x |
| 10M | 2158 ms | 2078 ms | 1.04x |

So ~4-8% end-to-end (noise-limited), because the pilot search dominates the build
and B2 does not touch it; the real (unloaded) partition is a small fraction. B2
also costs ~40 MB transient memory (shard_of) at 10M. Below the 20%-build ship
bar; the complexity + memory are not justified by ~5%. Kept on the experiments
branch as a measured negative; not recommended for the 0.4.0 merge.

Lesson: back-to-back A/B beats single-run profiles on a loaded machine. The
space (S3) and query-ratio (Q2) wins are timing-independent and unaffected.

### B1 / B3 / Q1: not pursued (rationale)

Given B2's modest result and the pilot search dominating an already-parallel
build, further build micro-opts are low-confidence on this noisy machine:
- B1 (per-thread arena): would only help if malloc is contended in the parallel
  section; not isolated, likely small.
- B3 (pilot-search inner loop): the search IS the core algorithm; micro-opts
  uncertain.
- Q1 (numpy bulk query): genuine value for huge-batch users (skips per-key
  PyObject overhead), but additive API scope and Q2 already fixed the common
  moderate-batch case. Deferred as future additive work.

### S1: shard/bucket schedule knob (SHIP as policy, not yet implemented)

Phase 1 showed a free Pareto lever (no format change). Recommend exposing an
`optimize="speed"|"balanced"|"space"` hint (or retuning the default) so callers
can pick a point: e.g. 8K/shard ~34% faster build at +9% bpk; 32K/shard ~7%
smaller bpk at +35% build. Pure Python/schedule change.

---

## Ship recommendation for 0.4.0

- **SHIP: S3 (PHF4, ~24% smaller blobs)** + **Q2 (batch threshold, fixes the
  0.3.2 regression)**. Both are large, robust, low-risk. Together: ~24% smaller
  output and much faster moderate-batch lookups, build/query otherwise unchanged,
  determinism preserved. Wire format breaks PHF3 -> PHF4 (document in MIGRATION).
- **OPTIONAL: S1 schedule knob** (free, additive).
- **DEFER: B2** (correct but ~5%, +40MB), **B1/B3** (low-confidence),
  **Q1 numpy** (additive future work).
