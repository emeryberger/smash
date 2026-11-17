# Peak RSS: eager fill compression and the peak guard

Capacity is provisioned for a process's peak resident set, so smash's
steady-state savings only count if the peak does not grow. This note records
where smash's peak comes from on current master and two opt-in mechanisms that
lower it. Both are **off by default**; see "Defaults" at the end for why.

## Where the peak comes from

`bench/run_peak_compare.py` samples RSS every 20 ms until the bench prints its
`workload_done` marker and takes the max of those samples and the bench's own
`VmHWM`. On master (`358f985` plus bench instrumentation only), two sources
put smash's peak above glibc's:

1. **The fill itself.** Every page of a bulk load is written during the load,
   so none is cold until the cold timeout (10 s by default) after its last
   write. The process peaks at the full uncompressed fill plus smash's
   allocator overhead (SQLite 1M rows: 764 MB at the end of the fill vs
   705 MB for glibc).
2. **The cold wave.** When the whole fill goes cold at once, Phase 2 shadows
   up to 1/8 of the heap in blob bytes (the #104 cap) before Phase B reclaims
   the frames. If the wave arrives while the application is still growing
   (SQLite's serve phase inserts rows), RSS rises above the end-of-fill level:
   SQLite 1M 764 → 825 MB, SQLite 250K 254 → 270 MB.

The 2026-06 finding ("peak inflation", smash's peak at fill end because nothing
compresses until the cold window elapses) still holds; #104 bounded but did
not remove the cold-wave overshoot; and at 3M rows smash's peak is still 10%
above glibc's (2453 vs 2222 MB).

## Mechanisms

**`SMASH_EAGER_FILL=1`.** While committed pages grow by at least
max(4 MiB, 1/64 of the heap) per tick, pages first committed during the growth
are marked as fill pages. A fill page that then stays unwritten for one whole
tick is compressed at floor 1 instead of the cold floor (the ROI gate treats
it as if it had reached the floor; the ratio gate, rc backoff, shadow cap and
VMA budget all still apply). Each page is eligible once: if it refaults or is
written while in shadow it returns to the normal policy, and that refault does
not feed the per-bucket rc EMA (an earlier version let eager refaults of
SQLite's hot rows lengthen the cold wait of the whole bucket, delaying the
normal cold wave by ~13 s). If more than 1/4 of eager compressions come back
(after 256) the mechanism switches itself off. `SMASH_EAGER_FILL_TAIL=N`
(default 1) keeps it on N ticks after growth stops.

**`SMASH_PEAK_GUARD=1`.** Every compression into shadow also draws on a
per-tick budget of `VmHWM − RSS` blob bytes (never below 4 MiB). A cold wave
therefore cannot lift RSS above the process's own high-water mark; each
tick's reclaim widens the headroom, so the wave ramps up geometrically
instead of all at once. Eager pages are guarded too: two earlier versions
exempted them (always, then only while the heap grew), and with either the
combined config ended SQLite's 1M-row fill above the guard-only peak
(807 vs 762 MB at a 1 s timeout), because shadow blobs made during a fill
are often discarded.

`tick` lines of `SMASH_PRESSURE_LOG` carry cumulative `eager=` (eager
compressions) and `eager_back=` (eager pages that came back).

## Measurements

cloudnew (AMD EPYC 9R14, Amazon Linux 2023, kernel 6.12, x86-64), every
process pinned to a 24-core `taskset` slice shared with nothing else of ours;
other workloads ran on other slices of the host. Median of 5 randomized,
interleaved runs per config. Default cold timeout (10 s) unless marked
"1 s". Full mode unless marked. MB = MiB.

Commands (from `bench/`'s parent, build configured with
`-DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=OFF -DSMASH_BUILD_BENCH_ALLOCATORS=OFF`):

```
H="python3 bench/run_peak_compare.py --build-dir build-linux --runs 5 \
   --configs glibc,smash,smash_eager,smash_guard,smash_both"
$H --modes full,large --bench sqlite                                   # SQLite 250K (--quick)
$H --modes full,large --bench rss                                      # 256 MiB paced over 8 s
$H --modes full --bench sqlite --sqlite-rows 1000000                   # SQLite 1M, one transaction
$H --modes full --bench sqlite --sqlite-rows 1000000 --sqlite-batch 10000
$H --modes full --bench sqlite --sqlite-rows 3000000
$H --modes full --bench sqlite --sqlite-rows 3000000 --sqlite-batch 10000
$H --modes full --bench sqlite --sqlite-rows 1000000 --cold-timeout 1
$H --modes full --bench sqlite --sqlite-rows 1000000 --sqlite-batch 10000 --cold-timeout 1
```

Peak / avgRSS (AUC ÷ duration) / min RSS during serve, in MB; serve ops/s.
glibc, smash, eager and guard columns: first matrix (all five configs
interleaved). "both" column: a second matrix of `--configs smash,smash_both`
run after the final change (eager pages guarded too); its interleaved smash
anchor matched the first matrix within 1% on every row (e.g. 830 vs 830 MB,
274 vs 274 MB, 2453 vs 2453 MB peaks), and its "vs smash" deltas below are
against that anchor.

| Workload | glibc | smash | smash + eager | smash + guard | smash + both |
|---|---|---|---|---|---|
| SQLite 250K | 250 / 196 / 180, 98.9K | 274 / 206 / 162, 92.1K | **208 / 147 / 106**, 91.8K | 259 / 209 / 139, 91.4K | 210 / 167 / 110, 91.6K |
| paced fill 256 MiB / 8 s (both modes) | 261 / 216 / 261 | 266 / 154 / 14 | **113 / 42 / 14** | 266 / 154 / 14 | **113 / 42 / 14** |
| SQLite 1M, 1 txn | 833 / 699 / 706, 90.3K | 830 / 525 / 284, 80.2K | 834 / 539 / 305, 81.1K | 777 / 538 / 336, 80.0K | **760 / 460** / 281, 79.8K |
| SQLite 1M, commit / 10K rows | 835 / 699 / 706, 91.4K | 835 / 512 / 278, 79.5K | 819 / 487 / 353, 83.2K | 776 / 512 / 285, 80.0K | **760 / 455** / 275, 80.9K |
| SQLite 3M, 1 txn | 2222 / 1814 / 2106, 77.9K | 2453 / 1851 / 1789, 52.9K | 2450 / 1816 / 1596, 52.6K | 2261 / 1793 / 1548, 51.1K | 2262 / **1740 / 1067**, **48.1K** |
| SQLite 3M, commit / 10K rows | 2220 / 1813 / 2106, 76.4K | 2457 / 1438 / 777, 50.0K | 2376 / 1385 / 776, 51.6K | 2253 / 1383 / 959, 50.2K | **2250 / 1300** / 759, 50.7K |
| SQLite 1M, 1 txn, 1 s | 834 / 699 / 706, 91.3K | 806 / 379 / 218, 81.0K | 807 / 387 / 218, 82.2K | 762 / 376 / 218, 81.9K | **760 / 353** / 218, 83.0K |
| SQLite 1M, commit / 10K, 1 s | 835 / 699 / 706, 91.9K | 819 / 363 / 217, 82.3K | 819 / 371 / 218, 82.6K | 762 / 364 / 218, 82.0K | **760 / 354** / 218, 82.7K |

Second-matrix commands: the same lines with
`--configs smash,smash_both`.

Large-only mode: SQLite's ~4.4 KB pages pass through to the system allocator,
so all smash configs match glibc there (250K: peak 254, avg 200 MB in every
config). The paced fill uses 1 MiB chunks and behaves identically in both
modes (rows above).

Two earlier versions of "both" exempted eager pages from the guard (always,
then only while the heap grew); both peaked at 807–819 MB on the 1 s rows,
no better than eager alone, which is why the final version guards them.

### What the numbers say

- **Write-once fills: eager wins outright.** The paced fill's peak falls from
  266 to 113 MB (−57%, and below glibc's 261) and avgRSS from 154 to 42 MB,
  with 29 refaults out of 65.6K compressions, in both modes. `test_peak_fill`
  pins this (growth ÷ bytes written 0.41 with eager, 1.00 without).
- **SQLite does not fill write-once.** With eager alone, about 59K of the
  ~243K eager compressions on 1M rows are shadow-discarded (written again
  after a quiet tick), with or without batched commits, mostly in one tick
  near the end of the fill (`sd=` in `SMASH_PRESSURE_LOG`). Hypothesis, not
  verified: every page carries its pcache header (`PgHdr1`) in the same
  allocation and SQLite rewrites those headers in bulk (dirty-flag clearing
  at COMMIT, the pcache hash table rehashing when it doubles). Eager alone
  still wins on a short fill (250K rows, 1.2 s: peak −24%, avgRSS −29%) but
  at 1M rows it is neutral on the peak and moves avgRSS −5% to +3%.
- **The guard alone** lowers the peak 6–8% wherever the cold wave overlaps
  growth (SQLite 250K/1M/3M) for ≤ 2.5% avgRSS and ≤ 3.4% throughput; it does
  nothing for the paced fill, whose peak is the fill itself.
- **Both together is the best peak config on every workload measured**: vs
  plain smash, peak −6% (1M, 1 s) to −23% (250K) and −57% (paced fill),
  avgRSS −3% to −19% (−72% paced fill), serve-phase minimum lower or within
  0.5%.
  The guard throttles eager compression during the fill, so shadow discards
  drop from ~59K to ~6K. Peak vs glibc: −16% (250K), −9% (1M), +1.4..+1.8%
  (3M, was +10%).
- **Throughput** vs plain smash: both −0.2% to +1.1%, except SQLite 3M in one
  transaction: −8.3% (48.1K vs 52.4K ops/s; the five runs do not overlap:
  47.2–49.7K vs 51.5–53.6K), with refaults up from 138K to 192K. The 3M-row
  gap to glibc (−32% serve ops/s) is pre-existing.

`bench/run_quick_ci.py` passes in both modes with the knobs off, with
`SMASH_EAGER_FILL=1`, and with both (cloudnew; bench_rss best reduction 87%
in every case, bench_sqlite 58.6–62.2% full / 27.4–27.7% large-only).

## Defaults

Neither knob is on by default. The bar for default-on was a clear win with no
regressions; the combination comes close (lower peak and avgRSS everywhere
measured) but costs 8% serve throughput on the 3M-row one-transaction SQLite
load, and eager alone is neutral-to-negative on large SQLite loads.
Recommended for capacity-bound deployments: `SMASH_EAGER_FILL=1
SMASH_PEAK_GUARD=1` together.

## Not measured here

- RocksDB (`bench_rocksdb` needs `-DSMASH_BUILD_BENCH_DEPS=ON`, which builds
  RocksDB 11.1.2 from source; the host has 9.8.4). The 2026-06 note found its
  memtables are rewritten until Flush(), which is the pattern where eager
  compression thrashes; the self-check bounds that but it is unmeasured.
- aarch64: only development runs on a Linux VM, of earlier versions of the
  code; not reported.
