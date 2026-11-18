# smash in memory-capped containers (cgroup v2)

Containers (Docker, Kubernetes, systemd units) bill memory through the cgroup
v2 memory controller: `memory.current` counts anonymous memory, page cache,
kernel memory and the zswap pool, and `memory.high` / `memory.max` cap it.
This document covers what goes wrong for smash under such a cap, what
`SMASH_CGROUP` does about it, and what was measured.

Code: `src/util/cgroup_monitor.h` (detection, parsing, the pure `decide()`
policy), `CompressorThread::cgroupInit/cgroupTick/pageResident/roiColdCount`
and `CompressStore::keepWarm` (`src/compress/`). Tests: `test_cgroup_monitor`,
`test_cgroup_policy_{full,large_only}`. Harness: `bench/run_cgroup_compare.py`.
Kubernetes example: `examples/k8s/smash-preload.yaml`.

## The problem, measured

All numbers in this section: cloudnew (AMD EPYC 9R14, kernel
6.12.115 Amazon Linux 2023, x86-64), workloads pinned to a 24-core taskset
slice on a shared host, zswap enabled with `compressor=zstd zpool=zsmalloc
max_pool_percent=20 shrinker_enabled=N`, one 16 GiB swapfile on NVMe. Caps:
sqlite `memory.high=250M memory.max=400M`, redis `160M/240M`.

1. **smash's blob store is swapped to disk, glibc's data is not.**
   `CompressStore` pages hold zstd output. zswap cannot compress them again
   and rejects them (`/sys/kernel/debug/zswap/reject_compress_fail`), so they
   go to the disk swap device. Per run, the host's `pswpin` delta was 0 for
   glibc on every run and 27K–32K pages for smash on sqlite (master and the
   new build with the policy off, 5 runs each). A pagemap probe
   (`/proc/PID/pagemap` + `/proc/PID/mem`, zlib-1 on each swapped page) at
   t=60 s of a capped sqlite run found incompressible swapped pages only in
   CompressStore regions (638 pages in one probe run, about 5K in a second
   run with `SMASH_DEBUG=1`); in the first run the 518 MiB heap VMA had 91K
   swapped pages, none incompressible. So a cold access to smash-compressed data pays
   a disk read plus a decompress, while the same access under glibc is an
   in-RAM zswap decompress. This is the "double fault" the README describes.
2. **The compressor swaps pages back in to compress them.** If the kernel has
   already swapped a page out, compressPage's snapshot faults it back in.
   bench_rss (256 MiB, cap 160M): 93.2K zswpin per run under smash vs 159
   under glibc; bench_rss re-touches only 10% of its pages, so nearly all of
   the difference is the compressor.
3. **memory.current sits at memory.high** for every allocator once the
   working set reaches the cap ("steady" column below); savings show up as
   lower mean memory.current, less swap and fewer stalls, not as a lower
   plateau.

## What SMASH_CGROUP does

At compressor start (`cgroupInit`), read `/proc/self/cgroup` (the `0::` line)
and walk from the process's cgroup under `/sys/fs/cgroup` up through every
ancestor that has a memory controller, keeping the ones with a limit (a
Kubernetes container has limits on both its own and the pod cgroup). If none
has a limit, the policy is off and costs nothing. Otherwise, once per tick
(1 s), `Monitor::sample()` reads memory.high/max/current per limited level
(tightest headroom wins), memory.events and the leaf's memory.pressure. Raw
`openat/read/close` syscalls into stack buffers; no malloc, no stdio.

`decide()` (pure, unit-tested):

| condition | level | actions |
|---|---|---|
| headroom ≥ 25% of limit | none | nothing |
| headroom < 25% | none | keep store warm; read back swapped store pages within budget |
| headroom < 10%, or PSI memory some avg10 ≥ 10% | elevated | keep warm; skip swapped-out pages |
| headroom < 3% (or above memory.high) | critical | same |

Levels are sticky for 10 samples after the last trigger.

- **Keep warm** (`CompressStore::keepWarm`): read one byte of each resident,
  live store page so its accessed bit is set and reclaim takes other pages
  (which are compressible and go to zswap). Non-resident pages are never
  touched by this read. It is a hint; the kernel can still evict.
- **Read back** (same function, budgeted): when not under pressure, issue
  `MADV_WILLNEED` (asynchronous swap-in readahead) on runs of live store
  pages that were swapped out, up to half the headroom above half the warm
  threshold, so the next cold access hits RAM.
- **Skip swapped pages**: phase 2 checks `mincore()` (one call per 64-page
  window per worker per tick) and leaves non-resident pages to the kernel.

Env: `SMASH_CGROUP=0|1` (unset = auto), `SMASH_CGROUP_ROOT` (tests),
`SMASH_CGROUP_HEADROOM_PCT`, `SMASH_CGROUP_PSI_PCT`, ablations
`SMASH_CGROUP_WARM=0`, `SMASH_CGROUP_SWAPIN=0`, `SMASH_CGROUP_SKIP_SWAPPED=0`;
opt-ins `SMASH_CGROUP_EARLY=1`, `SMASH_CGROUP_SPILL=1` (both measured worse,
below), and two opt-ins from round 2 that measured better (see "Round 2"):

- `SMASH_CGROUP_SKIP_SWAPPED=2`: skip non-resident pages whenever the cgroup
  is capped, not only at a pressure level (`1`, the default).
- `SMASH_CGROUP_WARM_MS=N`: while at a pressure level, also run keepWarm
  every N ms between ticks (resident store pages only; no read-back). The SIGUSR2 / `SMASH_DEBUG` stats line gains
`cg=level/headroomMiB/pressure_ticks/skipped/warm_pages/readback_pages`
(level -1 = policy inactive).

## Results (cloudnew, 24-core slice, 5 measured runs + 1 warmup per config, all interleaved)

Command (run 2026-11-17, `tmp/final2.json` on cloudnew; `old` = master
358f985, `off`/`noswapin`/`new` = this branch):

```
sudo -E python3 bench/run_cgroup_compare.py --build-dir build-linux \
  --config old=<master>/libsmash.so --config off=$L,SMASH_CGROUP=0 \
  --config noswapin=$L,SMASH_CGROUP_SWAPIN=0 --config new=$L \
  --apps sqlite,redis,rss --runs 5 --warmup 1 --seed 31 \
  --cap sqlite=250M:400M --cap redis=160M:240M --cap rss=160M:240M \
  --redis-server <redis 8 built with MALLOC=libc>
```

Medians (min–max). "disk swap-ins" is the host-wide `pswpin` delta (only these
cgroups were swapping).

**sqlite** (bench_sqlite, 500K rows, cool 30 s, serve 20 s)

| config | mean mem.current MiB | ops/s | cold p99 µs | disk swap-ins | zswpin |
|---|---:|---:|---:|---:|---:|
| glibc + zswap | 246 (246–246) | 90938 | 2284 (2261–2344) | 0 | 251712 |
| smash master | 195 (194–195) | 91601 | 3400 (3266–3553) | 29440 | 268908 |
| smash, policy off | 195 (195–197) | 90686 | 3464 (3430–3573) | 28833 | 268607 |
| smash, no read-back | 185 (184–187) | 91007 | 3553 (3418–3616) | 23675 | 239877 |
| smash, policy on | 185 (183–186) | 90753 | 2606 (2442–3121) | 25461 | 240452 |

**redis** (redis 8 with libc malloc, 300K SETs of 1 KiB, 30 s idle, 300K random GETs, 8 clients)

| config | mean mem.current MiB | GET ops/s | GET p99 µs | zswpin |
|---|---:|---:|---:|---:|
| glibc + zswap | 157 (157–158) | 50429 | 1479 | 103682 |
| smash master | 115 (114–115) | 40268 | 1583 | 223812 |
| smash, policy off | 115 (115–115) | 40639 | 1543 | 221249 |
| smash, policy on | 114 (114–115) | 40639 | 1551 | 179953 |

**bench_rss** (256 MiB, 40 s wait; memory only)

| config | mean mem.current MiB | steady MiB | peak swap MiB | zswpin | PSI some ms |
|---|---:|---:|---:|---:|---:|
| glibc + zswap | 159 | 160 | 99 | 159 | 166 |
| smash master | 62 | 8 | 262 | 93283 | 594 |
| smash, policy on | 61 | 9 | 110 | 27361 | 203 |

Reading: against master, the policy lowers sqlite mean memory 5%, its cold
p99 23% (3400→2606 µs; the read-back is the part that matters: 3553 µs
without it), redis zswpin 20% and bench_rss zswpin 71% / stall time 66%,
with no measured throughput change. Against glibc+zswap under the same cap,
smash uses 25–61% less memory on average, matches sqlite throughput, but
sqlite cold p99 is still 14% higher (2606 vs 2284 µs, the remaining store
swap-ins) and redis GET throughput is 19% lower. The redis gap is not caused
by the cap: uncapped, smash's redis peak memory.current was 263 MiB vs 204 MiB
for glibc (one calibration run), so under a 160M cap smash has more to swap.
That is allocator footprint, outside this policy.

### Alternatives measured and rejected (3 interleaved runs each, same caps, screening run `tmp/screen2.json`)

| config | sqlite mean MiB / cold p99 µs | redis mean MiB / GET ops/s |
|---|---|---|
| policy off | 195 / 3535 | 115 / 40944 |
| `SMASH_MLOCK_STORE=1` (mlock the store) | 211 / 2230 | 162 / **842** |
| `SMASH_CGROUP_SPILL=1` (blobs to the file-backed spill store at critical) | 191 / 3609 | 126 / 21709 |
| keep-warm only (first version: only at a pressure level) | 193 / 2884 | 115 / 41169 |
| keep-warm + skip + `SMASH_CGROUP_EARLY=1` | 166 / 3107 | 98 / 30048 |

- mlock removes sqlite's disk swap-ins but pins the whole store: under the
  redis cap 124 MiB of the 160 MiB became unevictable and everything else
  thrashed (9.1M zswpin, GET p99 218 ms). A soft hint (keep-warm) is the
  safe form of the same idea.
- Spill: clean page cache is cheap to drop, but a dropped spill page is a disk
  read on the next access, which is what the anonymous store already costs
  when swapped to disk, plus writeback and higher PSI.
- Early compression (lowered cold floor under pressure) lowers mean memory
  but cost redis 26% GET throughput (more refaults).

## Reproducing on another host

1. cgroup v2 at `/sys/fs/cgroup`, `memory` in the root's
   `cgroup.subtree_control`.
2. Swap and zswap, for example:
   ```
   sudo fallocate -l 16G /swapfile-smash && sudo chmod 600 /swapfile-smash
   sudo mkswap /swapfile-smash && sudo swapon /swapfile-smash
   echo zstd | sudo tee /sys/module/zswap/parameters/compressor
   echo zsmalloc | sudo tee /sys/module/zswap/parameters/zpool
   echo Y | sudo tee /sys/module/zswap/parameters/enabled
   ```
   Record the previous values and restore them afterwards.
3. Build with `-DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=OFF
   -DSMASH_BUILD_BENCH_ALLOCATORS=OFF` and copy each libsmash.so under test to
   its own file first (rebuilding the library in place during a run silently
   changes a config).
4. Run `bench/run_cgroup_compare.py` under `sudo -E` (it creates
   `/sys/fs/cgroup/smash-cgcmp/<run>` per run and drops the workload to
   `$SUDO_UID`). Distro redis packages link jemalloc statically, so
   `LD_PRELOAD` does nothing for them; pass a `MALLOC=libc` build with
   `--redis-server`.

The cold p99 numbers moved between days on the shared host (master's sqlite
cold p99 was 15–22 ms in one 3-run batch and 3.4–3.5 ms in later ones), so only
compare configs from the same interleaved batch.

## Bug found on the way (fixed in this branch)

Under a cgroup cap, as root, on the aarch64 dev VM (Ubuntu 24.04, kernel 6.8,
where idle-page tracking is readable), master SIGSEGV'd in every capped
bench_sqlite run (5/5). The idle-page read check falls back to
`mprotect(PROT_NONE)` when a page is swapped out (no PFN), but did not set
`prot_none_[i]`; in uffd mode compressPage's deferred snapshot skips the
PROT_RW mprotect unless that flag is set, so the worker read a PROT_NONE page.
Fix: `notePageProtNone()` in armReadCheck / escalateToDeepMonitoring. There is
no ctest for it (it needs root, swap and a capped cgroup); repro in the commit
message (8761687).

## Round 2 (2026-11-17/18): where the remaining gaps came from

cloudnew as above, but a 48-core taskset slice (136-183) and zswap
`zstd/zsmalloc`, one 16 GiB swapfile (`/swapfile-smash-cgwin`). Base is
master 4673d6c. All batches: 5 measured runs + 1 warmup per config, all
interleaved (`--seed` given), same caps as above (sqlite 250M:400M, redis and
rss 160M:240M), redis 8.8 built with `MALLOC=libc`. Each libsmash.so under
test is a copy in `tmp/`; `tmp/*.json` paths are the raw results on cloudnew
under `~/adoption/cgwin/`.

### 1. The gaps on current master

`run_cgroup_compare.py --config master=$M --config lo=$M,SMASH_LARGE_ONLY=1
--apps sqlite,redis --seed 41` (`tmp/base1.json`):

| app | config | mean mem.current MiB | ops/s | cold p99 / GET p99 µs | disk swap-ins |
|---|---|---:|---:|---:|---:|
| sqlite | glibc + zswap | 245.8 | 90003 | 2296 (2264–2385) | 0 |
| sqlite | smash | 185.2 | 89611 | 2444 (2374–2939) | 25770–32120 |
| sqlite | smash large-only | 245.9 | 89660 | 2291 | 3–12 |
| redis | glibc + zswap | 156.7 | 50378 (50109–50463) | 1487 | 0 |
| redis | smash | 115.8 | 39108 (38971–39830) | 1615 | 179–250 |
| redis | smash large-only | 154.6 | 49751 (49570–49850) | 1503 | 0 |

Both gaps were still there: sqlite cold p99 +6%, redis GET −22%. Redis
does not swap the store to disk; its loss has a different cause (section 3).

### 2. SQLite: two sources of store pages on disk

`bench/cgroup_phase_probe.py` (one capped bench_sqlite run per config, cgroup and
host counters every 0.1 s, bench stderr timestamped so phases line up; 2
reps per config) split smash's disk traffic by phase. Across 6 runs of
smash with the policy at its defaults: cooling phase 8.0–9.5K pswpout,
serve phase 3.9–5.0K pswpin (the budgeted MADV_WILLNEED read-back of
those), cold re-access phase 12–18K pswpout and 18–23K pswpin, cold phase
9.5–13.5 s. glibc (2 runs): no disk swap in any phase, cold phase 4.5 s.

- **Cooling.** The 0.1 s timeline shows memory.current at 104 MiB for ~10 s,
  then 36K zswpin in 0.5 s, memory.current at the 250 MiB cap, and 8.1K
  pswpout in the next 0.5 s. That is the compressor: `SKIP_SWAPPED=1` skips
  swapped-out pages only while a pressure level is held, and when the hold
  expires every page the kernel zswapped during the fill becomes a
  candidate at once. Snapshotting them swaps them in, pushes the cgroup to
  the cap, and the reclaim that follows takes freshly written store pages
  (incompressible: zswap rejects them, so they go to disk).
  `SKIP_SWAPPED=2` (skip whenever capped) removed this: cooling pswpout 0 in
  both probe runs.
- **Cold re-access.** Decompressing brings memory back to the cap and
  reclaim cycles the cgroup's LRU faster than once a second, so the tick's
  young bit does not protect the store. `WARM_MS=100` cut that phase's
  pswpout to 4.4–5.7K, `WARM_MS=20` to 2.8–3.2K (probe, 2 runs each).

5-run batch, `--config base=$L --config s2=$L,SMASH_CGROUP_SKIP_SWAPPED=2
--config w20=$L,SMASH_CGROUP_WARM_MS=20 --config w20s2=$L,...both...
--apps sqlite,redis,rss --seed 47` (`tmp/b2.json`):

**sqlite**

| config | mean mem.current MiB | ops/s | cold p99 µs | cold p50 µs | disk swap-ins | wall s |
|---|---:|---:|---:|---:|---:|---:|
| glibc + zswap | 245.9 | 89853 | 2286 (2197–2347) | 14.4 | 0 | 61.0 |
| smash | 184.9 | 89118 | 2659 (2390–3083) | 22.4 | 25911 | 68.5 |
| smash, SKIP_SWAPPED=2 | 186.3 | 89454 | 1904 (1822–2237) | 24.5 | 6113 | 64.4 |
| smash, WARM_MS=20 | 182.5 | 89866 | 2269 (2217–2336) | 23.7 | 11033 | 64.8 |
| smash, both | 186.1 | 90856 | **1777 (1740–1822)** | 24.8 | 4665 | 64.3 |

**redis**

| config | mean mem.current MiB | GET ops/s | GET p99 µs | zswpin |
|---|---:|---:|---:|---:|
| glibc + zswap | 156.9 | 50344 (50167–50497) | 1471 | 104805 |
| smash | 115.8 | 39042 (38961–39216) | 1623 | 188874 |
| smash, SKIP_SWAPPED=2 | 112.6 | 39620 | 1615 | 171865 |
| smash, WARM_MS=20 | 114.6 | 42111 (41976–42259) | 1487 | 172162 |
| smash, both | 111.2 | 41638 (41448–41725) | 1527 | 161054 |

**bench_rss**

| config | mean mem.current MiB | zswpin | majflt |
|---|---:|---:|---:|
| glibc + zswap | 159.1 | 159 | 119 |
| smash | 61.6 | 27557 | 4624 |
| smash, SKIP_SWAPPED=2 | 58.1 | 1442 | 1014 |
| smash, both | 58.0 | 1307 | 741 |

With both knobs, sqlite's cold p99 is 22% below glibc+zswap's (ranges do
not overlap) and 33% below default smash, at the same mean memory (186 vs
185 MiB) and throughput. The p50 is still higher than glibc's (24.8 vs
14.4 µs: a smash fault costs more than a zswap fault) and the run's cold
phase is still longer (wall time 64.3 vs 61.0 s). SKIP_SWAPPED=2 leaves more
data in swap (mean memory.current + memory.swap.current 404 vs 331 MiB for
sqlite), which matters only where memory.swap.max is set. Redis: +6.6% GET
over default smash; WARM_MS alone was 1.1% faster than both (ranges do not
overlap) but both used 3 MiB less mean memory. The knobs act only when a
cgroup limit is detected; outside a cgroup the policy is inactive, so the
uncapped paper workloads cannot be affected, but `run_quick_ci.py` /
`check_paper_regression.py` were not re-run on Linux this round (SSH access
ended), so both knobs stay opt-in.

### 3. Redis: footprint, not the store

Uncapped decomposition (`bench/redis_footprint.py`, same redis build and load,
no cgroup, 3 runs + warmup per config; RSS sampled every 0.2 s, INFO memory
and smaps at each phase; `tmp/decomp2.json`):

| config | peak RSS MiB | RSS after fill | after 30 s idle | after GETs | used_memory MiB | GET ops/s |
|---|---:|---:|---:|---:|---:|---:|
| glibc | 208 | 208 | 208 | 208 | 198 | 63.3K–63.7K |
| smash | 269–273 | 241–242 | 65 | 242 | 220–221 | 59.4K–59.9K |
| smash, SMASH_FINE_1K_CLASSES | 256–258 | 228 | 62 | 228–229 | 209 | 59.5K–60.2K |

- redis-benchmark's `-r 300000` gives ~190K distinct keys; each 1 KiB value
  is a 1030 B sds string. smash puts it in the 1152 B class; glibc's chunk
  is 1040 B. That is 120 B per value, the 23 MiB difference in used_memory
  (redis counts malloc_usable_size), and most of the +33 MiB at the end of
  the fill. The rest is the smash heap's slack (heap VMA RSS − used_memory:
  5 MiB vs 3 for glibc's [heap]) and smash's own metadata mappings.
- Uncapped, smash's GET loss is small (−5%): in one run, 20.5K
  decompression faults and 0.42 s total fault service time
  (`SMASH_METRICS_FILE`).
- Under the 160M cap smash swaps more: zswpin 188K vs 105K (1.8x). The
  uncapped footprints exceed the cap by 82 vs 48 MiB (1.7x); that the
  footprint drives the extra swapping is a hypothesis consistent with the
  frontier below (more cap, or a smaller footprint, both raise smash's GET
  rate), not something measured directly.

`SMASH_FINE_1K_CLASSES` (new CMake option, off by default) adds 8 classes
(1088 .. 2048 in steps of 64) so these values take 1088 B.

Frontier batch, redis only, `--seed 53` (`tmp/r3.json`; `ws` = both
cgroup knobs; `@cap` = per-config cap):

| config | cap | mean mem.current MiB | GET ops/s | GET p99 µs | zswpin |
|---|---|---:|---:|---:|---:|
| glibc + zswap | 160M | 156.8 | 50268 (50167–50633) | 1471 | 104673 |
| glibc + zswap | 130M | 127.3 | 37932 (37755–38304) | 1719 | 242712 |
| glibc + zswap | 115M | 113.0 | 31810 (31642–31952) | 1823 | 344115 |
| smash | 160M | 115.9 | 39159 (38815–39463) | 1599 | 187840 |
| smash ws | 160M | 111.4 | 41684 (41317–41759) | 1511 | 160741 |
| smash ws | 190M | 123.8 | 47326 (47177–47551) | 1359 | 96967 |
| smash ws + EAGER_FILL + PEAK_GUARD | 160M | **85.3** | 41586 (41414–41690) | 1511 | 161400 |
| smash ws, FINE_1K_CLASSES build | 160M | 108.1 | **45188 (43541–45228)** | 1375 | 132257 |
| smash large-only | 160M | 154.9 | 50243 (49423–50319) | 1487 | 109255 |

- At equal mean memory smash is ahead: 41.7K GET/s at 111 MiB vs glibc's
  31.8K at 113 MiB (+31%). At equal cap glibc is ahead, because glibc then
  uses 41% more memory on average.
- Finer classes: +8.4% GET over ws at 3 MiB less mean memory; the gap to
  glibc at the same cap goes from −17% to −10%.
- `SMASH_EAGER_FILL=1 SMASH_PEAK_GUARD=1` (docs/PEAK.md) compresses the
  write-once fill before it goes cold: mean memory −23% (85 vs 111 MiB) at
  the same GET throughput. Not measured on capped sqlite/rss this round.
- Large-only mode matches glibc+zswap on throughput and memory: Redis's
  allocations are all small, so large-only passes them to glibc. Under a cap,
  large-only is the config for "same as glibc"; full mode with the knobs is
  the config for "less memory".

Combination batch, sqlite + redis + rss, `--seed 59` (`tmp/b4.json`; `ws` =
both cgroup knobs, `eg` = `SMASH_EAGER_FILL=1 SMASH_PEAK_GUARD=1`, `fine` =
the FINE_1K_CLASSES build):

| config | sqlite mean MiB | sqlite cold p99 µs | sqlite ops/s | redis mean MiB | redis GET/s | rss mean MiB |
|---|---:|---:|---:|---:|---:|---:|
| glibc + zswap | 245.9 | 2285 (1920–2380) | 89691 | 157.1 | 50142 (50000–50386) | 158.8 |
| ws | 185.8 | 1850 (1784–1911) | 89562 | 111.6 | 41586 (41459–41789) | 58.0 |
| ws + eg | 185.9 | 1750 (1717–1861) | 88592 | 85.5 | 41505 | 19.9 |
| fine ws | 185.6 | 1805 (1743–1895) | 90708 | 109.5 | 44053 (43802–44274) | 58.3 |
| fine ws + eg | 186.5 | **1726 (1707–1823)** | 89477 | **82.8** | **44066 (43840–45338)** | **20.3** |

- `ws` reproduces b2/r3 (sqlite 1850 vs 1777 µs there, redis 41.6K in all
  three batches).
- Eager fill + peak guard: no memory change on capped sqlite (its fill is
  not write-once, docs/PEAK.md), cold p99 −5%, sqlite throughput −1.1%;
  redis mean memory −23% and bench_rss −66% at unchanged throughput.
- Finer classes: redis +5.9% GET, sqlite and rss unchanged (no 1–2 KiB
  objects).
- Best measured capped config: fine + ws + eg. vs glibc+zswap at the same
  cap: sqlite cold p99 −24% at 24% less mean memory; redis GET −12% at 47%
  less mean memory; bench_rss 87% less mean memory.

### Uncapped checks for the defaults question

Outside a capped cgroup the policy is off (`SMASH_CGROUP=1` logs "no
memory limit found, policy off" in the cloudnew user slice), so the two
cgroup knobs cannot change uncapped behaviour. Measured anyway:
`run_quick_ci.py` (bench_rss + bench_sqlite), 5 shuffled rounds of
{full, large-only} × {knobs off, `SKIP_SWAPPED=2 WARM_MS=20`}: all 20 runs
PASS; bench_rss best reduction 87% in every run; bench_sqlite median 59.1%
(off, 58.9–59.5) vs 59.1% (on, 59.0–59.2) in full mode, 27.2% (26.8–27.5)
vs 27.2% (27.2–27.3) in large-only.

Fine-class build vs default, uncapped paper workloads:
`run_paper_experiments.py --compress-only-only --configs full_smash --apps
sqlite,rocksdb,memcached,redis,redis_ext --runs 1`, 5 rounds per build from
`build-paper` / `build-paper-fine` (Release, bench deps on), build order
shuffled per round; one default round failed to configure (transient alloc8
fetch error) and was re-run after the others. `check_paper_regression.py`
passes for both. Median (min–max) RSS reduction %, serve ops/s:

| app | default | fine classes |
|---|---|---|
| SQLite | 70.6 (70.5–70.6), 90.4K (86.6–92.2K) | 70.5 (70.4–70.6), 89.4K (89.3–90.8K) |
| RocksDB | 85.7 (85.5–85.8), 648K (623–671K) | 85.6 (85.5–85.6), 636K (630–651K) |
| memcached | 89.8 (89.7–89.8), 215K (211–218K) | 89.7 (89.2–89.7), 214K (212–215K) |
| Redis | 58.6 (58.6–58.6) | 58.5 (58.4–58.5) |
| Redis-ext | 67.1 (66.9–67.7) | 67.1 (66.7–67.2) |

Peak RSS medians are identical (406 / 314 / 295 / 283 / 283 MB). Reductions
differ by ≤ 0.1 point; the throughput medians are 0.7–1.8% lower with fine
classes but every fine range lies inside the default range.

### Bug found on the way (fixed)

Redis's `setproctitle` overwrites the original environment block after
moving `environ`; smash kept getenv() pointers for `SMASH_METRICS_FILE`,
`SMASH_REPORT`, `SMASH_CONTROL_FILE`, `SMASH_OBSERVE_REPORT`,
`SMASH_SPILL_DIR` and `SMASH_CGROUP_ROOT`, so under redis-server the metrics
file went to `.tmp.<pid>` in the cwd (strace) and was never renamed into
place. They are now copied at init (`bootstrapEnv`); `test_report_clobbered_env`
zeroes the values in place and fails without the fix.

## Open issues

- sqlite cold p50 under a cap is higher than glibc+zswap's (24.8 vs
  14.4 µs) even with both round-2 knobs: a smash decompression fault (signal,
  decompress, mprotect) costs more than an in-kernel zswap fault.
- redis under a 160M cap: −10% GET vs glibc+zswap with finer classes and both
  knobs (at 31% less mean memory). The remaining footprint gap is 1088 vs
  1040 B per value plus heap slack.
- `SMASH_CGROUP_SKIP_SWAPPED=2`, `SMASH_CGROUP_WARM_MS=20` and
  `SMASH_FINE_1K_CLASSES` are still opt-in; the evidence for making them
  defaults is above (capped wins in three batches, uncapped checks neutral).
- Not measured: Kubernetes itself (the example manifest is untested on a
  cluster), memory.max-only caps without memory.high, hosts without zswap
  (disk swap only), cgroup v1.
