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
- Read tracking with the bitmap (`read_track=page_idle` or `smash-idled`) was
  first exercised on the arm64 dev VM on 2026-11-17 and was broken there: with
  PROT_READ write tracking (no soft-dirty) Phase 1 consulted the idle bit of
  every ACTIVE_MONITORING page, but a page is only armed (bit set) at the base
  floor in `escalateToDeepMonitoring`, so every unarmed page read "accessed",
  its cold count reset each tick, and it never reached the floor:
  `would_compress=0` for 45 s. The same applied to a real (non-observe) run
  with root or smash-idled on arm64. Now `armIdleRead()` records which pages
  are armed (`idle_armed_`), and Phase 1 and the smash-idled QUERY consult
  only those. A THP-backed page is armed and tested through its folio head
  PFN (`src/vm/page_idle.h`), in-process and in smash-idled.
- Measured with `tests/observe_read_track_test.cpp` (dev VM): reads of a
  reclaimed region count as `refaults_read` (+4096..4103 for a 16 MiB region,
  `refaults_write` unchanged), in both modes, in-process and via smash-idled.
  A region read every 100 ms (never written) was still projected once in the
  first ~3 s and then counted as read refaults before it backed off; why it
  passed the base-floor arm at all has not been established.
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
`test_observe_read_{root,idled}_{full,large_only}[_thp]`
(`tests/observe_read_track_test.cpp`, Linux): read tracking through the idle
bitmap — `root` re-runs the test as root (`sudo -n`), `idled` starts
`sudo -n smash-idled -p <test pid>` on a private socket and runs the
preloaded child unprivileged. Asserts a 32 MiB cold set is projected, a
read-hot region is not projected after 30 more ticks, and reading (not
writing) a projected 16 MiB region yields >= 80% of its pages as
`refaults_read`. Skip (77) without page_idle, passwordless sudo, or if smash
reports `read_track=none`; `_thp` also needs THP `always` and chunks that
really got huge pages. Before the arming fix all four non-THP cases failed
(`would_compress_bytes=0`); before the head-PFN fix the four `_thp` cases
failed under `THP=always` (`refaults_read grew by 8, want >= 3276`).

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
- `idle` marks every tracked page's PFN idle at t0 and reads the bitmap at
  each point. Two details matter (both fixed 2026-11-17 after the first runs
  on a host with the bitmap, the dev VM below):
  - **Transparent huge pages.** The bitmap tracks a large folio by its head
    PFN only: writes to a tail PFN's bit are ignored and a tail always reads
    "not idle" (`mm/page_idle.c`). pagemap gives each 4 KiB page its own PFN,
    so before the fix every tail of a THP read as accessed: `estimate_target`
    under `THP=always` reported 2.2 MiB idle instead of 80 MiB. The tool now
    resolves each page's folio head through `/proc/kpageflags`
    (`src/vm/page_idle.h`, shared with smash and smash-idled), so one access
    anywhere in a 2 MiB folio marks all 512 pages hot (that is the kernel's
    granularity) and the output notes how many pages were in large folios.
  - **The shared zero page.** An anonymous page that was read but never
    written maps the zero page: present in pagemap, absent from RSS, and
    never idle (not on an LRU). It was counted in `anon_present_bytes`
    (128 MiB "present" vs 113 MiB RSS for `estimate_target`). With PFNs
    visible (root) such pages are now skipped in every method and noted.
  Unprivileged runs (soft-dirty, referenced) cannot see PFNs, so they still
  count zero-page PTEs as present; soft-dirty additionally treats them as
  idle up to the mapping's unreferenced bytes.
- `--method auto` picks `idle` whenever the bitmap opens and pagemap shows
  PFNs (root), even where soft-dirty works; on the arm64 dev VM it picks
  `idle` as root and `referenced` otherwise (arm64 never sets soft-dirty).
- **smash-idled cannot make `idle` unprivileged.** The daemon answers only
  for the connecting process's own pages (it reads the peer's pagemap from
  `SO_PEERCRED`) and only for one registered contiguous range; the estimator
  needs another process's many mappings. Supporting it would mean a new op
  that names a target pid, a scope rule for it (the target must be in the
  daemon's scope and the client must already be able to ptrace it), and
  multi-range registration — a larger, security-sensitive change to a root
  daemon, for a one-shot tool whose user can run `sudo smash-estimate`
  (which the daemon itself needs anyway). Not done.

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
target with 64 MiB compressible-idle, 16 MiB random-idle, 16 MiB write-hot,
16 MiB read-hot and 16 MiB read-only (zero page) regions; asserts idle in
[72, 92] MiB and saving in [54, 72] MiB. Unprivileged, so they run in CI.
`test_smash_estimate_idle` and `test_smash_estimate_idle_thp` force
`--method idle`, running smash-estimate via `sudo -n` when not root, and
exit 77 (skipped) without passwordless sudo or `/sys/kernel/mm/page_idle`;
they also assert `anon_present_bytes <= RSS + 4 MiB` (zero page excluded).
The `_thp` variant has the target `madvise(MADV_HUGEPAGE)` its regions (THP
must not be `never`) and requires the "large folios" note. Before the fixes:
`_idle` failed with "anon present 128.0 MiB > RSS 112.9 MiB", `_idle_thp`
with "idle 2.2 MiB not in [72, 92]"; after: both pass (dev VM, 2026-11-17).

## Comparing estimates with a real run

`bench/validate_estimator.py --build-dir build-bench --runs 5` (needs
`-DSMASH_BUILD_BENCH=ON`) runs bench_sqlite under glibc (+ `smash-estimate`),
observe mode, and full smash in random interleaved order and compares, at the
same point of the serve phase, each projection with the measured RSS
reduction. `--methods idle,referenced` runs one glibc config per
`smash-estimate --method` (reported per config). Measured numbers are in the
section below.

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

**Idle method, dev VM** (Lima `smash`: Ubuntu 24.04 arm64, kernel 6.8, 10
vCPUs on an Apple-silicon Mac, THP `madvise`, 2026-11-17; 5 randomized
interleaved rounds, medians; smash-estimate as root via `sudo -n`; observe
mode `write_track=prot-read`, `read_track=none`). Command:
`python3 bench/validate_estimator.py --build-dir build-vm --runs 5 --rows 650000 --serve 60 --at 50 --window 10 --methods idle,referenced`

| | RSS (MiB) | projected / measured reduction |
|---|---|---|
| glibc | 849.0 | `idle`: **63.0%**; `referenced`: **56.2%** of glibc RSS |
| observe | 871.7 | observe projection: **63.4%** |
| smash full mode | 517.2 | measured: **39.1%** vs glibc, **40.7%** vs observe |

Both methods overestimate here (+23.9 pp idle, +17.1 pp referenced). That
`idle` reports more idle bytes than `referenced` is expected on a growing
heap: `referenced` subtracts each mapping's Referenced bytes from its
Anonymous bytes at t0, and pages the heap gains during the window count as
referenced (bench_sqlite's glibc RSS grows ~8 MiB/s in the serve phase), while
`idle` only follows pages present at t0. That accounts for the direction;
whether it accounts for the whole 6.8 pp has not been measured. The gap to the
measured smash run is not decomposed (candidates: the unmodeled items above,
plus smash's refault backoff after the rehash described below, which lands
inside the first ~20 s of serve on this VM).

Same harness, default `--rows 500000 --at 25 --window 10`: `idle` 3.8%,
`referenced` 48.0%, measured 42.8%. `idle` was right about the window — the
pager rehash (below) fell inside it in all five idle runs and read every page
— and `referenced` saw it in 1 of 5 runs; when the rehash lands relative to
the window varies with insert throughput (not investigated further). At
`--at 14 --window 8` smash itself had not finished compressing in 3 of 5 runs
(RSS 373 vs 239 MiB), and at `--at 40` it measured only 6.5%: after the rehash
decompresses everything, smash's refault backoff delays recompression, which
no window-based estimate models.

`estimate_target` (launch mode, 4 s window, VM): `idle` and `referenced`
both report 80.0 MiB idle (exact: 80 MiB) and 62.3-62.7 MiB saving, with THP
off and (after the fix) with `THP=always`.

Pitfall seen while measuring: bench_sqlite's in-memory pager rehashes its page
cache as the DB grows (a read of every page). One lands ~30 s into the serve
phase; an estimate window that contains it reports ~0% idle — correctly, since
smash would decompress everything at that moment too. Estimates are only as
representative as the window.
