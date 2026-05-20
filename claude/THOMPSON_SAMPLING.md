# Bandit-based compression tier selection — learnings

This is a working notebook on the multi-armed-bandit tier selector for smash's
compression decision (`SMASH_UCB=1`). Despite the filename, what's actually
implemented is **UCB**, not Thompson sampling — the title was kept from the
original conversation thread that explored both. See "Why not Thompson sampling"
near the end.

The code lives in:
- `src/compress/compression_roi.h` — `selectTierUCB`, `ucbReward`, `UcbVariant`,
  `kUcbRewardMaxBytesPerUs`, `kUcbMinPullsDefault`.
- `src/compress/compressor_thread.h` — `SizeClassStats::arm_pulls/arm_mean/arm_m2`,
  `recordReward`, the UCB branch in `compressPage()`.

Env-vars:
- `SMASH_UCB=1` — opt in (default off; ROI cost/benefit model otherwise).
- `SMASH_UCB_VARIANT={1|2}` — 1 = UCB1-Tuned (default), 2 = UCB-V.
- `SMASH_UCB_MIN_PULLS=N` — force-pull each arm N times before the formula kicks
  in. Default 4. Higher = more deep-tier exploration on cold-start.

## Reward design

`reward = clamp(bytes_saved / compress_us, 0, kUcbRewardMaxBytesPerUs) / kUcbRewardMaxBytesPerUs`

with `kUcbRewardMaxBytesPerUs = 16384.0`. Failed/poor compressions get reward 0;
maximally-efficient compressions saturate at 1. The reward is attributed to the
**originally chosen** arm even when the fast→deep ratio-gate fallback fires —
the bandit has to learn the actual cost of its decision, not the cost of a
later corrective action.

## What the May 2026 sweeps showed

Sweep 1 (initial UCB1-Tuned vs ROI, 5 runs each):
- ROI shows much wider variance than UCB on rss_reduction (sqlite: 28.5–53.5%
  for ROI vs 28.6–29.8% for UCB).
- ROI run 2 of sqlite hit 53.5% reduction (vs typical ~29%); UCB never
  reproduced.
- UCB had ~5% lower throughput median than ROI on rocksdb_quick.
- Diagnosis: UCB1-Tuned's variance term contracts the bound aggressively when
  early samples land in the low-reward fast-tier regime. The deep tier (rare
  big wins) gets pulled less than it deserves on heavy-tailed reward
  distributions.

Sweep 2 (ROI / UCB1T-4 / UCB1T-16 / UCBV-4 / UCBV-16, 5 runs each):

| Bench | Best deep-tier hit | Config that found it |
|---|---|---|
| sqlite_full | 296.4 MB min_rss / 38.4% reduction | UCBV-4 (1/5 runs) |
| rocksdb_quick | 61.8 MB min_rss (vs ~405 MB typical) | UCBV-16 (1/5 runs) |
| rocksdb_quick | 259.6 MB min_rss | UCB1T-16 (1/5 runs) |

ROI never found a deep-tier win in either bench across 5 runs. UCB variants
each found it once. Higher `min_pulls` and UCB-V's longer exploration tail
both demonstrably increase the *probability* of finding the deep-tier sweet
spot — but median performance is unchanged across configs because the win
remains rare (1 in 5).

Throughput cost is real but small: 2–7% below ROI median on rocksdb_quick;
within ±1% on sqlite_full.

## Why not Thompson sampling

The original thread proposed Thompson sampling per `(arena, size_class)`. We
went with UCB because:
1. UCB has no tuning parameter (Thompson needs a posterior family choice +
   prior).
2. Deterministic — easier to debug across runs without RNG state in
   BootstrapAlloc.
3. UCB1-Tuned's `O(K log n)` regret bound matches Thompson's empirically on
   stochastic bandits with bounded rewards; UCB-V is even better when reward
   variance is heterogeneous (which it is here — fast vs deep tier cost
   distributions are very different).

Thompson sampling remains a reasonable alternative if posteriors-as-Gaussians
gives meaningfully better small-sample behavior. Not implemented yet.

## Open: making the deep-tier win consistent

UCB variants find the deep-tier sweet spot 1/5 runs at best. ROI finds it 0/5.
Median performance is unchanged. The next steps to investigate:

### A. Per-arena warm-start posterior (`SMASH_UCB_WARMSTART=1`)

Seed an under-explored bucket's arm means from the arena aggregate so cold
buckets don't have to re-discover the deep tier from scratch. Concretely:
keep a per-(worker, arena) aggregate of `(pulls, mean, m2)`; on `recordReward`
update both bucket and arena; in `selectTierUCB`, blend the bucket's posterior
with the arena's via a weighted mean when the bucket is under-explored
(`bucket_pulls < min_pulls`).

Hypothesis: arena routing already produces structurally homogeneous pages,
so the arena aggregate is a meaningful prior for any `(arena, sc)` bucket
within it. Should reduce the variance of "first-K-pages" reward estimates.

### B. Forced periodic deep-tier sampling (`SMASH_UCB_FORCE_DEEP_EVERY=N`)

On very-cold pages, override the UCB decision every Nth time and force a
deep-tier pull. Costs CPU on rare pages but keeps the variance estimate of
the deep arm live so it doesn't get permanently abandoned.

Hypothesis: heavy-tailed rewards mean the deep tier's "true mean" is mostly
0 with rare big wins. UCB needs occasional re-sampling to avoid converging
on the fast tier just because it has more low-variance evidence.

## Implementation log

- 2026-05-14: UCB1-Tuned implemented behind `SMASH_UCB=1`. 14/14 ctests pass.
- 2026-05-14: First sweep — ROI vs UCB. Diagnosis as above.
- 2026-05-14: Added UCB-V variant + `SMASH_UCB_MIN_PULLS` knob.
- 2026-05-15: Sweep 2 across 5 configs. Median performance unchanged; rare
  deep-tier wins occur in UCB but not ROI. Heavy-tailed reward hypothesis
  confirmed.
- 2026-05-15: Implemented warm-start (`SMASH_UCB_WARMSTART=1`) and
  forced-deep sampling (`SMASH_UCB_FORCE_DEEP_EVERY=N`). Notes:
  - Warm-start uses parallel Welford combine (Chan/Golub/LeVeque) to merge
    the per-arena aggregate posterior with the per-bucket posterior. The
    arena prior is capped at `(min_pulls - bucket_pulls)` effective pulls
    so it fades out as the bucket accumulates evidence — preventing a
    strong arena prior from drowning a (legitimately) divergent bucket.
  - Force-deep cycles per-bucket (`s.selections++ % N == 0`) rather than
    globally, so the forced pulls are spread across buckets instead of
    creating a tick-aligned CPU spike. Still attributes reward to the
    forced arm so the bandit's variance estimate stays live.
  - Both updates flow through `worker.arena_arm[arena_id]` only when
    `SMASH_UCB_WARMSTART=1` is set; the per-bucket arm posterior is
    updated unconditionally under UCB. This means the arena aggregate is
    only kept fresh when warm-start is enabled — which is fine because
    nothing else reads it.
  - 14/14 ctests pass under all new env-var combinations.
- 2026-05-15: Sweep 3 results: ROI vs UCBV-16 vs UCBV-16+WS vs UCBV-16+FD8
  vs UCBV-16+WS+FD8, 5 runs each on sqlite_full + rocksdb_quick.

  **Sqlite_full (median rss_reduction_pct [min-max]):**
  | ROI | UCBV-16 | +WS | +FD8 | +WS+FD8 |
  |---|---|---|---|---|
  | 30.3 [28.7-31.4] | 30.1 [29.4-30.3] | 29.8 [29.0-30.4] | 29.7 [29.5-30.8] | 29.9 [**19.4**-30.6] |

  **Rocksdb_quick (median peak_rss_mb [min-max]):**
  | ROI | UCBV-16 | +WS | +FD8 | +WS+FD8 |
  |---|---|---|---|---|
  | 412.6 [399.1-448.2] | 410.3 [401.3-453.4] | 411.6 [396.1-471.4] | 424.6 [400.3-497.5] | 403.4 [**392.1**-421.8] |

  **Headline reads (sweep 3):**

  1. **No deep-tier jackpot in any config this time.** Sweep 2's UCBV-16
     hit 61.8 MB min_rss on rocksdb (1/5); sweep 3's UCBV-16 (same config!)
     never went below 401 MB. The "rare big win" is genuinely stochastic —
     not even a fixed config reproduces it across N=5.

  2. **WS+FD8 has the lowest median rocksdb peak (403.4) AND the tightest
     spread (392.1–421.8).** The combo seems to *compress more
     consistently*, even when it doesn't find the jackpot regime. WS alone
     and FD8 alone don't reproduce this — only the combination does. The
     hypothesis: WS gives FD8 a non-uniform prior that points toward a
     productive deep-tier region, and FD8 then keeps re-sampling it.

  3. **WS+FD8 has a serious sqlite outlier**: run 1 hit 19.4% reduction
     (vs ~30% typical) with cold_p99_us=16.75 (vs typical 1.5). Looks like
     the warm-start prior fed FD8 a bad deep-tier estimate that survived
     the cold-start period and dragged down compression for the whole run.
     The other 4 runs of WS+FD8 sqlite were normal (29.6–30.6%). So the
     combo can either help or hurt: it depends on whether the early arena
     prior happens to be informative or misleading. With N=5 we can't
     distinguish a real failure mode from a measurement artifact.

  4. **All UCB variants now within ±2% of ROI on both benches' medians.**
     The single-config variance reduction in sweep 3 (vs sweep 2) suggests
     measurement-noise is dominating signal at N=5 and at this workload
     scale. Sweep 2's "ROI never finds the jackpot" was likely a sample
     accident, not a structural difference.

  5. **Throughput**: WS+FD8 is on par with ROI on rocksdb (1001K vs 996K
     ops/s) — better than plain UCBV-16's 1021K is anomalous and probably
     also noise.

  **What this actually settles:**

  - The "ROI vs UCB" question is empirically a wash on these benches at
    N=5. To distinguish them we need either (a) much higher N, (b) workloads
    with more bucket heterogeneity (Redis/DuckDB TPC-H), or (c) longer
    cooling phases that let the bandit's posterior actually converge.
  - WS+FD8's sqlite outlier (19.4%) is a real concern — it means a poorly-
    seeded warm-start can get *stuck* on the wrong arm even with forced
    deep-tier sampling. The FD8 mechanism only forces deep, not fast, so
    if the bandit picks deep too often via a misleading WS prior, FD8
    can't correct it.
  - Recommended default if we ship UCB at all: keep `SMASH_UCB_VARIANT=2`
    + `SMASH_UCB_MIN_PULLS=16` (sweep 2 best-finder, sweep 3 confirms it
    doesn't regress). Don't ship WS or FD8 as defaults — they're
    well-motivated mechanisms that don't reliably help at this scale.

  **Open questions worth a longer experiment:**

  - Does WS+FD8 dominate at higher N (say 20 runs/config)? The single
    sqlite failure could be a rare event masking a real median improvement.
  - Does WS alone help when the workload has clearly distinct arenas (e.g.
    JSON-heavy + KV-heavy in one process)? The current benches don't
    really exercise arena differentiation.
  - Should FD8 force *both* arms periodically, not just deep, to avoid
    getting stuck on a misleading prior?

- 2026-05-15: Tried to extend sweep to Redis + memcached on this macOS box.
  Hit two infrastructure problems before getting any data:

  **Problem 1 — redis-server-libc + DYLD_INSERT_LIBRARIES crashes on
  startup.** Server prints `=== REDIS BUG REPORT START ===` then exits
  before flushing the trace, even with stderr redirected. Reproducible
  with stock flags. The patched `redis-server-smash` (libsmash linked
  in, not interposed) starts fine.

  **Problem 2 — RSS does not drop on macOS during cooling, on either
  redis-server-smash or DYLD-interposed memcached, even with 60+s cool
  windows on 134-403 MB fill sizes.** Tried `redis-server-smash` with
  `--idle-mode yes` and all the bg-tasks-disabled flags from CLAUDE.md;
  tried `SMASH_VERY_COLD_TICKS=5` to accelerate cold detection; tried
  workloads up to 300K SETs / 2KB values. RSS sits exactly at fill RSS
  through 135 seconds of cooling. Memcached showed a hint of compression
  in one run (160 MB → 46 MB at t=5s, partial drop early then stuck).

  **Likely cause.** The benchmarks in CLAUDE.md cite EC2 (Linux) Redis
  results showing 47% reduction. macOS uses `MADV_FREE_REUSABLE` which
  requires PROT_READ on pages before the protection flip; the scan/
  compress phases on this box may not be tripping that path. Or the
  bg-disabled flags don't fully quiet redis on macOS — the event loop
  + IPC channels may keep enough pages warm that the compressor sees
  nothing eligible.

  **What this means for the bandit sweep.** Without a workload that
  actually exercises the compression path, comparing UCB variants to ROI
  measures only noise. I'm not going to claim sweep results from this
  bench setup. The right next step is to run the existing sweep on Linux
  (paper benchmarks were measured on Linux EC2), or find a macOS-friendly
  workload that reliably triggers cooling — but that's a separate
  problem from the bandit comparison.

  **Time spent on this:** ~30 minutes building the driver, ~30 minutes
  diagnosing why it doesn't trigger compression. Calling it before
  burning the rest of the day on infrastructure debugging that doesn't
  inform the bandit question.

  **State of the bandit work:** code is implemented and correct (14/14
  ctests pass), the in-process sweep showed median performance is a
  wash, and we now have evidence that distinguishing the variants needs
  either much higher N on the existing benches or a workload regime
  not currently reproducible on this macOS box.
