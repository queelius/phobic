# Efficiency experiments toward 0.4.0

Plan of record for a Pareto-frontier optimization study of phobic
(compute + space). Approved scope: optimize build compute, query throughput,
and space (bits/key) together; wire format and API are both open (a v4 layout
and optional additive APIs are on the table); broad workload; sweeps capped at
1M keys with shortlisted winners validated at 10M.

This is research. The installed package and `master` (0.3.2) stay pristine.
Variants are built and measured in isolation; only proven, approved winners
get implemented afterward (a separate design/plan/implement cycle).

## Baseline (from prior sweeps, 0.3.x, 12-core, 16B keys, load_factor=0.5)

| N | num_shards | bits/key | build @12t | batch ns/key |
|---:|---:|---:|---:|---:|
| 100K | 7 | 1.17 | 26 ms | ~140 |
| 1M | 63 | 1.17 | ~250 ms | - |
| 10M | 625 | 1.16 | ~1.18 s (assume_unique) | - |

Central trade-off: `bucket_size` / shard granularity governs bpk vs build time.
Bigger buckets -> fewer pilots -> lower bpk, but exponentially harder pilot
search. Current auto schedule (~16K keys/shard, bucket_size ~= 14) trades
~0.2 bpk for build parallelism (a 100K single-shard build hit 0.95 bpk).

## Measurement protocol

- Harness measures, per (key-shape x N): build time (1/4/12 threads),
  bits/key, scalar ns/key, batch ns/key, peak build RSS.
- Key shapes: 16B-uniform, short-str, long-str, int-as-bytes, skewed.
- N: 1K, 100K, 1M for sweeps; 10M for winner validation.
- >=5 reps, median. Fixed seeds.
- **Determinism gate (hard):** any variant must keep `to_bytes()` byte-identical
  across thread counts and machines, or it is disqualified regardless of speed.
- "Win worth shipping": build >=20% faster at equal bpk; OR bpk >=15% smaller at
  <=10% build cost; OR batch query >=2x. Smaller wins reported, not auto-recommended.

## Experiment matrix

### E0 - Baseline harness
Reproducible bench producing a structured table; the substrate for all deltas.

### Space (bits/key)
- **S1 Shard-granularity sweep** - keys/shard in {8K,16K,32K,64K}; map
  bpk <-> build-time <-> parallelism. Within v3 (schedule tunable).
- **S2 Pilot-value distribution + entropy bound** - measure actual pilot values;
  compute the information-theoretic bpk floor. Bounds S3's potential before building.
- **S3 Compact pilot encoding (PHF4 prototype)** - per-shard fixed bit-width
  (`ceil(log2(max_pilot+1))`) and/or var-width packing. Big space lever; needs a
  v4 layout. Measure bpk win and the query-cost of unpacking.

### Build compute
- **B1 Per-thread arena allocator** - remove per-shard malloc/free churn
  (contention at 12 threads / 10M).
- **B2 Parallel shard-distribution** - count + scatter passes are single-threaded
  (~50 ms @ 10M).
- **B3 Pilot-search inner loop** - the hot path: O(bsize^2) candidate dedup ->
  bitset/sorted; early-exit; remove the known dead store.

### Query compute
- **Q1 numpy bulk path** - `lookup` over an ndarray, skipping per-key
  PyBytes/PyLong (predicted ~4-5x). Optional additive API; zero-dep core preserved.
- **Q2 batch-threshold re-tune** - re-measure the 1024 threading threshold.
- **Q3 SIMD `slot_with_pilot`** - vectorize the batch hash/slot computation.

## Deliverable

`.claude/EXPERIMENTS_0_4_0.md`: Pareto curves + a ranked ship/no-ship list with
measured deltas. Implementing winners is a separate approval after the data lands.
