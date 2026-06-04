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
