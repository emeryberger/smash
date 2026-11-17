# Deployment safety: kill switch, watchdog, SLO controller

Code: `src/compress/safety.h` (mailbox, signal parsing, latency histogram,
`HealthMonitor` = watchdog + controller logic), the "Fail-open kill switch"
block in `src/compress/compressor_thread.h` (`killSwitchTick`, `makeInert`,
`sweepProtectedActive`, `healthTick`), the API in `src/smash_heap.h`
(`killSwitchDisable/Enable`) and `src/smash_heap.cpp` (`smash_disable`,
`smash_enable`, `smash_safety_status`; declared in `include/smash/smash.h`).

## Kill switch

**Inert state.** Compression off: no Phase 1/2/3, no Phase B shadow reclaim, no
deferred-madvise sweep, no uffd-wp `PAGEMAP_SCAN`. Every page is plain memory:
no COMPRESSED / COMPRESSED_SHADOW / ACTIVE_MONITORING page, no `PROT_NONE` /
`PROT_READ` page in the arena, uffd write-protect cleared. The allocator keeps
running (it owns the live objects) and smash induces no further faults. The
fault handler stays installed (it is a no-op unless a page is protected). The
store trim keeps running so the drained blobs' memory goes back to the OS.

**Triggers** (all post to the same mailbox, `safety::g_request`, last writer wins):

| Trigger | Disable | Enable |
|---|---|---|
| C API | `smash_disable()` (blocks until inert, ≤ 30 s; 0 = inert, 1 = still draining, -1 = no compressor) | `smash_enable()` |
| Signal (opt-in) | `SMASH_DISABLE_SIGNAL=SIGUSR1` (number or name; `SIGRTMIN+n` on Linux) | `SMASH_ENABLE_SIGNAL=...` |
| Control file (opt-in) | `SMASH_CONTROL_FILE=path` containing `disable` / `off` / `0` | `enable` / `on` / `1` |
| Watchdog | automatic, see below | — |
| Startup | `SMASH_START_DISABLED=1` (start inert; enable later by any trigger) | — |

No signal is taken by default: claiming one would break hosts that use it
(same reasoning as `SMASH_STATS_SIGNAL`, which gates SIGUSR2 for stats).
`parseSignal` refuses SEGV/BUS/ILL/FPE/ABRT/KILL/STOP, the stats signal when it
is enabled, and the same signal for both directions. The handlers are
re-claimed each tick like the stats handler. The control file is polled once a
tick and is *edge-triggered* on its first word: a file left saying `enable`
cannot undo a watchdog trip; rewrite it to re-assert.

**Mechanics and race-freedom.**
- Requests are applied only by the coordinator, at the top of `tick()`
  (`killSwitchTick`), i.e. between phases, under the normal `in_tick_` /
  `paused_` fork handshake. The 10 ms sleep loop wakes early when a request is
  pending, and `forEachLivePage` checks `tickAbortRequested()` (fork pause or a
  pending disable) every 16 chunks, so a long Phase 2 does not delay a disable
  or a fork.
- `makeInert()` walks every committed page and handles one page at a time under
  that page's lock (`tryLock`; a busy page is retried next tick): COMPRESSED →
  decompress in place with `restorePageContents` (populate-before-readable, the
  verified `procmem` restore) or `UFFDIO_COPY`, then release the blob;
  COMPRESSED_SHADOW → the page is still mapped and authoritative (it may have
  been written after the blob was made), so the blob is dropped, never
  restored; ACTIVE_MONITORING → `makeResidentPageWritable`. This is the fault
  handler's own single-page pattern, so it composes with concurrent faults the
  same way the fault handler does with itself (per-page lock, CAS-free state
  stores under the lock). No multi-page lock is ever held across an mprotect
  (the TLB-shootdown circular wait described at `flushPhase2PendingProt`).
- Then uffd write-protect is cleared over the registered arena, and
  `sweepProtectedActive()` reads the kernel's view (`/proc/self/maps`, or
  `mach_vm_region` on macOS) for non-RW ranges in the arena and makes any
  ACTIVE page found there writable. That repairs the known Phase 3 race (a
  batched `mprotect(PROT_READ)` landing on a page a fault just restored), which
  would otherwise cost one more fault after the drain. EMPTY pages are left to
  the decommit thread, which restores `PROT_RW` on freed ranges before reuse.
- The drain repeats each tick until nothing is left; then `inert` is set and
  logged once. `smash_disable()` returns 0 only when disabled ∧ inert ∧ no
  request pending.
- Re-enable resets every page's cold streak (so it is not an instant
  whole-heap compression) and re-bases the watchdog.
- Fork: `uffdPrepareFork` only protects COMPRESSED pages; an inert process has
  none.

Transitions write one line each to stderr (`[smash] ... kill switch: ...`).

## Watchdog (fail-open)

Evaluated once per tick (`healthTick`), only while compression is on; a trip
logs one line with the reason and disables compression as above.

| Variable | Default | Trips when |
|---|---|---|
| `SMASH_WATCHDOG_FAULT_RATE=N` | off | decompress faults/s, averaged over the last `SMASH_WATCHDOG_WINDOW` ticks, exceed N |
| `SMASH_WATCHDOG_P99_US=N` | off | the fault-service p99 exceeds N µs on `WINDOW` consecutive ticks that had ≥ `SMASH_WATCHDOG_MIN_SAMPLES` (20) faults; ticks with fewer faults are neutral |
| `SMASH_WATCHDOG_DECOMP_FAIL=N` | **1** | N decompressions returned a short page (data loss). `0` disables |
| `SMASH_WATCHDOG_WINDOW=N` | 5 | window, ticks (1..120) |

The rate limit uses a window average, not consecutive ticks, because refaults
are bursty: a page recompresses one tick and refaults the next, so a per-tick
streak resets on every quiet tick (observed in an early `test_watchdog_rate` run on a
macOS laptop: ticks alternated between 0 and ~900 faults/s and a
consecutive-tick streak never tripped).

Latency: `handleFault` is timed with `clock_gettime(CLOCK_MONOTONIC)`
(async-signal-safe) only when `SMASH_WATCHDOG_P99_US` is set, into a
log-linear histogram (4 sub-buckets per power of two µs). The reported p99 is
the upper bound of the bucket holding the 99th-percentile sample, so it
over-states by at most 25% and the watchdog errs toward tripping early. The
reason line prints it as `fault_p99_us<=N`.

Decompression failures are counted on the fault path, the prefetch path,
`restoreRange`, and the kill-switch drain. Before this change the return value
was ignored everywhere.

## SLO-aware controller

`SMASH_MAX_FAULT_RATE=N` (refaults/s budget). Each tick: if the tick's
decompress-fault rate is > N, the multiplier doubles; if it is < N/2 it decays
by 1/8; in between it holds. Range 1× .. 255×, x256 fixed point. It scales the
cold threshold in `effectiveColdFloor()`, after `SMASH_ADAPTIVE_COLD`'s
resident-size scaling, and the result is clamped to 255 (the 8-bit cold
counter). The ROI model (`compression_roi.h`) is not modified: Phase 2 gates on
the scaled floor before `shouldCompress`, and ROI already grows with cold
count, so a higher floor means fewer, colder, higher-ROI candidates. The
controller never trips; combine it with the watchdog for a hard backstop.

## Observability

`smash_safety_status()` (struct `SmashSafetyStatus`): disabled, inert,
compressed / reclaimed pages, faults served, decompress faults, decompress
failures, watchdog trips, effective cold floor, controller multiplier, last
tick's fault rate and p99, last disable reason. The SIGUSR2 stats line
(`SMASH_STATS_SIGNAL=1`, `SMASH_STATS=1`, `SMASH_DEBUG=1`) gained
`cold_floor= slo_x256= fault_rate= fault_p99_us= faults= decomp_fail=
disabled= inert= trips=`.

## Tests

| ctest | What it proves |
|---|---|
| `test_safety` | `safety.h` logic: signal parsing/refusals, histogram bucket bounds and p99, controller doubling/hold/decay/cap, window-averaged rate trip (incl. bursty input), p99 streak with neutral ticks, decompress-failure trip and re-base, mailbox |
| `test_kill_switch_{api,signal,file}[_large_only]` | waits for ≥ 1024 *reclaimed* pages, triggers the disable without touching the data, then: 0 compressed pages, every page of every test chunk is RW in the kernel's map, data byte-exact, a full read+write pass plus 3 ticks cause 0 smash faults and 0 recompressions; re-enable → pages reclaimed again, read-back byte-exact and takes decompress faults |
| `test_watchdog_rate[_large_only]`, `test_watchdog_p99[_large_only]` | synthetic refault storm (16 MiB read every 150 ms, `SMASH_RECOMPRESS_BACKOFF=0`) trips the watchdog with the right reason; process then inert, data exact, no faults |
| `test_slo_controller[_large_only]` | same storm with `SMASH_MAX_FAULT_RATE`: the effective cold floor rises (≥ 2× base) without a trip, then the multiplier decays once the storm stops |
| `test_soak[_large_only]` | 8 threads alloc/free/realloc (16 B – 3 MiB) with random idle gaps, random reads of a reclaimed cold set (concurrent decompress faults), while a thread flips `smash_disable()`/`smash_enable()` (enabled 3.5–7 s, so pages are reclaimed before each drain; disabled 0.3–1.5 s); every object is pattern-checked; `smash_disable()==0` must imply 0 compressed pages. CI's stress job runs it for minutes |
| `test_fork_stress[_large_only]` (Linux) | issue #61 regression: threads churn + fault while the main thread forks 120 times; each child allocates across size classes and reads the whole reclaimed cold set; a child that does not finish in 20 s fails the test |

Soak runs on cloudnew (x86-64, 24-core taskset, development check, not a
benchmark), `LD_PRELOAD=build-linux/libsmash.so SMASH_DEFER_PHASES_MS=0
SMASH_COLD_TIMEOUT_SEC=1 SMASH_SOAK_TOGGLE={0,1} SMASH_LARGE_ONLY={0,1}
build-linux/tests/soak_test 120 8`: all four PASS. With toggling, 19 disables
per run drained 525,486 (full) / 325,954 (large-only) compressed pages in
total while the workers took 211,378 / 178,784 decompress faults.

## Fork safety (issue #61)

PR #92 made `SmashHeap::lock()/unlock()` (alloc8's `xxmalloc_lock`) take every
allocation-path lock. Checking the issue against current code found three
remaining gaps, fixed here:

- **Child-handler self-deadlock (found by `test_fork_stress`).** On Linux,
  pthread_atfork child handlers run in registration order. smash's handlers are
  registered at constructor priority 202; alloc8's (which call
  `xxmalloc_unlock`) at the default priority, i.e. later. So
  `smash_atfork_child` runs while the forking thread still holds every lock
  `xxmalloc_lock` took. `VmRegion::resetDecommitThreadForFork()` drained pending
  decommit entries synchronously, and `processDecommitEntry` takes a free-shard
  lock: self-deadlock whenever a decommit was queued at fork(). Measured on
  cloudnew (x86-64, 24-core taskset), full mode, `fork_stress_test 150 6`:
  before, 1–2 hung children per run in 3 of 4 runs; backtrace
  `smash_atfork_child → resetDecommitThreadForFork → processDecommitEntry →
  Spinlock::lockSlow`. After (pending entries stay queued for the restarted
  decommit thread): 8 of 8 runs (5 full, 3 large-only; 1200 forks) with no
  hang. Command: `LD_PRELOAD=build-linux/libsmash.so SMASH_LARGE_ONLY={0,1}
  SMASH_DEFER_PHASES_MS=0 SMASH_COLD_TIMEOUT_SEC=1
  build-linux/tests/fork_stress_test 150 6`.
- **CompressStore shard locks were not quiesced** (found by reading the code;
  no failure observed). The fault path takes one (`releaseBlob`), as does a
  worker storing a blob; a fork inside either would leave the child's shard
  locked, and the child's first fault on a compressed page in that shard would
  spin. `CompressStore::lockForFork()` is now taken in `SmashHeap::lock()`
  (a leaf: just before BootstrapAlloc).
- **A long tick could outlive the 50 ms prepare wait** and fork while holding
  page / store locks. Phases now poll `paused_` (via `tickAbortRequested`)
  every 16 chunks and return, so a tick ends promptly when a fork is pending.

Still open: an application thread *inside the fault handler* holds its page
lock for one decompression; a fork at that instant leaves that page locked in
the child. Quiescing it needs the fault handler to take part in the atfork
protocol (an in-fault counter the prepare handler waits on). Not observed in
the runs above.

`test_fork_stress` is a stress test, not a deterministic discriminator. (Its
first version had a test bug, a 64-byte memset into allocations as small as 8
bytes, which glibc reported as `malloc(): corrupted top size` even without
smash preloaded; fixed before the measurements above.)

## ThreadSanitizer findings (not fixed)

A `-fsanitize=thread` build (clang 15, cloudnew) of the in-process unit tests:
13 tests report no race and gate CI. `test_contention` (7 reports) and
`test_stop_no_drain` (2 reports) report races; CI runs them report-only.
Reported pairs: `stableCallsiteHash` vs `stableCallsiteHashSlow` (unsynchronized
call-site hash cache); `releaseCompressedPages` vs `phase1Range` (plain
per-page `uint8_t` arrays such as `cold_count_`, racy by design);
`adaptWorkerCount` vs `helperEntry` (`active_workers_` is a plain `int`);
`armShadowBudget` vs `operator delete`; and `SmashHeap()` / `start()` writes vs
first `tick()` reads, which pthread_create should order. That last group may be
a TSan artifact of alloc8 interposing `pthread_create`; not verified. Each of
these is a C++ data race (undefined behaviour) even where the algorithm
tolerates stale values; converting them to relaxed atomics is the follow-up.

## Not covered / not modeled

- The kill switch is not in the TLA+/Lean models (`model/`); its per-page step
  reuses transitions the models do cover (COMPRESSED → ACTIVE restore,
  ACTIVE_MONITORING → ACTIVE), but the drain loop and mailbox are not modeled.
- Pages smash compressed in a *fork child* of a disabled parent: the child
  inherits the disabled flag and stays inert (the coordinator is respawned
  lazily with the same state).
- External pages (`SMASH_TRACK_EXTERNAL=1`) are drained by state; the kernel
  protection sweep covers only the contiguous arena.
