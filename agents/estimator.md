# Estimating savings without risk: `SMASH_MODE=observe` and `smash-estimate`

Two ways to find out how much smash would save before running it for real:

| | `SMASH_MODE=observe` | `smash-estimate <pid>` |
|---|---|---|
| Needs | preload + restart | nothing (Linux, running process) |
| Allocator | smash's (full or `SMASH_LARGE_ONLY=1`) | the app's own (glibc, jemalloc, ...) |
| Cold detection | smash's own (same thresholds, ROI, backoffs) | idle age at the end of a window |
| Compression | smash's codec + ROI per page, blob discarded | zstd-1 on a random sample |
| Refaults | estimated (see below) | not estimated |
| Refault backoff | assumed without read tracking (held pages) | assumed with a long enough window |
| smash's own RSS | in the measured RSS (same allocator) | modeled: metadata, runtime, glibc-chunk layout |
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
4. **Referenced bits** (Linux, no idle bitmap, `SMASH_OBSERVE_REFERENCED`
   default on): every 2 ticks observe mode reads `/proc/self/smaps` and
   writes `/proc/self/clear_refs` `1`, which clears the young bits of the
   process's pages (no data or protection changes). Pages then look older
   to the kernel's LRU, so under memory pressure they may be reclaimed
   sooner, as with `smash-estimate`. `SMASH_OBSERVE_REFERENCED=0` turns it
   off (and with it the read-hot correction below).

### What the projection does and doesn't capture

`projected_saving_bytes` = reclaimed pages x page size − their blob bytes −
blob bytes of pages still in the shadow window (a real run holds both) −
the metadata a real run writes per compressed page (`kRunMetaPerPage`,
27 B: `CompressedPageInfo`, ticks, flags) + the resident observe-only
arrays − two holds that model how a real run behaves beyond "not written
for the cold timeout":

- **Refault backoff** (`backoff_held_bytes`). A real run doubles the cold
  wait of a page, and of fresh pages from the same (arena, size class),
  once one of them refaults (`bucket_rc_ema_x256_`). Observe mode does not
  simulate that. When it cannot see reads (no idle bitmap) or once it has
  projected a refault, it assumes the backoff: reclaimed pages idle for
  less than 2x the cold floor + the deferred-reclaim delay are counted
  resident. This is transient for a page (it ends at that idle age) but
  steady for a growing workload: on bench_sqlite a few hundred read
  refaults right after the first cold wave doubled the floor of the
  buckets that every serve-phase page lands in, and the real run held
  54 MiB more than the same run with `SMASH_RECOMPRESS_BACKOFF=0` (5 runs,
  below).
  `assumes_backoff` says whether the hold applies.
- **Read-hot pages** (`read_held_bytes`, Linux without the idle bitmap).
  Write tracking cannot see reads, so a page that is only read counts as
  reclaimed; a real run keeps it (pre-compression read check) or refaults
  it. Observe mode reads the arena's smaps `Referenced` every 2 ticks
  (`clear_refs 1`, see the risk model), subtracts the pages it read itself
  (snapshots, shadow re-reads), and keeps the largest sub-window of the
  last cold timeout + 2 ticks (`referenced_bytes`). At least
  `max(0, referenced − non-reclaimed resident)` of those pages must be
  among the reclaimed ones; they are counted resident. This is a lower
  bound: a page read less often than every 2 ticks can be missed.

`projected_saving_no_backoff_bytes` is the projection without either hold
(the round-1 formula plus the metadata terms). Still **not modeled**:
CompressStore fragmentation from refault churn (bench_kv_store: 56.3 MiB of
resident store pages for 46.4 MiB of live blobs), the RSS of pages that
refault and stay resident, allocator feedback loops (a real run steers new
allocations away from compressed pages, observe pages stay ACTIVE), and
the write-detection gap between Phase 1's scan and the end-of-tick
`clear_refs` (caught by the shadow hash check only inside the shadow
window, as in a real run).

With the idle bitmap (root or `smash-idled`) reads are visible per page:

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
`projected_saving_no_backoff_bytes`, `backoff_held_bytes`,
`read_held_bytes`, `referenced_bytes` (-1 = no signal), `assumes_backoff`,
`observe_metadata_bytes`, `run_metadata_bytes`, `rss_parts` (see
`smash_rss_part_bytes` below),
`projected_reduction_pct` (saving / current RSS), `avg_projected_reduction_pct`
(time average over ticks), `peak_relative_reduction_pct`
(1 − min projected RSS / peak RSS, the paper's metric), `snapshots`,
`rejected` (ratio gate), `zero_pages`, `shadow_discards`, `reclaims`,
`refaults_write`, `refaults_read`, `refault_rate_pct` (refaults / reclaims),
`freed_while_compressed`, `ratio_hist`.

Tests: `test_observe_full`, `test_observe_large_only` (`tests/observe_mode_test.cpp`):
cold regions projected, a hot region not, rewriting a projected region counts
refaults, SIGUSR2 stats show `compressed=0 shadow=0`, RSS keeps every page,
every byte reads back. `test_observe_projection_{full,large_only}`
(`tests/observe_projection_test.cpp`, cold timeout 6 s): right after the
first projected reclaim the backoff hold covers the cold region and later
ends; on Linux a region that is only read lands in `read_held_bytes`.
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
smash-estimate [--window 10] [--points 3] [--cold-timeout 10] [--samples 4096] [--method auto] [--json] <pid>
smash-estimate --window 21 --points 7 <pid>      # also models refault backoff (recommended)
smash-estimate [options] [--warmup 5] [--kill] -- <command> [args...]
```

Built by default on Linux (`SMASH_BUILD_ESTIMATE`, `tools/smash-estimate/`),
installed to `bin/`. Steps: list anonymous private writable mappings
(`/proc/pid/maps`), record present pages (`pagemap`), then split `--window`
into `--points` segments, resetting the tracker at the start of each, so
every page gets one access bit per segment (pages that appear mid-window
join at the next point, and a growing mapping's end is re-read). A page's
idle age at the END of the window is its run of trailing idle segments;
`idle_age` is that curve (bytes idle for >= s at the end). The projection
takes the pages idle for the age at which smash drops a page's frame and
compresses a sample of them with zstd-1 under smash's store rules (all-zero
page = free, blob > 75% of a page = stays uncompressed, 16-byte blob
granularity), scales the mean per-page saving by the idle page count, and
subtracts smash's own footprint (below). The 95% CI is from the sampling
variance (with finite-population correction); it covers neither
idle-detection error nor the footprint model.

**Which idle age.** smash works in 1 s ticks: a page that reaches the cold
timeout T is read-checked or snapshotted the next tick and its frame is
dropped one deferred-reclaim tick later, at T + 2 s. Once any page of a
bucket (arena, size class) refaults, smash doubles that bucket's cold wait,
and the backed-off pages are dropped at 2T + 1 s. The refaults are mostly
reads of compressed pages, which the tool cannot attribute, so when the
window has a point at or past 2T + 1 the projection uses that age
(`backoff_modeled`) and reports the T + 2 figure as
`projected_saving_no_backoff_bytes`, an upper bound for programs that never
re-read a compressed page. `--window 21 --points 7` (3 s segments) has both
ages for the default T = 10 (`--cold-timeout` sets T). The default window
(10 s, 3 points) ends at T: no backoff, and pages are counted at idle >= 10 s.

**smash's own footprint** (`smash_metadata_bytes`, `smash_runtime_bytes`,
`layout_bytes`), measured on cloudnew with `smash_rss_part_bytes`:

- metadata: 4.2 MiB fixed + 28 B per resident page of the slab classes
  (`heap`, `malloc-arena`), 14 B per page of `anon-mmap` (large
  allocations), 30 B per projected compressed page. The per-page terms are
  from bench_rss at 64 MiB - 1 GiB before and after compression; the fixed
  part is the residual on bench_sqlite, bench_kv_store and memcached
  (3.7-4.7 MiB: compressor worker contexts, thread caches);
- runtime: 4.4 MiB (libsmash's image, its fault-slot and compressor
  buffers, thread stacks: the `other` part's growth over the glibc run of
  bench_sqlite), + 1.4 MiB for libstdc++ and 0.25 MiB for libm when the
  program does not map them (libsmash pulls them in);
- layout: for `heap` and `malloc-arena` mappings the tool walks the glibc
  chunk chain (reads only, `process_vm_readv` in 1 MiB blocks), prices
  every in-use chunk with smash's size classes (`src/core/size_classes.h`:
  smallest request for the chunk size, rounded to the class, its share of
  the span) and charges `resident bytes x (factor - 1)`; idle pages
  compress the padding away. bench_sqlite: factor 1.068 (its ~4.4 KB pages
  take the 4608 class). A walk that breaks within the first half of a
  mapping (another allocator, or the chain changed mid-walk) leaves the
  factor at 1, with a note.

**Idle methods** (`--method auto` takes the first available):

| method | detects | granularity | needs |
|---|---|---|---|
| `idle` | reads + writes | page | root (PFNs) + `CONFIG_IDLE_PAGE_TRACKING` |
| `soft-dirty` | writes; reads via per-mapping cap | page | same uid; arch with soft-dirty (x86-64) |
| `referenced` | reads + writes | mapping | same uid |

- `soft-dirty` also clears referenced bits and caps each mapping's idle bytes
  by its unreferenced bytes (`smaps Referenced`), thinning that mapping's
  sample pool to match. A VMA that changes during a segment (a growing brk
  heap) gets `VM_SOFTDIRTY` and every page reads dirty; such mappings (more
  dirty than referenced bytes in a segment, impossible otherwise) fall back
  to the `referenced` method, noted in the output.
- Referenced bytes are per segment. For "idle for the last m segments" a
  mapping's unreferenced bytes are its size at the start of those segments
  minus the largest per-segment Referenced, each reduced by the growth since
  that start (pages faulted in later are referenced but were never idle
  candidates; without this a growing heap lost its growth from the idle
  count: ~65 MiB on bench_sqlite's serve phase). The max is a lower bound of
  the union of the segments' referenced sets (exact for a stable hot set).
- `referenced` cannot tell which pages are idle, only how many, so it samples
  compressibility from all present pages of each mapping.
- `idle` marks every tracked page's folio-head PFN idle at the start of
  each segment and reads the bitmap at its end. Two details matter (both fixed 2026-11-17 after the first runs
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
(at the start of every segment) makes the kernel write-protect / age the
target's PTEs (minor faults on its next write per page; pages look older to
reclaim); a target that relies on soft-dirty itself (CRIU, a smash run)
loses that window's write data. The idle method sets idle flags in the
global page_idle bitmap. The layout walk only reads.

**Classes and what smash would manage** (heuristic, from `/proc/pid/maps`):
`heap` (brk; glibc small allocations: full mode; large-only only for
allocations >= 1 MiB), `malloc-arena` (64 MiB-aligned glibc thread arenas:
full mode only), `anon-mmap` (malloc allocations >= 1 MiB in both modes, or
direct mmaps, which need `SMASH_TRACK_EXTERNAL=1`), `stack` (from each task's
`/proc/pid/task/*/syscall` stack pointer, else 8 MiB mappings above a guard
page; never managed). The projection assumes every idle page of a class is
managed — read the class notes.

**Not modeled**: how smash's layout changes which pages are hot. It
segregates call sites into arenas and size classes and keeps allocator
metadata out of data pages, so a read of one object touches fewer pages than
in a glibc heap where hot and cold objects (and chunk headers) share pages.
On bench_kv_store (hash-chain walks through an `std::unordered_map`) this
is most of the error: the glibc heap had ~168 MiB referenced per 12 s, smash
kept ~129 MiB resident (diagnostic runs), and the estimate is 14.8 pp low (below).
Also: refaults and their re-inflation, CompressStore fragmentation, the
ROI gate, large-only mode (the tool projects full mode), and allocators
other than glibc for the layout factor.

Tests: `test_smash_estimate` (auto method) and `test_smash_estimate_referenced`
(`tests/estimate_test.cpp` + `tests/estimate_target.cpp`): launch mode, a
target with 64 MiB compressible-idle, 16 MiB random-idle, 16 MiB write-hot,
16 MiB read-hot and 16 MiB read-only (zero page) regions, and the
compressible-idle VMA growing by 10 MiB/s of write-hot pages; asserts idle
in [72, 92] MiB, compression saving in [54, 72] MiB, and projected saving =
compression saving − metadata − runtime (no layout change: the regions are
mmaps). Unprivileged, so they run in CI.
`test_smash_estimate_idle` and `test_smash_estimate_idle_thp` force
`--method idle`, running smash-estimate via `sudo -n` when not root, and
exit 77 (skipped) without passwordless sudo or `/sys/kernel/mm/page_idle`;
they also assert `anon_present_bytes <= RSS + 4 MiB` (zero page excluded).
The `_thp` variant has the target `madvise(MADV_HUGEPAGE)` its regions (THP
must not be `never`) and requires the "large folios" note. Before the fixes:
`_idle` failed with "anon present 128.0 MiB > RSS 112.9 MiB", `_idle_thp`
with "idle 2.2 MiB not in [72, 92]"; after: both pass (dev VM, 2026-11-17).


## Where a smash process's RSS is: `smash_rss_part_bytes`

With `SMASH_METRICS_FILE=path SMASH_METRICS_RSS_PARTS=1` the metrics file
also has `smash_rss_part_bytes{part=...}`: smash's data pages by page state
(`data_live`, `data_shadow`, `data_pending_madvise` = compressed with the
deferred madvise still pending, `data_compressed_resident`, `data_empty` =
freed pages still holding a frame), the CompressStore's `store_header` and
`store_data` pages (with `smash_store_live_bytes`, fragmentation is
`store_data` minus live blobs), BootstrapAlloc `metadata`, and `other` = RSS
minus all of these (program image, libraries, stacks, smash's own buffers).
Residency comes from `/proc/self/pagemap` (present and exclusively mapped;
mincore on macOS): mincore also reports the shared zero page that reads of
never-written memory map, and smash's metadata then read as more than the
whole RSS. Off by default because it reads a pagemap entry per committed
page every interval. Observe mode's JSON always carries the same split
(`rss_parts`).

## Comparing estimates with a real run

```
python3 bench/validate_estimator.py --build-dir build-bench --runs 5 \
    [--baseline-build-dir build-base] [--workloads sqlite,memcached,kvstore] [--large-only]
python3 bench/validate_estimator.py --summarize out.json     # re-print a saved run
```

Needs `-DSMASH_BUILD_BENCH=ON` (and `memcached` on PATH for that workload;
`memtier_benchmark` if installed drives its GETs). Each workload loads data,
idles for `--cool` seconds and then serves a hot subset; every config is
measured at the same point of the serve phase (`--at`, default 25 s), in
random interleaved order:

| workload | load | serve |
|---|---|---|
| `sqlite` | bench_sqlite, 500K rows in an in-memory DB | hot-only queries; the heap grows ~7 MiB/s |
| `memcached` | 400K keys of 1000-byte JSON-like values over the text protocol (1 MiB slab allocations) | GETs of the first 5% of the keys |
| `kvstore` | bench_kv_store, `std::unordered_map` of 1M ~200-byte values | lookups of the first 5% of the keys |

Configs: `glibc` (RSS + `smash-estimate --window 21 --points 7` attached
21 s before the measurement, via `sudo -n`), `observe`, `smash` (full mode,
or `--large-only`), `smash_nb` (`SMASH_RECOMPRESS_BACKOFF=0`: the RSS
difference to `smash` is what refault backoff holds) and, with
`--baseline-build-dir`, `glibc_base` / `observe_base` from another build
(its estimate with `--baseline-est-args`, default the round-1
`--window 10 --points 1`). `--methods idle,referenced` runs one glibc
config per `smash-estimate --method` (reported per config). Runs under libsmash set
`SMASH_METRICS_RSS_PARTS=1`; the script prints, per workload, the measured
smash RSS by component next to the RSS each estimator implies for it:
`other` (program, libraries, stacks: the glibc run's non-heap RSS +
modeled runtime, or observe's measured `other`), `metadata`, and data pages
+ store (glibc heap − projected compression saving + layout, or observe's
resident data − reclaimed pages + blobs + holds). The differences add up to
the projection error in MiB.

### Dev measurements

Development numbers, not results: cloudnew (EPYC 9R14, kernel 6.12 x86-64,
40-core taskset slice), 2026-11-18, 5 randomized interleaved rounds each,
means (all standard deviations of the percentages <= 0.2 pp). Commands:

```
python3 bench/validate_estimator.py --build-dir build-bench --baseline-build-dir build-base \
    --runs 5 --seed 2 --workloads sqlite,memcached,kvstore --json-out tmp/final5.json
python3 bench/validate_estimator.py --build-dir build-bench --baseline-build-dir build-base \
    --runs 5 --seed 3 --workloads memcached --large-only --no-decompose --json-out tmp/final5-lo.json
```

(`build-base` = origin/master 4673d6c, the round-1 code; `smash-estimate`
method `soft-dirty`, sqlite's heap via the changed-VMA Referenced fallback;
observe `write_track=soft-dirty read_track=none`.) Reductions are
1 − smash RSS / glibc RSS for `smash-estimate` and 1 − smash RSS / observe
RSS for observe (observe runs smash's allocator, so its own RSS is the
baseline); "before" is the round-1 code on the same runs:

| workload | smash-estimate before | after | measured vs glibc | observe before | after | measured vs observe |
|---|---|---|---|---|---|---|
| sqlite | 50.8% (+6.9 pp) | 44.8% (+0.9) | 43.9% | 59.1% (+11.3) | 49.7% (+1.9) | 47.8% |
| memcached | 65.3% (+3.3) | 62.1% (+0.1) | 62.0% | 64.3% (+1.4) | 63.9% (+1.0) | 62.9% |
| kvstore | 46.9% (−11.5) | 43.5% (−14.8) | 58.3% | 73.7% (+13.3) | 63.2% (+2.9) | 60.4% |
| memcached, large-only | (full mode) | | 62.3% | 64.1% (+1.0) | 63.7% (+0.6) | 63.1% |

Re-measured after rebasing onto origin/master 9ed1c70 (THP / zero-page
handling in smash-estimate, `--methods`), same commands and seeds
(`--json-out tmp/rebased5.json` / `tmp/rebased5-lo.json`), cloudnew
2026-11-18 with a 16 GiB swapfile and zswap (zstd/zsmalloc) enabled by
another workstream (the workloads are not memory-capped; no notes about
zero pages or large folios appeared, so #133's paths did not trigger here):

| workload | smash-estimate before | after | measured vs glibc | observe before | after | measured vs observe |
|---|---|---|---|---|---|---|
| sqlite | 50.8% (+6.6 pp) | 45.4% (+1.2) | 44.2% | 59.0% (+10.7) | 49.6% (+1.3) | 48.3% |
| memcached | 65.2% (+3.3) | 62.0% (+0.1) | 61.9% | 64.3% (+1.4) | 63.9% (+1.0) | 62.9% |
| kvstore | 46.9% (−11.3) | 43.2% (−15.0) | 58.2% | 73.7% (+13.5) | 63.3% (+3.1) | 60.2% |
| memcached, large-only | (full mode) | | 62.4% | 64.1% (+1.0) | 63.7% (+0.6) | 63.1% |

The one change beyond noise: on sqlite smash-estimate saw 377.8 ± 0.3 MiB
idle (was 360.4 ± 0.1) and projected 0.6 pp more; the measured run and the
other rows moved by ≤ 0.6 pp. Not investigated further.

RSS (MiB, mean ± sd): sqlite glibc 524.8±1.0, observe 563.9±2.1, smash
294.5±1.4, smash with backoff off 240.4±1.3; memcached 464.0±0.0 /
475.9±0.1 / 176.5±0.1 / 176.8±0.1; kvstore 487.6±0.5 / 512.4±0.4 /
203.1±0.4 / 203.2±0.3.

**Where round 1's error came from** (sqlite, MiB, the decomposition above):
the real run's store had no fragmentation (97.9 MiB of resident store pages
for 97.9 MiB of live blobs) and its metadata was 10.1 MiB, in observe's RSS
too. The gap was pages the real run still held: refault backoff 54.1 ± 1.1
(smash vs smash with backoff off). ~790 decompression faults by then, most
of them reads (an instrumented run counted 662 read and 113 write faults),
the first ones right after the first cold wave; they set the bucket bias,
and every page the serve phase writes then waited 20 s instead of 10. For
`smash-estimate` two errors had cancelled: the growth bias undercounted
idle pages by ~68 MiB (diagnostic runs: 367 vs 436 MiB idle), and the
missing backoff, smash's metadata (10.4 modeled), runtime (4.4) and
allocator layout (10.8: factor 1.068) went the other way.

**Residuals after the fix** (smash RSS implied − measured, MiB):

| | other | metadata | data + store | total |
|---|---|---|---|---|
| sqlite, smash-estimate | −0.0 | +0.3 | −2.9 | −2.7 |
| sqlite, observe | +0.0 | +0.9 | −4.8 | −3.9 |
| memcached, smash-estimate | −0.5 | +0.1 | −0.1 | −0.5 |
| memcached, observe | +0.0 | +0.3 | −4.9 | −4.6 |
| kvstore, smash-estimate | +0.3 | −1.4 | +73.3 | +72.2 |
| kvstore, observe | +0.0 | +0.1 | −14.5 | −14.4 |

- **kvstore, smash-estimate (−14.8 pp)**: smash's layout makes fewer pages
  hot (see "Not modeled" above). In single diagnostic runs the glibc heap
  had ~168 MiB referenced per 12 s, while smash compressed 366 of 502 MiB
  and kept 129 MiB of data pages resident.
  The new footprint terms are real costs and move this estimate 3.3 pp
  further below; it errs low (conservative) on this kind of workload.
- **kvstore, observe (+2.9 pp)**: the read signal keeps 51.7 MiB of read-hot
  pages resident (`referenced_bytes` 115.6 MiB). The rest is store
  fragmentation the real run builds through ~11,700 refaults (56.2 MiB of
  store pages for 46.4 MiB of live blobs) and pages read less often than the
  2-tick sub-windows catch.
- **memcached, observe (+1.0 pp)**: ~1,100 refaults in the real run re-inflate
  pages observe counts as reclaimed; no backoff (the run with backoff off has
  the same RSS).
- The `metadata` constants of `smash-estimate` were fitted on these runs'
  smash metadata (see above), so its metadata row is in-sample.

**Idle method, dev VM** (measured with the code before the growth fix and
the end-of-window / footprint model above; not re-measured since, the VM
was not available to this work) (Lima `smash`: Ubuntu 24.04 arm64, kernel 6.8, 10
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
