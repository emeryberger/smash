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
below). The SIGUSR2 / `SMASH_DEBUG` stats line gains
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

## Open issues

- sqlite cold p99 is still above glibc+zswap under a tight cap: some store
  pages are swapped in the same tick they are written, before keep-warm runs.
- redis under a cap loses GET throughput to glibc because smash's uncapped
  footprint for 1 KiB values is larger.
- Not measured: Kubernetes itself (the example manifest is untested on a
  cluster), memory.max-only caps without memory.high, hosts without zswap
  (disk swap only), cgroup v1.
