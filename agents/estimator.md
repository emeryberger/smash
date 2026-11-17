# Estimating savings without risk: `SMASH_MODE=observe` and `smash-estimate`

Two ways to find out how much smash would save before running it for real:

| | `SMASH_MODE=observe` | `smash-estimate <pid>` |
|---|---|---|
| Needs | preload + restart | nothing (Linux, running process) |
| Allocator | smash's (full or `SMASH_LARGE_ONLY=1`) | the app's own (glibc, jemalloc, ...) |
| Cold detection | smash's own (same thresholds, ROI, backoffs) | idle over one window |
| Compression | smash's codec + ROI per page, blob discarded | zstd-1 on a random sample |
| Refaults | estimated (see below) | not estimated |
| Output | `[smash observe]` stderr lines + JSON file | text or `--json` |

## Observe mode (`SMASH_MODE=observe`)

```
LD_PRELOAD=libsmash.so SMASH_MODE=observe SMASH_OBSERVE_REPORT=/var/tmp/smash.json ./server
LD_PRELOAD=libsmash.so SMASH_MODE=observe SMASH_LARGE_ONLY=1 ./server       # large-only
```

`getSmashMode()` reports `Full` for `observe`; the allocator is exactly the
one a real run uses. Only the compressor changes (`compressor_thread.h`,
`observe_` / `obs_*`):

- **Phase 2 / `compressPage()`** runs the normal eligibility, ROI and tier
  logic, snapshots the page into the worker scratch buffer (Linux:
  `process_vm_readv` on self, so an unreadable page is an EFAULT rather than a
  fatal worker fault; macOS: memcpy), zeroes free slots as usual, compresses,
  and **discards the blob**. The page keeps its state (ACTIVE or
  ACTIVE_MONITORING) and protection; nothing is stored, madvised or written.
  `sc_stats` are updated as in a real run so later ROI decisions match.
- **Lifecycle per page** mirrors deferred reclaim: `kObsShadow` at snapshot;
  after `getDeferredReclaimDelay()` ticks Phase 1 re-reads the page and
  compares a hash of the free-slot-zeroed contents with the snapshot's (the
  same check Phase B does before dropping a frame). Match -> `kObsReclaimed`
  (a real run would have dropped the frame); mismatch or a tracked write ->
  a *discard* (a real run cancels, no fault). Any access the tracker sees on a
  reclaimed page is a **projected refault**; freeing the memory is not.
- **Disabled in observe mode**: userfaultfd registration, deep-monitoring
  PROT_NONE (`escalateToDeepMonitoring`), the PROT_NONE pre-compression read
  check (`armReadCheck`, `readCheckOff()`), and therefore Phase B, the
  deferred-madvise sweeper, spill and prefetch never see a page.

### Risk model

What observe mode can still do to the application:

1. **The allocator is smash's.** Full mode replaces malloc for everything;
   large-only replaces it for allocations > 16 KiB. Allocator bugs, layout
   differences and the memory overhead of smash's metadata are all present.
   This is the main residual risk; large-only is the lower-risk setting.
2. **Write tracking.**
   - Linux with working soft-dirty (x86-64): no protection changes at all.
     The kernel write-protects PTEs after each `clear_refs` (minor faults,
     invisible to the app) — the same mechanism a default smash run uses.
   - Linux without soft-dirty (arm64: the kernel accepts `clear_refs 4` but
     never sets bit 55 — probed at startup by `obsSoftDirtyWorks()`), macOS,
     or `SMASH_SOFTDIRTY=0`: Phase 3 **PROT_READ** write monitoring is
     required for cold detection. The first write to each monitored page per
     tick takes a SIGSEGV that smash's handler resolves (PROT_RW, mark
     accessed). Exposure is the same as a normal smash run's monitoring:
     interplay with an application SIGSEGV handler, syscalls writing into a
     PROT_READ buffer (EFAULT -> smash's retry wrappers), VMA count growth.
     **No page is ever made PROT_NONE and no data is ever changed.**
   - `SMASH_NO_MONITOR=1` removes the PROT_READ faults but leaves cold
     detection blind on those platforms (every page looks cold); the
     projection is then an upper bound with no refault signal.
3. **CPU**: snapshots + compression of every page that becomes eligible, plus
   one re-read per page after the shadow delay, on the compressor threads.

### What the projection does and doesn't capture

- `projected_saving_bytes` = reclaimed pages x page size − their blob bytes −
  blob bytes of pages still in the shadow window (a real run holds both). Blob
  bytes are `CompressStore::store()`'s charge (16-byte rounding). **Not
  modeled**: CompressStore region fragmentation and partially drained
  regions, per-page metadata, decompression buffers, and the RSS that real
  refaults would re-inflate (observed refaults are reported separately).
- Reads: only visible with the idle-page bitmap (root or `smash-idled`). Without
  it (`read_track=none`) a read-hot page that is never written counts as
  reclaimed and its reads are not counted as refaults, so the saving is an
  upper bound and `refault_rate` a lower bound for read-heavy workloads. A real
  run would also have filtered such pages with the PROT_NONE read check, which
  observe mode skips.
- Allocator feedback loops: a real run steers new allocations away from
  compressed pages (`COMPRESSED_PAGE` slot class) and the arena feedback hook
  sees compressions; observe pages stay ACTIVE, so placement can differ.
- Write-detection gap: writes between Phase 1's scan and the end-of-tick
  `clear_refs` are caught by the shadow hash check only during the shadow
  window, as in a real run.

### Report

Every `SMASH_OBSERVE_INTERVAL` seconds (default 10; 0 = exit only) and at exit
(`stop()`), one line on stderr:

```
[smash observe] interval pid=… t=…s mode=full write_track=soft-dirty read_track=none rss=…MiB peak_rss=…MiB
  managed=…MiB would_compress=…MiB blob=…MiB projected_saving=…MiB projected_rss=…MiB reduction=…%
  avg_reduction=…% peak_relative=…% refaults=N(w=…,r=…) refault_rate=…% discards=… rejected=…
  hist[rej,<2,2-4,4-8,8-16,16+,zero]=…
```

With `SMASH_OBSERVE_REPORT=path` the same data is rewritten as JSON at every
report (fork children write `path.<pid>`). Fields: `rss_bytes`,
`peak_rss_bytes` (max of sampled RSS and `ru_maxrss`), `managed_bytes`
(resident smash-managed pages), `would_compress_bytes`, `shadow_bytes`,
`blob_bytes`, `projected_saving_bytes`, `projected_rss_bytes`,
`projected_reduction_pct` (saving / current RSS), `avg_projected_reduction_pct`
(time average over ticks), `peak_relative_reduction_pct`
(1 − min projected RSS / peak RSS, the paper's metric), `snapshots`,
`rejected` (ratio gate), `zero_pages`, `shadow_discards`, `reclaims`,
`refaults_write`, `refaults_read`, `refault_rate_pct` (refaults / reclaims),
`freed_while_compressed`, `ratio_hist`.

Tests: `test_observe_full`, `test_observe_large_only` (`tests/observe_mode_test.cpp`):
cold regions projected, a hot region not, rewriting a projected region counts
refaults, SIGUSR2 stats show `compressed=0 shadow=0`, RSS keeps every page,
every byte reads back.

## `smash-estimate` (Linux)

```
smash-estimate [--window 10] [--points 3] [--samples 4096] [--method auto] [--json] <pid>
smash-estimate [options] [--warmup 5] [--kill] -- <command> [args...]
```

Built by default on Linux (`SMASH_BUILD_ESTIMATE`, `tools/smash-estimate/`),
installed to `bin/`. Steps: list anonymous private writable mappings
(`/proc/pid/maps`), record present pages (`pagemap`), reset tracking, wait
`--window` seconds reading the tracker at `--points` evenly spaced times
(idle-age curve), then sample pages that stayed idle, compress each with
zstd-1 under smash's store rules (all-zero page = free, blob > 75% of a page =
stays uncompressed, 16-byte blob granularity) and scale the mean per-page
saving by the idle page count. The 95% CI is from the sampling variance (with
finite-population correction); it does not cover idle-detection error.

**Idle methods** (`--method auto` takes the first available):

| method | detects | granularity | needs |
|---|---|---|---|
| `idle` | reads + writes | page | root (PFNs) + `CONFIG_IDLE_PAGE_TRACKING` |
| `soft-dirty` | writes; reads via per-mapping cap | page | same uid; arch with soft-dirty (x86-64) |
| `referenced` | reads + writes | mapping | same uid |

- `soft-dirty` also clears referenced bits and caps each mapping's idle bytes
  by its unreferenced bytes (`smaps Referenced`), thinning that mapping's
  sample pool to match. A VMA that changes during the window (a growing brk
  heap) gets `VM_SOFTDIRTY` and every page reads dirty; such mappings (more
  dirty than referenced bytes, impossible otherwise) fall back to the
  `referenced` method, noted in the output.
- `referenced` cannot tell which pages are idle, only how many, so it samples
  compressibility from all present pages of each mapping.
- `idle` is implemented but has not been exercised on a host with
  `CONFIG_IDLE_PAGE_TRACKING` and root (the dev VM had the bitmap, cloudnew
  does not); treat it as untested.

**Permissions.** Reading page contents uses `process_vm_readv`, i.e. ptrace
access: root, being the target's parent (`-- <command>`), yama
`ptrace_scope=0`, or a target that called `prctl(PR_SET_PTRACER)`. Without it
the idle bytes are still printed, with no savings (exit code 3).

**Side effects on the target** (no memory contents are written): `clear_refs`
makes the kernel write-protect / age the target's PTEs (minor faults on its
next write per page; pages look older to reclaim); a target that relies on
soft-dirty itself (CRIU, a smash run) loses one window of write data. The
idle method sets idle flags in the global page_idle bitmap.

**Classes and what smash would manage** (heuristic, from `/proc/pid/maps`):
`heap` (brk; glibc small allocations: full mode; large-only only for
allocations >= 1 MiB), `malloc-arena` (64 MiB-aligned glibc thread arenas:
full mode only), `anon-mmap` (malloc allocations >= 1 MiB in both modes, or
direct mmaps, which need `SMASH_TRACK_EXTERNAL=1`), `stack` (from each task's
`/proc/pid/task/*/syscall` stack pointer, else 8 MiB mappings above a guard
page; never managed). The projection assumes every idle page of a class is
managed — read the class notes.

**Not modeled**: smash's own layout (it segregates call sites into arenas and
keeps allocator metadata out of data pages, so a glibc heap's access pattern
— chunk headers touched by frees and consolidation — overstates touches);
compression thresholds (smash's default cold timeout is 10 s: use a window at
least that long); refaults; CompressStore fragmentation.

Tests: `test_smash_estimate` (auto method) and `test_smash_estimate_referenced`
(`tests/estimate_test.cpp` + `tests/estimate_target.cpp`): launch mode, a
target with 64 MiB compressible-idle, 16 MiB random-idle, 16 MiB write-hot and
16 MiB read-hot regions; asserts idle in [72, 92] MiB and saving in
[54, 72] MiB. Unprivileged, so they run in CI.

## Comparing estimates with a real run

`bench/validate_estimator.py --build-dir build-bench --runs 5` (needs
`-DSMASH_BUILD_BENCH=ON`) runs bench_sqlite under glibc (+ `smash-estimate`),
observe mode, and full smash in random interleaved order and compares, at the
same point of the serve phase, each projection with the measured RSS
reduction. Measured numbers are in the section below.

### Dev measurements

Development numbers, not results: cloudnew (EPYC 9R14, kernel 6.12 x86-64)
under a 24-core taskset, 2026-11-16, 5 randomized interleaved rounds, medians.
Command: `python3 bench/validate_estimator.py --build-dir build-bench --runs 5`
(bench_sqlite `--rows 500000 --cool 5 --serve 60`, measured 25 s into the
serve phase; `smash-estimate --window 10` run via `sudo -n`, method
`soft-dirty`, heap via the changed-VMA Referenced fallback; observe mode
`write_track=soft-dirty`).

| | RSS (MiB) | projected / measured reduction |
|---|---|---|
| glibc | 524.9 | `smash-estimate`: **50.8%** of glibc RSS (CI half-width ~0.25 MiB) |
| observe (smash allocator, nothing reclaimed) | 563.6 | observe projection: **58.8%** of observe RSS |
| smash full mode | 293.1 | measured: **44.2%** vs glibc, **48.0%** vs observe |

Gap: `smash-estimate` overestimates the measured reduction by **6.6 pp**;
observe mode by **10.8 pp** (median projected saving 329.9 MiB vs 270.5 MiB
between the median observe-run and smash-run RSS). Both are overestimates; the unmodeled
items above (CompressStore fragmentation and metadata, refault re-inflation,
reads invisible without idle tracking) are the candidate causes — this has
not been decomposed. Single workload; treat as a sanity check, not a
calibration.

Pitfall seen while measuring: bench_sqlite's in-memory pager rehashes its page
cache as the DB grows (a read of every page). One lands ~30 s into the serve
phase; an estimate window that contains it reports ~0% idle — correctly, since
smash would decompress everything at that moment too. Estimates are only as
representative as the window.
