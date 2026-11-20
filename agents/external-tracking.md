# External-Mapping Tracking (`SMASH_TRACK_EXTERNAL=1`)

Standard smash compresses pages of its own `malloc` arena. Memory the process maps itself — a bundled allocator's arenas (Valkey/Redis built with `MALLOC=jemalloc`, mimalloc, gperftools tcmalloc), CPython's obmalloc arenas, FFmpeg frame pools — never passes through `malloc` and so escapes the compressor. With `SMASH_TRACK_EXTERNAL=1` the `mmap` / `sbrk` / Mach VM interposers register such memory with the VmRegion's external-page table, and the compressor treats it like arena pages. This makes smash a compression layer *under* a bundled allocator with no allocator-specific hooks: those allocators get their memory from libc `mmap` / `__sbrk` through the PLT, which `LD_PRELOAD` interposes. **Opt-in, default off** — see "Default: what the evidence says" below.

Code: `src/external_track.h` (registration policy + hooks shared by both platforms), the interposers in `src/linux_syscall_wrappers.cpp` and `src/smash_heap.cpp` (macOS), `VmRegion::trackExternalPage` / `untrackExternalPage` (`src/vm/vm_region.h`), `CompressorThread::detachExternalRange` / `scanExternalWritten` (`src/compress/compressor_thread.h`). Regression test: `tests/external_patterns_test.cpp` (`test_external_patterns`, `test_external_patterns_large_only`).

## What gets registered

`ext::eligibleMapping`: private anonymous, protection exactly `PROT_READ|PROT_WRITE`, and none of `MAP_SHARED` (shared with forked children: the parent's blob goes stale when the child writes), `MAP_STACK` / `MAP_GROWSDOWN`, `MAP_HUGETLB` (can't be split into 4 KiB protections), `MAP_LOCKED`. Read-only and `PROT_EXEC` mappings are skipped (smash restores `PROT_RW` on decompress, which would strip exec from a JIT region). Sources:

- `mmap` / `mmap64` results; `MAP_FIXED` over tracked pages first settles them (below).
- `sbrk` / `__sbrk` growth (gperftools tcmalloc's `SbrkSysAllocator` calls `__sbrk`). glibc malloc's own break growth is an internal call and never reaches the wrapper.
- `mach_vm_allocate` / `vm_allocate` in our own task (macOS).
- A mapping larger than the whole slot budget is skipped with a one-time warning (a multi-GiB buffer pool is app-managed and kept warm; registering it wedged mariadbd startup before).

smash's own `mmap` / `madvise` / `mprotect` / `sigaltstack` go through raw syscalls (`vm::rawMmap`, `rawMadvise`, `rawMprotect`, `rawSigaltstack`), so the exported wrappers never see smash-internal memory (#84).

## Lifecycle: what the app may do to a tracked (possibly compressed) page

Every call that changes tracked memory first runs `CompressorThread::detachExternalRange` **before** the real syscall, under each page's lock (the serialization `model/SmashExternalRace.{lean,tla}` proves necessary; no syscall that tightens protection runs under the lock):

| App call | Mode | What smash does |
|---|---|---|
| `munmap`, `sbrk(<0)` | `kUnmap` | free the blob (it used to leak), untrack, recycle the slot |
| `madvise(MADV_DONTNEED / FREE / REMOVE / DONTNEED_LOCKED)` (macOS: also `MADV_FREE_REUSABLE`) | `kPurge` | free the blob, make a compressed page accessible, stay tracked. Without this a later access decompressed the **stale** blob over the purged page — jemalloc treats `DONTNEED`'d extents as zeroed ; on uffd-reclaimed pages `FREE` becomes `DONTNEED` and the purged PTEs are mapped to the zero page afterwards |
| `madvise(MADV_WIPEONFORK / DONTFORK / GUARD_INSTALL)`, macOS `minherit` (≠ `VM_INHERIT_COPY`), `ioctl(UFFDIO_REGISTER)`, liburing `io_uring_register_buffers*` | `kRestore` | restore, untrack, unregister from smash's uffd (round 2, see "Hazard guards") |
| `madvise(MADV_POPULATE_READ / WRITE)` | — | tracked pages in the range are decompressed first |
| `mprotect` | `kRestore` | decompress through the fault path, untrack, recycle; re-register afterwards only if the whole range was tracked and the new protection is plain RW. Covers app guard pages (a `PROT_NONE` guard used to be reset to RW by the compressor, compressed, and come back zero) |
| `mremap` | `kRestore` | restore + untrack the old range, register the result |
| `mmap(MAP_FIXED)` over tracked pages | `kRestore` | restore + untrack (restore, not discard: a failed `MAP_FIXED` leaves the old mapping) |
| `mlock`, `mlock2` | `kRestore` | locked memory must stay resident (often secrets kept out of swap); never tracked again. `mlockall` untracks everything; `MCL_FUTURE` stops all registration |
| `sigaltstack` | `kRestore` | signal handlers — smash's own included — run there |
| `pthread_sigmask` / `sigprocmask` | — | `SIGSEGV`/`SIGBUS` are stripped from blocked sets (all memory, not only external) |
| path-taking calls (`open*`, `rename*`, `unlink*`, `mkdir*`, `stat` family, `fopen`, …) | — | the path string is read once from user context first, so a path on a compressed page doesn't make the kernel return `EFAULT` (a Valkey `BGSAVE` child's `rename()` of its RDB file failed this way) |
| first read of a stdio stream | — | libc mallocs the buffer inside `fread`/`fgets`/`fgetc`; if that chunk is on a compressed page libc's internal `read()` gets `EFAULT`: the wrapper retries once after warming the new buffer (full-mode bug, independent of external tracking; `test_stdio_first_fill`) |

The signal-mask rule is not external-specific but was found here: a synchronous fault on a thread that blocks `SIGSEGV` is not delivered — the kernel resets the action and kills the process. jemalloc's background purge thread is created with every signal blocked and walks extent metadata; bundled-jemalloc Valkey died within seconds of its heap being compressed until the wrappers stripped those two signals.

## Coroutine stacks: alternate signal stack

A suspended coroutine's stack (e.g. a `makecontext` stack in a plain RW `mmap`; whether a fiber library passes `MAP_STACK` varies, and only `MAP_STACK` mappings are filtered) is cold, so it gets compressed. On resume the first touch faults with the stack pointer inside the inaccessible range; without an alternate stack the kernel cannot push the signal frame and kills the process. With external tracking on (override `SMASH_ALTSTACK=0/1`), smash gives every thread it sees in `threadInit` a 64 KiB `sigaltstack` (raw-mmap'd, guard page, never tracked, released in `threadCleanup`) and installs its handler with `SA_ONSTACK`. A thread whose app installs its own alternate stack keeps it (and it is untracked); smash's handler then runs on the app's stack, so a very small app alternate stack is a residual risk.

## Write tracking and untouched pages

- **uffd-wp mode** (Linux 6.7+ with async write-protect, the default on x86-64): each run of address-contiguous tracked pages is registered with smash's uffd on first sight (MISSING + WP since round 2, see "uffd reclaim" below) and `PAGEMAP_SCAN`ned each tick in two passes: written pages, re-protected (`PM_SCAN_WP_MATCHING`, present pages only: for a matched empty PTE the kernel installs a uffd-wp marker, which made a reclaimed page read as a swap entry and stopped it from re-compressing), then not-present or zero-page pages (no write-protect). A run that can't be registered counts as written.
- **Soft-dirty mode** (e.g. aarch64 6.8): pagemap bit 55, as before; runs are registered MISSING-only for uffd reclaim. A uffd page that is present but not exclusively mapped (pagemap bit 56: the zero page, or shared with a fork child) counts as written.
- **Untouched pages** of a big reservation (mimalloc maps 1 GiB `MAP_NORESERVE` arenas) are never aged into compression: pagemap present bit / `PAGE_IS_PFNZERO` (Linux) or one `mincore` per contiguous run (macOS).
- Pages on the classic path (macOS; Linux without UFFDIO_MOVE, `SMASH_UFFD=0`, `SMASH_EXT_UFFD=0`, or a range whose MISSING registration failed) still reclaim with `mprotect(PROT_NONE)` and split VMAs; the VMA guard reads the real `/proc/self/maps` count whenever external pages are live, and shadow reclaim checks the `PROT_NONE` result before decommitting.

## Slot budget and table

`kTrackMaxPages` = 4 GiB of **live** tracked memory (1M slots at 4 KiB, 256K at 16 KiB); every per-page table is demand-zero, so unused capacity is address space only. Untracked slots go on a free list and are reused (before, slots were never recycled: CPython-style 1 MiB arena churn exhausted the budget and tracking silently ended for the rest of the run). The open-addressing hash reuses tombstones and clears tombstone runs that end at an empty slot; mutations (track / untrack / recycle) are serialized by a leaf `track_lock_` (taken on the interposer paths only, also held across `fork`), lookups — fault handler, compressor — stay lock-free.

The hash places 64-page groups, and a group's pages take adjacent slots (round 2). Hashing each page independently scattered a mapping over the whole 32 MiB demand-zero table, one 4 KiB table page per tracked page: RocksDB (~4.7K external pages over its run) carried 21.5 MB more smash metadata with `TRACK_EXTERNAL=1` (33.9 vs 12.4 MB) and its paper reduction fell from 85.8 % to 80.5 %; grouped, 13.6 vs 12.1 MB (`SMASH_METRICS_RSS_PARTS=1`, cloudnew). Guarded by `test_vm_region` (4096 tracked pages must leave ≥ 32 tracked pages per resident table page).

`SMASH_STATS_SIGNAL=1` + `SIGUSR2` prints a second line: `[smash stats] external live=N cpages=M slots_hw=K uffd=U busy=B regfail=F unshare=S vmas=V` (live tracked pages, of which compressed, slot high-water; pages on uffd reclaim; registrations refused with EBUSY / failed otherwise; MOVE-refused pages unshared and retried; `/proc/self/maps` lines).

## Round 2: uffd reclaim, hazard guards, TE-forced CI

### uffd reclaim (no VMA splits)

Linux 6.8+ with smash's userfaultfd on (the default; `SMASH_UFFD=0` or `SMASH_EXT_UFFD=0` keep the classic path). `CompressorThread::registerExternalRun` / `zeroFillExternal` / `refillExternalRange` / `unprotectAfterFork` / `unshareForMove`; the per-page registration state is `ext_wp_` (`kExtWp`, `kExtMiss`, `kExtForeign`, `kExtClassic`), and `uffdPage(i)` is true for arena pages and `kExtMiss` external pages.

- **Registration.** A run of address-contiguous tracked pages, all ACTIVE, is registered MISSING (+ WP with async uffd-wp) on first sight, under the VmRegion track lock (no page of the run can be untracked and its index recycled between the check and the REGISTER). Then its never-touched pages are mapped to the zero page (`PAGEMAP_SCAN` finds the holes): an empty PTE in a registered range turns a kernel-mode access (`read()` into it) into `EFAULT`. From then on the pages reclaim with `UFFDIO_MOVE` and restore with `UFFDIO_COPY` exactly like the arena: no protection change, no VMA split.
- **Fallbacks.** `EBUSY` (another uffd, i.e. the app's own, owns the range) → never compressed. Any other failure → classic path. No WP-only legacy state is entered when MISSING registration succeeds.
- **Keeping registered ranges free of empty PTEs.** A discard `madvise` of such pages is followed by a zero-fill; `MADV_FREE` on them becomes `MADV_DONTNEED` (a lazily freed page the kernel reclaims later would leave an empty PTE at an unknowable time; DONTNEED is a valid implementation of FREE). Pages that leave tracking while still mapped (`kRestore`: `mprotect`, `mremap`, `MAP_FIXED`, `mlock`, `sigaltstack`, the guards below) are `UFFDIO_UNREGISTER`ed after the detach and before the app's syscall; their registration flags are read after the untrack and the slot is recycled only then. A MISSING fault on an untracked page inside a registered range (the short windows between untrack and unregister, or between a discard and its refill) is zero-filled; `UFFDIO_ZEROPAGE` fails with `ENOENT` on memory smash's uffd does not cover, so an app's own `SIGBUS` still reaches it. The kill switch unregisters every external range once inert.
- **fork.** The child has no uffd: `uffdPrepareFork` protects every MISSING external page `PROT_NONE` as it does for the arena. If that `mprotect` fails (VMA cap) the page is decompressed in place instead of reaching the child MISSING (`test_external_patterns forkcap`; with the fallback disabled the child read a wrong page, FAIL). A fork-protected page is restored by copying first (UFFDIO_COPY works into a `PROT_NONE` VMA) and unprotecting second (PR #138: unprotect-first left a window in which a concurrent `fork()` gave the child a zero page; arm64 VM, unmodified fork_stress, 11/20 → 20/20). After `fork()` returns the parent drops those protections again on its next tick (`unprotectAfterFork`): kept, they fragmented the parent — bundled-jemalloc Valkey went from 137 VMAs before BGSAVE to ~40K after it (5/5 runs, previous commit); now 134 (5/5). `forkvma` guards it (previous commit: 85 → 4199 VMAs, FAIL; now 88 → 4201 right after the fork → 88, PASS).
- **After fork, `PageAnonExclusive`.** fork clears it on every page resident at that moment and only a write fault sets it again, so `UFFDIO_MOVE` refused those pages (`EBUSY`) for good — even after the child exited (in Valkey/Redis: everything resident at a BGSAVE stayed unreclaimable until written). On `EBUSY`, if pagemap bit 56 says this process alone maps the page, smash issues `MADV_POPULATE_WRITE` on it (a write fault that changes no data and reuses the frame) and retries the MOVE once; a page a live child still maps keeps the round-1 behaviour (shadow discarded: unsharing would copy it). `test_move_refused` now also requires the 32 MiB to be reclaimed once the child exits: 5bab2bf 20 480 / 0 bytes after 90 s (FAIL), now all 32 MiB within 4 s (PASS), both modes (arm64 VM).

### Hazard guards

| Hazard | Guard | Detectable? | Test (fail on 5bab2bf → pass) |
|---|---|---|---|
| io_uring fixed buffers (pinned, `FOLL_LONGTERM`) | uffd mode: `UFFDIO_MOVE` refuses a maybe-DMA-pinned page (`EBUSY`), shadow discarded. liburing `io_uring_register_buffers{,_tags,_update_tag}` are interposed (versioned `LIBURING_2.x`): untrack (restore) first | pinned pages: only through MOVE's refusal (uffd mode). Registration: only through shared liburing (liburing makes the syscall itself, no libc wrapper) | `test_iouring_pin`: raw-registered buffer, READ_FIXED after cold: base app read the stale blob (FAIL) → PASS. `test_iouring_liburing`: registering a compressed buffer: base `-EFAULT` → PASS |
| `MAP_SHARED` | never registered (`eligibleMapping`) | yes | `shared` (round 1) |
| `mlock` / `mlock2` / `mlockall` / `MAP_LOCKED` | untracked / never registered | yes, through the interposers; a raw-syscall `mlock` is not seen | `mlock` (round 1), `maplocked` |
| the app's own userfaultfd | `ioctl(UFFDIO_REGISTER)` is interposed: untrack + unregister ours first. A range the app registered first makes smash's REGISTER fail `EBUSY` → never compressed (`kExtForeign`). smash's own ioctls use `SYS_ioctl` | yes, except an app REGISTER through `syscall(SYS_ioctl)` (then the app gets `EBUSY` if smash registered first) | `appuffd`: base: app REGISTER of a compressed range `EBUSY` (FAIL) → PASS; raw-registered range never compressed |
| `PROT_EXEC` / JIT | never registered (exactly `PROT_READ|PROT_WRITE`); `mprotect` to anything else untracks | yes | `exec`, `mprotect` (round 1) |
| `MADV_WIPEONFORK` | untracked + unregistered | yes | `wipeonfork`: base child read the parent's data (FAIL) → PASS |
| `MADV_DONTFORK`, `MADV_GUARD_INSTALL`, macOS `minherit(!= VM_INHERIT_COPY)` | untracked + unregistered | yes | `dontfork` (passes on base too: the child's not-present check already skips those pages; kept as a regression test), `guardinstall` (skips below 6.13) |
| `MADV_POPULATE_READ/WRITE` on compressed pages | tracked pages in the range are read (decompressed) first | yes | `populate`: base `EINVAL` (FAIL) → PASS |
| RDMA (`ibv_reg_mr`), VFIO DMA maps | uffd mode: MOVE refuses the pinned pages. libibverbs' fork protection uses `MADV_DONTFORK` (untracked). Registering an MR over a compressed page fails (`EFAULT`) | pins: only via MOVE. Not interposed, not tested (no hardware) | — |
| `process_vm_readv/writev` / ptrace from another process, Linux AIO / `O_DIRECT` on compressed pages | none: the other side sees `EFAULT` / a partial transfer (classic and uffd alike), not stale data | no | — |
| kernel-mode access in the registration window (between REGISTER and the zero-fill, microseconds per run) | interposed buffer syscalls retry on `EFAULT`; others would see `EFAULT` | — | — |
| classic path (macOS, older kernels) + a pinned page registered behind libsmash's back | none: reclaim drops the app's mapping while the device writes the old frame | no (pins are invisible from user space without uffd) | `test_iouring_pin` skips (77) in classic mode |

### TE-forced CI matrix

`-DSMASH_TEST_FORCE_TRACK_EXTERNAL=ON` appends `SMASH_TRACK_EXTERNAL=1` to every test's environment; `ci.yml` job `te-forced` runs it on ubuntu-latest, ubuntu-24.04-arm and macos-latest, then soak (60 s, kill switch toggling) both modes and fork_stress (300 forks × 6 threads, both modes ×2, Linux) with TE=1. soak_test and fork_stress_test now also hold a directly-mmap'd cold set (verified by every fork child) and churn mmap'd extents like a bundled allocator (fill / check / `MADV_DONTNEED` + expect zeros / `munmap`).

Local results, final tree (counts = ctest totals, 0 failed in every row):

| Platform | Normal | TE-forced |
|---|---|---|
| cloudnew x86-64, AL2023 6.12 (uffd + async WP) | 91/91 | 91/91 |
| arm64 Lima VM, Ubuntu 6.8 (uffd, no async WP; no liburing) | 89/89 | 89/89 |
| macOS arm64 laptop (classic path) | 72/72 (+1 disabled) | 72/72 (+1 disabled) |

fork_stress (`bench/fork_stress_loop.sh build-linux 0-1 20`, `taskset -c 0-1`, 150 forks × 6 threads): TE=1 20/20 full, 20/20 large-only; TE=0 20/20, 20/20. arm64 VM, 4 concurrent × 10 (`tmp/vm_fs_par.sh`), TE=0/1 × both modes: 160/160. soak 120 s with toggling, TE=1: PASS both modes (cloudnew); macOS 60 s: PASS both modes (5 more full-mode runs rc=0).

### Measured, round 2 (dev numbers)

cloudnew (AMD EPYC 9R14, AL2023 6.12, x86-64), `taskset` 96-core slice, other agents on other slices. Five randomized interleaved runs per row; medians, [min, max] for the reduction.

Bundled allocators, final tree: `python3 bench/bench_bundled_alloc.py --build-dir build-linux --valkey-dir tmp/deps/valkey/src --python python3.12 --mimalloc-inc … --mimalloc-lib … --tcmalloc-name tcmalloc --apps valkey,mi,tc,python --configs none,full:0,full:1,large:0,large:1 --runs 5 --cool 20 --mib 1024 --py-entries 3000000 --valkey-ops 3000000` (`SMASH_COLD_TIMEOUT_SEC=1`; workloads as in round 1). "Before" = 5bab2bf (`--alt-lib base=`, same command, an earlier session of the same day). VMAs = `/proc/<pid>/maps` lines, maximum over the cool phase; Valkey "after BGSAVE" is read right after the BGSAVE finished. Correct = 5/5 in every row (Valkey: `DEBUG DIGEST` after fill = after cool = after `BGSAVE` + `DEBUG RELOAD`).

| App | Config | Reduction % | Peak MB | Fill s | VMAs (cool max) | VMAs after BGSAVE |
|---|---|---|---|---|---|---|
| valkey | full, TE=0 | 0.0 | 713 | 44.69 | 101 | 101 |
| valkey | full, TE=1 | 84.1 [83.1, 84.4] | 734 | 46.87 | 134 | 134 |
| valkey | full, TE=1, before | 16.7 [16.5, 25.1] | 738 | 56.59 | 57 346 | 54 537 |
| valkey | large-only, TE=0 | 0.0 | 710 | 44.41 | 116 | 116 |
| valkey | large-only, TE=1 | 85.3 [82.8, 85.5] | 746 | 47.24 | 153 | 153 |
| valkey | large-only, TE=1, before | 24.1 [23.2, 25.1] | 754 | 56.47 | 57 339 | 51 272 |
| mi | full / large-only, TE=0 | 0.0 / 0.0 | 1149 / 1148 | 2.79 / 2.79 | 92 / 90 | |
| mi | full / large-only, TE=1 | 92.8 [92.8, 92.8] / 92.8 [92.8, 92.8] | 1184 / 1185 | 2.94 / 2.96 | 109 / 107 | |
| mi | full / large-only, TE=1, before | 92.4 [91.3, 92.5] / 81.8 [25.7, 92.5] | 1187 / 1189 | 2.95 / 2.95 | 57 287 / 57 313 | |
| tc | full / large-only, TE=0 | 0.0 / 0.0 | 1149 / 1149 | 2.85 / 2.84 | 93 / 93 | |
| tc | full / large-only, TE=1 | 93.8 [93.8, 93.8] / 93.8 [93.8, 93.8] | 1173 / 1173 | 3.19 / 3.17 | 109 / 109 | |
| tc | full / large-only, TE=1, before | 92.8 / 92.8 | 1185 / 1185 | 3.19 / 3.18 | 45 587 / 45 607 | |
| python 3.12 | full / large-only, TE=0 | 8.0 / 8.2 | 1149 / 1148 | 6.70 / 6.65 | 117 / 135 | |
| python 3.12 | full / large-only, TE=1 | 79.4 [79.4, 80.3] / 80.6 [80.5, 80.6] | 1187 / 1179 | 7.74 / 8.11 | 155 / 178 | |
| python 3.12 | full / large-only, TE=1, before | 79.3 / 79.5 | 1196 / 1187 | 8.01 / 8.04 | 9 568 / 8 884 | |

No-smash baselines: valkey 704 MB, fill 44.55 s; mi 1142 MB, 2.66 s; tc 1145 MB, 2.72 s; python 1142 MB, 6.38 s. Valkey BGSAVE took 5.6 s (full, TE=1) vs 3.4 s (TE=0) — the child decompresses what it serializes.

Paper workloads, full smash, TE=0 vs TE=1: `bench/paper_te_ab.sh build-paper tmp/paper2 5` (`run_paper_experiments.py --compress-only-only --configs full_smash`, default parameters, 5 rounds, setting order random per round; Redis/memcached/RocksDB 11.1.2 binaries reused from another workstream's deps build):

| App | Reduction % TE=0 | TE=1 | Peak MB TE=0 / TE=1 | ops/s TE=0 / TE=1 |
|---|---|---|---|---|
| SQLite | 70.5 [70.5, 70.6] | 70.5 [70.5, 70.6] | 406.1 / 406.1 | 89 753 / 90 589 |
| RocksDB | 85.8 [85.8, 85.8] | 85.5 [85.5, 85.5] | 314.0 / 314.5 | 630 362 / 640 918 |
| memcached | 89.7 [89.7, 89.8] | 89.7 [89.7, 89.7] | 294.8 / 294.8 | 212 066 / 211 712 |
| Redis | 58.5 [58.5, 58.6] | 58.6 [58.6, 58.7] | 283.0 / 283.5 | — |
| Redis-ext | 59.3 [59.0, 60.6] | 59.4 [58.7, 60.2] | 282.7 / 282.7 | — |

(Before the hash fix the same A/B gave RocksDB 85.8 vs 80.5 %.) RocksDB's remaining −0.3 points is +1.1 MB minimum RSS (smash metadata for its ~1.6K live external pages).

Malloc-only quick-CI benches (`python3 bench/ab_track_external.py --build-dir build-linux --runs 5`, medians): `bench_rss` 87 % in all four (mode × TE) cells; `bench_sqlite --quick` full 59.1 / 59.0 % (TE=0 / 1), 90 595 / 91 146 ops/s, hot p99 52.6 / 52.2 µs, child CPU 14.68 / 14.58 s; large-only 27.6 / 27.6 %, 99 354 / 99 364 ops/s, 47.4 / 47.5 µs, 11.28 / 11.28 s.

## Measured, round 1 (dev numbers)

All numbers below are development measurements: cloudnew (AMD EPYC 9R14, Amazon Linux 2023, kernel 6.12, x86-64) pinned to a 24-core `taskset` slice shared with other agents' jobs on other slices. Not for the README results table.

`python3 bench/bench_bundled_alloc.py --build-dir build-linux --alt-lib base=<libsmash.so of a2ff45b> --valkey-dir tmp/deps/valkey/src --python python3.12 --mimalloc-inc … --mimalloc-lib … --tcmalloc-name tcmalloc --apps <app> --configs none,full:0,full:1,large:0,large:1,full:1@base,large:1@base --runs 5 --cool 20 --mib 1024 --py-entries 3000000 --valkey-ops 3000000`, 5 randomized runs per row, `SMASH_COLD_TIMEOUT_SEC=1`. Library code at `ce1faa4` (the later fork-gate commit is not in these runs). "old lib" = `a2ff45b` (before this work). Reduction = 1 − min RSS during a 20 s idle phase / RSS at the end of the fill. Correct = every object re-read and compared (mi/tc/python), or `DEBUG DIGEST` after fill = after cool = after `BGSAVE` + `DEBUG RELOAD` with the reload confirmed in the server log (Valkey 9.1.0, `MALLOC=jemalloc` default build, `valkey-benchmark -t set -n 3000000 -r 3000000 -d 256`, ~1.9 M keys). mi = `mi_malloc` (mimalloc 2.1.7), tc = `tc_malloc` (gperftools 2.9.1, AL2023 package), python = CPython 3.12 dict of 3 M entries (one quarter deleted).

| App | Config | Correct | Peak RSS MB | Min RSS MB (cool) | Reduction % median [min, max] | Fill s |
|---|---|---|---|---|---|---|
| valkey | no smash | 5/5 | 703 | 703 | 0.0 [0.0, 0.0] | 44.39 |
| valkey | full, TE=0 | 5/5 | 711 | 711 | 0.0 [0.0, 0.0] | 44.34 |
| valkey | full, TE=1 | 4/5 | 742 | 579 | 22.3 [16.6, 24.9] | 53.93 |
| valkey | large-only, TE=0 | 5/5 | 709 | 709 | 0.0 [0.0, 0.0] | 44.39 |
| valkey | large-only, TE=1 | 4/5 | 741 | 577 | 23.1 [17.2, 25.6] | 54.21 |
| valkey | full, TE=1, old lib | 0/5 | 722 | 722 | 0.0 [0.0, 0.0] | 44.44 |
| valkey | large-only, TE=1, old lib | 5/5 | 720 | 719 | 0.0 [0.0, 0.0] | 44.87 |
| mi | no smash | 5/5 | 1142 | 1142 | 0.0 [0.0, 0.0] | 2.67 |
| mi | full, TE=0 | 5/5 | 1148 | 1148 | 0.0 [0.0, 0.0] | 2.80 |
| mi | full, TE=1 | 5/5 | 1187 | 94 | 92.1 [79.2, 92.5] | 2.96 |
| mi | large-only, TE=0 | 5/5 | 1148 | 1148 | 0.0 [0.0, 0.0] | 2.83 |
| mi | large-only, TE=1 | 5/5 | 1189 | 94 | 92.1 [34.6, 92.5] | 2.96 |
| mi | full, TE=1, old lib | 5/5 | 1158 | 1158 | 0.0 [0.0, 0.0] | 2.81 |
| mi | large-only, TE=1, old lib | 5/5 | 1158 | 1158 | 0.0 [0.0, 0.0] | 2.80 |
| tc | no smash | 5/5 | 1144 | 1144 | 0.0 [0.0, 0.0] | 2.76 |
| tc | full, TE=0 | 5/5 | 1149 | 1149 | 0.0 [0.0, 0.0] | 2.84 |
| tc | full, TE=1 | 5/5 | 1185 | 85 | 92.8 [92.8, 92.8] | 3.18 |
| tc | large-only, TE=0 | 5/5 | 1148 | 1148 | 0.0 [0.0, 0.0] | 2.90 |
| tc | large-only, TE=1 | 5/5 | 1185 | 85 | 92.8 [92.8, 92.9] | 3.22 |
| tc | full, TE=1, old lib | 5/5 | 1154 | 1154 | 0.0 [0.0, 0.0] | 3.12 |
| tc | large-only, TE=1, old lib | 5/5 | 1154 | 1154 | 0.0 [0.0, 0.0] | 3.17 |
| python | no smash | 5/5 | 1142 | 1142 | 0.0 [0.0, 0.0] | 6.41 |
| python | full, TE=0 | 5/5 | 1149 | 1057 | 8.0 [8.0, 8.0] | 6.61 |
| python | full, TE=1 | 5/5 | 1188 | 246 | 79.3 [79.2, 79.4] | 7.90 |
| python | large-only, TE=0 | 5/5 | 1148 | 1054 | 8.2 [8.2, 8.2] | 6.64 |
| python | large-only, TE=1 | 5/5 | 1194 | 243 | 79.6 [79.5, 79.6] | 7.71 |
| python | full, TE=1, old lib | 5/5 | 1159 | 1067 | 7.9 [7.9, 8.0] | 6.59 |
| python | large-only, TE=1, old lib | 5/5 | 1159 | 1064 | 8.2 [8.1, 8.2] | 6.71 |

- With the old library and `TRACK_EXTERNAL=1` nothing under a bundled allocator was compressed on this x86-64 6.12 host (async uffd-wp marked external pages "written" every tick), and a 1 GiB mimalloc arena exceeded the old 512 MiB slot budget anyway. CPython's 8 % at TE=0 is its >512 B objects, which go through `malloc`.
- Valkey reaches only ~22 % because compression stops at the VMA guard: external pages use the classic `mprotect` path, and re-reads after the fill (`DEBUG DIGEST`) restore scattered pages, each splitting a `PROT_NONE` run. After the idle phase the process sits at ~57 300 VMAs (cap 65 530, guard margin 8 192). In one run without the post-fill digest the same fill ended with 438 VMAs, consistent with the fragmentation coming from restores rather than from the fill itself (one run; not a controlled comparison).
- Correctness failures in the new-library Valkey rows (1/5 full, 1/5 large-only; 1 more in 20 extra runs) failed the reload check; the one whose server log was kept was a `BGSAVE` child that never finished, and a loop of fill/cool/`BGSAVE` trials caught two such children in gdb spinning in `PageLockTable::lock` on a page lock a parent thread held at the instant of `fork` (the two bench failures without logs are attributed to the same cause by symptom only). Fixed afterwards (see "fork" below). After the fork gate (library at `a6289c9`, same command, `--configs full:1` / `large:1`, 5 runs each): **10/10 correct**, reduction median 21.1 % (full) / 25.0 % (large-only).
- `full, TE=1, old lib` fails every Valkey run: `DEBUG RELOAD` hits "Short read or OOM loading DB" — the stdio first-fill EFAULT (`test_stdio_first_fill`), a full-mode bug independent of external tracking (it also fails with `TRACK_EXTERNAL=0` on the old library).
- Costs with TE=1 vs TE=0 (same mode): fill time Valkey 44.3 → 53.9 s (+22 %), CPython 6.61 → 7.90 s (+20 %), tc 2.84 → 3.18 s (+12 %), mi 2.80 → 2.96 s (+6 %); peak RSS +3–4 % (compressed store and metadata in flight). All with the 1 s cold timeout, which compresses during the fill; the default 10 s does less of that.

Micro-costs (`tmp`-built `mmap_churn.c`, 5 runs, medians; cloudnew, dev): 1 MiB mmap + touch + munmap cycle 410 µs without smash → 430 µs with TE=1 (TE=0: 410 µs); registering a 1 GiB `MAP_NORESERVE` reservation 33 ms and unmapping it 25 ms (TE=0: <0.01 ms); a touch + `madvise(DONTNEED)` of one tracked page 4.2 µs vs 4.1 µs.

fork: `pauseForFork` now waits for in-flight external detaches (and blocks new ones until the parent/child handlers run), and the child releases page locks whose owner thread did not survive the fork.


## Default: what the evidence says

Decision (this branch): **not flipped** — `SMASH_TRACK_EXTERNAL` stays opt-in; the maintainer decides from the round-2 evidence above.

Resolved since round 1: the VMA budget (uffd reclaim, measured above: Valkey 57K → 134–153 VMAs, reduction 17–24 % → 84–85 %; no VMA growth across BGSAVE), the fill-phase cost on Valkey (+5–6 % vs TE=0, was +22 %), io_uring fixed buffers and other pins in uffd mode (MOVE refuses them), apps using userfaultfd themselves, WIPEONFORK/DONTFORK/guard pages/populate, and a TE-only metadata leak that cost RocksDB 5 points. Malloc-only workloads (the paper suite and the quick-CI benches) do not change with TE=1.

Still open (what a default would accept):
1. **Classic path platforms.** macOS, and Linux without UFFDIO_MOVE (< 6.8, or uffd unavailable: seccomp, `vm.unprivileged_userfaultfd=0` without `UFFD_USER_MODE_ONLY`), still reclaim external pages with `PROT_NONE` (VMA splits as in round 1) and cannot see a pinned page registered behind libsmash's back (raw `io_uring_register`, RDMA, VFIO). A default could be TE=1 only where uffd reclaim is active.
2. **Fill-phase CPU.** Measured fill-time cost with TE=1 (1 s cold timeout, which compresses during the fill): CPython +16–22 %, tcmalloc +12 %, mimalloc +5–6 %, Valkey +5–6 %; peak RSS +2–5 %. The default 10 s cold timeout does less of that (not measured here).
3. **Kernel-mode access to compressed external pages from paths smash does not interpose**: `process_vm_readv`/ptrace from another process, Linux AIO / `O_DIRECT`, io_uring with ordinary (non-fixed) buffers completing into a compressed page, and the microsecond window between a run's REGISTER and its zero-fill. These return `EFAULT`/short transfers rather than wrong data; the same holds for smash's `malloc` memory today, but TE widens it to every private anonymous mapping.
4. **Not validated**: Firefox-class workloads (the round-1 single-trial stability question is not re-measured), GC runtimes (out of scope), apps that call `syscall(SYS_ioctl, …, UFFDIO_REGISTER)` directly, MariaDB-style buffer pools bigger than the 4 GiB slot budget (skipped by design).

## Out of scope

- **GC runtimes** (JVM, Go, V8/Node): their heaps are managed by a moving/marking collector that touches every live object on each cycle (so little stays cold), they install their own `SIGSEGV` handlers for implicit null checks and safepoints, and Go makes raw `mmap` syscalls that no interposer sees. Compressing under a collector needs collector cooperation, not page tracking.
- **`MAP_FIXED` / static pools that bypass interposition**: MariaDB's InnoDB buffer pool is a `MAP_FIXED` mapping created where `LD_PRELOAD` doesn't reach it (an earlier MariaDB study traced it with strace), and a pool larger than the slot budget is skipped by design.
- **Memory pinned for device or kernel DMA** (io_uring fixed buffers, RDMA `ibv_reg_mr`, VFIO): hypothesis, not tested — the kernel keeps using the pinned frame after smash decommits the mapping, so writes by the device would not reach the decompressed page. This applies to smash-managed `malloc` memory as well.
- **`SMASH_MODE=compress_only`** (README: "for programs linked against jemalloc or tcmalloc") is a separate mechanism (finds pages via `/proc/self/maps`) and was not evaluated here, beyond one observation: under it, `external_patterns_test ops` saw none of its 2 MiB anonymous regions compressed within 30 s (cloudnew, old and new library).
