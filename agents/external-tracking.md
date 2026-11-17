# External-Mapping Tracking (`SMASH_TRACK_EXTERNAL=1`)

Standard smash compresses pages of its own `malloc` arena. Memory the process maps itself — a bundled allocator's arenas (Valkey/Redis built with `MALLOC=jemalloc`, mimalloc, gperftools tcmalloc), CPython's obmalloc arenas, FFmpeg frame pools — never passes through `malloc` and so escapes the compressor. With `SMASH_TRACK_EXTERNAL=1` the `mmap` / `sbrk` / Mach VM interposers register such memory with the VmRegion's external-page table, and the compressor treats it like arena pages. This makes smash a compression layer *under* a bundled allocator with no allocator-specific hooks: those allocators get their memory from libc `mmap` / `__sbrk` through the PLT, which `LD_PRELOAD` interposes. **Opt-in, default off** — see "Why still opt-in" below.

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
| `madvise(MADV_DONTNEED / FREE / REMOVE / DONTNEED_LOCKED)` (macOS: also `MADV_FREE_REUSABLE`) | `kPurge` | free the blob, make a compressed page accessible, stay tracked. Without this a later access decompressed the **stale** blob over the purged page — jemalloc treats `DONTNEED`'d extents as zeroed |
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

- **uffd-wp mode** (Linux 6.7+ with async write-protect, the default on x86-64): external pages used to be marked "written" every tick — i.e. never compressed on those kernels. Now each run of address-contiguous tracked pages is registered for async write-protect on first sight (`UFFDIO_REGISTER_MODE_WP` only: no MISSING mode, so first touch remains an ordinary kernel zero-fill) and `PAGEMAP_SCAN`ned like the arena. A run that can't be registered counts as written. Registration does not survive fork (no `EVENT_FORK`; the child uses soft-dirty) or mremap (re-tracked by the wrapper).
- **Soft-dirty mode** (e.g. aarch64 6.8): pagemap bit 55, as before.
- **Untouched pages** of a big reservation (mimalloc maps 1 GiB `MAP_NORESERVE` arenas) are never aged into compression: pagemap present bit (Linux; `PAGEMAP_SCAN` with inverted `PAGE_IS_PRESENT`) or one `mincore` per contiguous run (macOS). "Compressing" them saved nothing and turned the app's first touch into a fault.
- External pages always use the classic `mprotect(PROT_NONE)` reclaim (not uffd MOVE), so they split VMAs. The VMA guard therefore reads the real `/proc/self/maps` count whenever external pages are live (uffd mode used to assume 1, which drove a 1 GiB mimalloc heap to `vm.max_map_count`; the failed `PROT_NONE` was ignored and readable pages were decommitted — zeroed objects). Shadow reclaim now checks the `PROT_NONE` result before decommitting.

## Slot budget and table

`kTrackMaxPages` = 4 GiB of **live** tracked memory (1M slots at 4 KiB, 256K at 16 KiB); every per-page table is demand-zero, so unused capacity is address space only. Untracked slots go on a free list and are reused (before, slots were never recycled: CPython-style 1 MiB arena churn exhausted the budget and tracking silently ended for the rest of the run). The open-addressing hash reuses tombstones and clears tombstone runs that end at an empty slot; mutations (track / untrack / recycle) are serialized by a leaf `track_lock_` (taken on the interposer paths only, also held across `fork`), lookups — fault handler, compressor — stay lock-free.

`SMASH_STATS_SIGNAL=1` + `SIGUSR2` prints a second line: `[smash stats] external live=N cpages=M slots_hw=K` (live tracked pages, of which compressed, slot high-water).

## Measured (dev numbers)

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


## Why still opt-in

Decision (this branch): **keep the default off.** What the evidence supports, and what blocks flipping it:

For: `test_external_patterns` (18 cases on Linux, 14 on macOS) and the rest of ctest pass in both modes on macOS arm64 and cloudnew x86-64; the four bundled-allocator workloads above are correct with `TRACK_EXTERNAL=1` after the fixes, and malloc-only workloads don't change (A/B below).

Blocking, measured:
1. **VMA budget.** External pages reclaim with `mprotect(PROT_NONE)` (the arena uses uffd MOVE, which never splits VMAs). Valkey with 1.9 M keys parks at the VMA guard (~57 K VMAs) at ~22 % instead of compressing its cold data, and near the cap correctness depends on the whole-VMA restore paths (`restoreWholeVmaExternal`, the fork-child case). A default must not push arbitrary programs toward `vm.max_map_count`. Next step: register external runs with userfaultfd MISSING as well and use the arena's MOVE/COPY reclaim for them (first touch of untouched pages then becomes a smash-resolved fault — needs measuring), or reclaim in aligned multi-page groups.
2. **Fill-phase cost.** +6 % to +22 % fill time on the four workloads (1 s cold timeout).

Blocking, not tested (hypotheses):
3. **Kernel access outside the interposed syscalls**: io_uring registered buffers, Linux AIO / `O_DIRECT` submission, RDMA / VFIO pinning, `process_vm_readv` from another process. A pinned or kernel-held reference to a page smash later decommits would read or write a stale frame. This exists for smash's `malloc` memory too, but external tracking widens it to every private anonymous mapping — including buffers apps map specifically for those interfaces.
4. **Apps that use userfaultfd themselves** on their own mappings: smash's async write-protect registration of external runs would make the app's `UFFDIO_REGISTER` fail (`EBUSY`).
5. **Not re-validated** on Firefox-class workloads (below) or GC runtimes; a very small app-installed `sigaltstack` would host smash's handler (residual stack-overflow risk).

Earlier note, still unresolved: single-trial Firefox 5-tab Wikipedia at 90 s (full smash + DEFER 30 s + all-procs) showed the registration path possibly regressing stability (33–35 s lifetime vs 61 s with tracking off), with variance too high to call it. The "compressed=266K pages = 4.2 GB" figure seen then was mostly virtual-address artifacts (SpiderMonkey/Skia reserve large `MAP_ANON` ranges that never fault in); never-present pages are now excluded from compression, which should remove that artifact (not re-measured on Firefox).

A/B, malloc-only workloads (`python3 bench/ab_track_external.py --build-dir <build> --runs 5`, medians, TE=0 vs TE=1, dev numbers):

| Bench (cloudnew, 24-core slice) | Mode | TE=0 | TE=1 |
|---|---|---|---|
| `bench_rss --wait 20` best RSS reduction | full / large-only | 87 % / 87 % | 87 % / 87 % |
| `bench_sqlite --quick` RSS reduction | full / large-only | 59.0 % / 27.5 % | 59.0 % / 27.6 % |
| `bench_sqlite --quick` ops/s | full / large-only | 91 278 / 98 935 | 91 819 / 99 076 |
| `bench_sqlite --quick` hot p99 µs | full / large-only | 52.95 / 47.66 | 52.27 / 47.78 |
| `bench_sqlite --quick` child CPU s | full / large-only | 14.57 / 11.28 | 14.56 / 11.28 |

macOS arm64 laptop (same script, `--sqlite-modes 0`): bench_rss 82 % / 83 % both settings; bench_sqlite full mode 60.1 % and 111 105 vs 111 418 ops/s. (`bench_sqlite --quick` in large-only mode hangs on macOS in an SQLite realloc loop with the *old* library too, independent of `TRACK_EXTERNAL`; not investigated here.)


## Out of scope

- **GC runtimes** (JVM, Go, V8/Node): their heaps are managed by a moving/marking collector that touches every live object on each cycle (so little stays cold), they install their own `SIGSEGV` handlers for implicit null checks and safepoints, and Go makes raw `mmap` syscalls that no interposer sees. Compressing under a collector needs collector cooperation, not page tracking.
- **`MAP_FIXED` / static pools that bypass interposition**: MariaDB's InnoDB buffer pool is a `MAP_FIXED` mapping created where `LD_PRELOAD` doesn't reach it (an earlier MariaDB study traced it with strace), and a pool larger than the slot budget is skipped by design.
- **Memory pinned for device or kernel DMA** (io_uring fixed buffers, RDMA `ibv_reg_mr`, VFIO): hypothesis, not tested — the kernel keeps using the pinned frame after smash decommits the mapping, so writes by the device would not reach the decompressed page. This applies to smash-managed `malloc` memory as well.
- **`SMASH_MODE=compress_only`** (README: "for programs linked against jemalloc or tcmalloc") is a separate mechanism (finds pages via `/proc/self/maps`) and was not evaluated here, beyond one observation: under it, `external_patterns_test ops` saw none of its 2 MiB anonymous regions compressed within 30 s (cloudnew, old and new library).
