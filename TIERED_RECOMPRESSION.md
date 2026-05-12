# Tiered Recompression

Branch: `tiered-recompression` (off master, post-PR-#15)

## Motivation

Smash currently picks one compression algorithm per page at *initial* compress time — never re-compresses. The choice is driven by the ROI model: zstd-1 (fast tier) for cold_count ≥ floor (≈ 10 ticks), zstd-9 (deep tier) for cold_count ≥ kVeryColdTicks (60). Once a blob is stored, it's frozen until the page faults back to ACTIVE.

This is wrong for burst workloads. On the aarch64 VM, measured throughputs (`bench_algo_compare`, json data, 4 KB pages):

| Algo | Comp MB/s | Decomp MB/s | Ratio |
|---|---|---|---|
| LZ4 | 1237 | 5117 | 32.8% |
| zstd-1 | 349 | 667 | 16.7% |
| zstd-9 | 130 | 1035 | 16.7% |

The compressor can't keep up with cache-server fills (~50 MB/s) using zstd-1 as the *initial* tier — that's the root of the Q2 peak-RSS problem. The infrastructure for tier upgrade has been there since the recompress-thrash work: `cold_count_` keeps incrementing for COMPRESSED pages (compressor_thread.h:523 says "so zstd upgrade can trigger"), but no code acts on it.

## Goal

Use LZ4 to catch the burst (high throughput), then progressively re-compress with zstd-1 and finally zstd-9 as pages prove they stay cold. Each page transitions at most twice over its lifetime.

| Tier | Algo + level | Picked when | Approx ratio | Approx comp MB/s |
|---|---|---|---|---|
| **T1 — initial** | LZ4 | first compression (cold_count ≥ floor) | 33% | 1200 |
| **T2 — steady** | zstd-1 | T1 page that stayed cold N₁ ticks | 17% | 350 |
| **T3 — deep** | zstd-9 | T2 (or T1) page that stayed cold ≥ kVeryColdTicks | 16% | 130 |

Transitions T1 → T2 → T3 only. No downgrades (pages that get touched are decompressed by the fault handler; on re-cooling they restart at T1).

## Current architecture (relevant pieces)

- `CompressAlgo` enum: `NONE / LZ4 / ZSTD / ZSTD_DICT`. Packed in top 2 bits of `CompressedPageInfo::comp_size` (30 bits left for size).
- `CompressedPageInfo`: `{ void* data, uint32_t comp_size+algo, uint32_t alloc_size }` per page. Indexed by VmRegion page index.
- `CompressEngine::compress(src, dst, src_sz, dst_cap, algo, sc)` — public API, but uses fixed `kZstdNormalLevel`. Per-worker `CompressWorker::compress` already takes `zstd_level` parameter ✓.
- `CompressionROI::selectProfile(cold_count, stats_count, stats_sum, observed_costs)` returns `AlgoProfile* {algo, zstd_level, ...}`.
- Per-bucket `SizeClassStats` already tracks per-tier (`kTiers=2`, fast/deep) ratios and EMA-smoothed compression times.
- `CompressStore::store(data, size, &alloc_size, page_idx)` / `release(ptr, alloc_size, page_idx)` — sharded bump allocator + per-region live_bytes refcount. Decommits when a region drains to 0.
- Per-page lock (`PageLockTable::lock(page_idx)`) — already used by `compressPage` and the fault handler.

## Design

### State machine

Unchanged. Pages still transit ACTIVE → ACTIVE_MONITORING → COMPRESSING → COMPRESSED → (faulted) → ACTIVE. The new feature: a COMPRESSED page can have its blob *upgraded in place* (no state transition).

### Re-tier flow

Executes on a compressor worker, similar shape to existing `compressPage`:

1. Phase 2 enumerates live pages. For each in `COMPRESSED` state:
   - Read existing `CompressedPageInfo` (current algo, comp_size, ptr, alloc_size)
   - ROI: would upgrade to (zstd-1 or zstd-9) win on cost/benefit given current cold_count?
   - If no: continue.
2. Acquire per-page lock.
3. Re-check state is still COMPRESSED (else released by fault handler — abort).
4. Set state → COMPRESSING (we own the blob now).
5. Decompress existing blob into worker's scratch buffer (`page_buf`).
6. Compress scratch buffer with new algo+level into worker's output buffer.
7. If new compressed size < current AND new ratio passes `kMinCompressRatio` — proceed; else abort & restore COMPRESSED.
8. `CompressStore::store(new_blob, new_size, &new_alloc_size, page_idx)` — get new ptr.
9. **Publish**: write new `CompressedPageInfo` (data, comp_size+algo, alloc_size) atomically (need 16-byte atomic store or store under lock).
10. State → COMPRESSED.
11. Release lock.
12. `CompressStore::release(old_ptr, old_alloc_size, page_idx)` — drop refcount on old blob.

### Concurrency contract with fault handler

The fault handler (compressor_thread.h:1730+) for COMPRESSED state:
- Acquires page lock
- Reads CompressedPageInfo, decompresses, transitions to ACTIVE
- Releases lock

Because re-tier holds the lock from step 2 through 11, the fault handler either:
- (a) acquires lock first → reads old blob → decompresses → transitions to ACTIVE → re-tier sees state != COMPRESSED in step 3 and aborts, OR
- (b) waits for re-tier to finish → reads NEW blob (published in step 9)

Either case is correct.

### Why pre-store-then-release order matters

If we released the old blob before storing the new one, a concurrent (well, lock-blocked but soon-to-run) fault handler that picked up the lock between operations would see invalid memory. Storing first means the page always has *a* valid blob.

### CompressedPageInfo atomic update

The struct is `{ void*, uint32_t, uint32_t }` = 16 bytes on aarch64. Updates happen under the page lock, and the fault handler reads under the same lock. So plain assignment is fine — the lock provides the ordering.

(If we wanted to make the fault path lock-free, we'd need a 16-byte CAS or a separate atomic discriminator. Not needed here.)

### Initial tier: LZ4

`selectProfile` currently returns one of:
- `{LZ4, level=0}` when `kUseLz4=true`
- `{ZSTD, kZstdFastLevel=1}` otherwise (the default since the LZ4 build was demoted)
- `{ZSTD, kZstdDeepLevel=9}` when cold_count ≥ kVeryColdTicks AND ROI permits

Phase A flips the default fast tier to LZ4 (just like the old `SMASH_USE_LZ4` path) — but only the *initial* compression. Re-tier scans then promote to zstd-1.

### ROI thresholds

Need to add to ROIConfig:
- `tier_upgrade_lz4_to_zstd1_ticks` — default ~kColdTicks * 3 (30 ticks ≈ 30 s)
- `tier_upgrade_zstd1_to_zstd9_ticks` — kVeryColdTicks (60)

A page picked at T1 (LZ4) with cold_count = 30+ becomes eligible for T2 upgrade. A page at T2 (zstd-1) with cold_count = 60+ becomes eligible for T3 upgrade.

The size-win gate: new blob must be ≥ 20% smaller AND the upgrade cost (decompress + recompress μs) must amortize against expected remaining cold lifetime. ROI math is parallel to the existing `selectProfile` ROI; we just feed it current vs target.

### Worker-tier ordering on phase2

Currently phase2 only processes ACTIVE / ACTIVE_MONITORING pages. We extend the loop to ALSO process COMPRESSED pages, with a separate counter so we can tune throughput allocation.

## Implementation phases

- [x] **Phase 0**: Plan doc, branch, baseline numbers. (this doc)
- [x] **Phase A**: `selectProfile` picks LZ4 as the initial fast tier. Added `kTieredRecompression` master flag (default on, disable via `-DSMASH_NO_TIERED_RECOMPRESSION`) which implies `kUseLz4FastTier`. 15/15 tests pass; bench_rss 44 % (= master); bench_sqlite cooling 52.0 % (vs master 63.5 % — expected ratio regression since LZ4 ≠ zstd-1).
- [ ] **Phase B**: `recompressPage(page_idx, worker, target_algo, target_level)` helper. Locks, decompress, recompress, swap, unlock. Unit-test in isolation.
- [ ] **Phase C**: phase2 scans COMPRESSED pages and decides on upgrade via ROI. Per-page cold_count threshold + per-bucket SizeClassStats.
- [ ] **Phase D**: Workload benchmarks. memcached + Redis + bench_rss + bench_sqlite. Compare:
    - fill_rss (should be ≈ master since LZ4 catches the burst at last-tick instead of not at all)
    - cool_rss (should converge to ≈ master after upgrades — ratio recovers)
    - serve_rss (similar)
    - throughput (fill, GET) — LZ4 keeps app threads less blocked on faults
- [ ] **Phase E**: Tune thresholds; add stats line fields (`tiered_t1`, `tiered_t2`, `tiered_t3` counts).

## Open questions / risks

- **LZ4 incompressible-data fallback**: random data compresses to 100.4% with LZ4. The existing `kMinCompressRatio = 0.75` gate already rejects such pages — they stay ACTIVE. No new handling needed.
- **Re-tier during fill burst**: if we're still in a fill, re-tier wastes CPU. Gate: only re-tier when alloc_rate is low (idle). Could reuse the alloc_rate_ema from the Q2 branch if we want this — for now, simpler to scan unconditionally and let ROI do the work.
- **ZSTD_DICT integration**: deep-tier currently can use a trained dict. Keep that behavior — T3 upgrade still uses ZSTD_DICT if available, plain ZSTD-9 otherwise.
- **Worker tick budget**: re-tiering competes with first-time compression. Need to avoid starving first-time work during sustained allocation. Initial heuristic: process re-tier candidates *after* all first-time eligible pages in the tick.
- **Per-bucket stats — which tier's stats apply?**: current SizeClassStats tracks 2 tiers (fast=0, deep=1). With 3 tiers we either bump `kTiers` to 3 or treat T1+T2 as the "fast" bucket (loses T1 vs T2 telemetry). Plan: bump kTiers to 3, expand sc_stats arrays.

## Progress log

### 2026-05-12 — Phase 0

- Branch created off master post-PR-#15.
- This plan doc written.
- Baseline numbers (master HEAD = 1cb9bd79):
  - bench_rss: 44%, bench_sqlite: 63.5%, ctest 15/15
  - memcached (1M keys, 30s cool, 20s serve): fill 279.2, cool 99.2, serve 279.2
  - Redis: fill 182.4, cool 75.4, serve 154.2

### 2026-05-12 — Phases A–D

Phase A: switched initial fast tier to LZ4 via `kTieredRecompression` ⇒ `kUseLz4FastTier=true`. 15/15 tests; bench_sqlite cooling drops 63.5 % → 52.0 % (expected — LZ4 ratio worse than zstd-1).

Phase B: `recompressPage(page_idx, worker, target_algo, target_level)` helper. Same lock contract as `compressPage`; acquires fault slot for DCtx during decompress; new blob stored before old blob released so the fault handler sees a valid blob through the transition.

Phase C: `phase2` scans COMPRESSED pages with `cold_count >= very_cold_ticks`; upgrades fast-tier blobs to zstd-9 (or ZSTD_DICT if a dict is trained). Added per-page `page_tier_` byte (1 MB / 1M pages) so we can identify fast-tier blobs unambiguously after the upgrade has changed the algo enum. Added `tier_upgrade_attempts_` / `tier_upgrade_success_` counters and surfaced them in SIGUSR2 stats.

Phase D: workload benchmarks.

**Memcached (1M keys, 90 s cool, 20 s serve)**:

| Config | fill_rss | cool_rss | serve_rss |
|---|---|---|---|
| libc | 235.8 | 241.8 | 241.8 |
| jemalloc | 240.3 | 252.4 | 252.4 |
| master smash | 279.2 | **99.2** | 279.2 |
| smash + LZ4 initial + zstd-9 upgrade | 279.6 | **181.5** | 370.7 |
| smash + zstd-1 initial + zstd-9 upgrade | 282.8 | **100.7** | 289.2 |

Trace (SIGUSR2 stats during 80s cool) confirms upgrades fire: `tier_up=39953/40082` for the LZ4 variant. So the mechanism works; the issue is RSS doesn't drop after upgrades.

**Diagnosis — CompressStore bump-allocator fragmentation.** `CompressStore` allocates blobs via per-shard bump pointer; a region is decommitted only when ALL its blobs are released (`live_bytes` hits 0). During a re-tier wave:
1. Page X's old blob (LZ4, ~1.3 KB) is in region R. R has many other live blobs.
2. recompressPage allocates the new blob (zstd-9, ~0.7 KB). `bumpAlloc` on the shard's current region. R's offset is already near `kRegionSize`, so the new blob lands in a NEW region R'.
3. Old blob released → R.live_bytes drops by 1.3 KB. R still has many other live blobs.
4. As more pages upgrade, R drains slowly. But a handful of pages won't upgrade (faulted by app between phase1 and phase2), leaving a few stranded LZ4 blobs in R. R can never fully drain, so its 16 MB stays committed.
5. With LZ4 (1.3 KB) blobs vs zstd-9 (0.7 KB) blobs the size mismatch is 2× — accumulates fast. With zstd-1 → zstd-9, the size mismatch is ~5 % — fragmentation is negligible.

This is why **the LZ4 variant adds 82 MB of bloat** (stranded LZ4 blobs in regions that can't drain), while **the zstd-1 variant matches master** (sizes are similar enough that fragmentation is invisible).

**Redis (200K keys, 30 s cool, 20 s serve)** — zstd-1 + upgrade variant:

| Config | fill_rss | cool_rss | serve_rss |
|---|---|---|---|
| libc | 142.3 | 142.3 | 142.4 |
| jemalloc | 144.7 | 144.7 | 144.9 |
| master smash | 182.4 | 75.4 | 154.2 |
| smash + zstd-1 initial + upgrade | 183.6 | 76.9 | 156.1 |

Same conclusion: re-tier runs but provides no measurable benefit because zstd-1 → zstd-9 ratio gap is only 5–7 pp.

### Conclusion

The tiered-recompression mechanism is structurally sound (15/15 tests, debug confirms upgrades fire) but **provides no measurable benefit on tested workloads with the current `CompressStore` design**:
- LZ4 → zstd-9: blob-size disparity causes region fragmentation, net RSS regresses by 80 % on memcached.
- zstd-1 → zstd-9: ratio improvement is ~5 % (17 % → 16 %), lost in noise.

**To unlock LZ4-as-initial** (the original motivation — catching fill bursts with 4× faster compression), `CompressStore` would need a freelist/slab allocator (~200-400 lines) so released slots can be reclaimed without waiting for whole-region drain.

### Phase E — `CompressStore` per-region freelist with split (added)

Added in-band freelist to each region: when `release()` is called and live_bytes > 0, the slot is overwritten with a 16-byte `FreeNode` header (`size`, `next_offset`) and pushed onto the region's free list. `bumpAlloc()` checks the current region's free list (and on overflow, older regions' free lists) before bumping the offset. When a freelist slot is larger than requested, it splits — the remainder is returned to the free list as a smaller node. With both pieces:

| Workload | master | LZ4 no freelist | LZ4 + freelist | LZ4 + freelist + split | zstd-1 + tier (no LZ4) |
|---|---|---|---|---|---|
| memcached cool | 99 | 181 (+82) | 132 (+33) | 132 (+33) | 100 (~master) |
| Redis cool | 75 | — | 84 (+9) | 84 (+9) | 77 (~master) |

Freelist alone closed ~60 % of the memcached bloat gap (49 MB recovered). Split didn't measurably improve on freelist-only, because the remaining 33 MB delta isn't within-region fragmentation — it's **whole-region waste**: a handful of "stranger" blobs (pages faulted by the app between phase1 and phase2) keep ~4 LZ4-sized regions from fully draining, even after every other blob in them has been freed and reused.

### Phase E findings: LZ4 initial provides no measurable workload benefit

Comparing LZ4-initial vs zstd-1-initial (both with freelist + upgrade enabled):

| Metric | LZ4 initial | zstd-1 initial |
|---|---|---|
| memcached fill_rss | 278.7 | 282.8 |
| memcached cool_rss | 132.1 | 100.7 |
| memcached fill_ops/s | 4750 | 4750 |
| Redis fill_rss | 183.6 | 183.6 |
| Redis cool_rss | 84.0 | 76.9 |
| Redis fill_ops/s | 230946 | 207468 |

- Fill RSS: identical — the burst isn't caught any earlier by switching algorithms.
- Fill throughput: identical (memcached) or marginally better with LZ4 (Redis: +11 %).
- Cool RSS: LZ4 costs 32 MB on memcached, 9 MB on Redis vs zstd-1.

The original Q2 hypothesis ("compressor throughput is the bottleneck") doesn't hold for these workloads — switching to LZ4 doesn't reduce the peak. The bottleneck is somewhere else (compressor bookkeeping, fault overhead, or simply that 1-second tick granularity is too coarse).

### Conclusion — recommended PR

**Keep**:
- `CompressStore` freelist + split. Genuine allocator improvement; closes the LZ4 bloat path; adds zero overhead on workloads that don't release-then-re-store.
- `recompressPage` helper + per-page `page_tier_` byte + phase2 upgrade scan. Infrastructure for future tier-upgrade work; behind `kTieredRecompression` flag (default on).
- Tier-upgrade telemetry in SIGUSR2 stats (`tier_up=success/attempts`).

**Revert**:
- `kUseLz4FastTier` default flip. Keep `SMASH_USE_LZ4` as opt-in for users who want to experiment. With the freelist now in place, opting in costs ~33 MB cool_rss on memcached-class workloads instead of the previous 82 MB.

This leaves the codebase with a real allocator improvement, a tier-upgrade mechanism that's a no-op at zstd-1→zstd-9 (5 % ratio gap, in noise), and the LZ4 option available behind a flag.

### Side experiment — re-enabled dictionary training (kDictTrainSamples=16)

Question asked: would per-size-class trained dictionaries pull cool_rss below the current ~17 % zstd-1 ratio?

| Workload | no-dict baseline cool | with dict cool | delta |
|---|---|---|---|
| memcached (1M keys json) | 100.7 MB | 102.6 MB | +1.9 MB |
| Redis (200K keys json) | 76.9 MB | 79.3 MB | +2.4 MB |

Dict overhead (1.2 MB steady-state predicted; ~2-3 MB observed) costs more than the ratio improvement returns. **Not material on these workloads.** Reverted `kDictTrainSamples = 0`.

Hypothesis: the JSON records used in these benches have shared field names but varied content; the dict captures the schema (~few hundred bytes of header structure) but most of each blob is the variable payload that doesn't dedup. For workloads with *highly* structured shared content (e.g., protobufs with fixed fields, log messages with shared templates), dicts could still win.

### Side experiment — zstd-3 as fast tier

Bench_algo_compare on a single run showed zstd-3 with notably faster decomp (~50 %) than zstd-1, suggesting it could improve fault latency. Re-measured on a clean run:

| Algo | json comp | json decomp | json ratio | sqlite comp | sqlite decomp |
|---|---|---|---|---|---|
| zstd-1 | 340 | 716 | 16.7% | 438 | 901 |
| zstd-3 | 315 | 728 | 17.2% | 374 | 854 |

The earlier "fast decomp" advantage was an outlier (warmup/caching artifact). On clean runs zstd-3 is 1-15 % faster decomp at the cost of slower compress and slightly worse ratio. **Not a win — keep zstd-1.**
