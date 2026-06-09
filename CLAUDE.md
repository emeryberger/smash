# CLAUDE.md - Project Guide for Claude Code

## What is smash?

A compression-aware memory allocator that transparently compresses cold pages to reduce RSS. It interposes on malloc/free via alloc8 and uses signal-based fault handling to decompress on access.

## Build

```bash
# Requires alloc8 as sibling directory (../alloc8) or set -DALLOC8_DIR=...
mkdir build && cd build
cmake .. && make -j$(nproc)

# With benchmarks
cmake .. -DSMASH_BUILD_BENCH=ON && make -j$(nproc)
```

## Test

```bash
cd build && ctest --output-on-failure
```

All 18 tests must pass. CI (`.github/workflows/ci.yml`) runs the full suite on `ubuntu-latest` and `macos-latest` on every push and PR. Four of the tests are end-to-end under `DYLD_INSERT_LIBRARIES` / `LD_PRELOAD` with a live compressor:

- `test_external_mapping` exercises the `SMASH_TRACK_EXTERNAL=1` registration path. The ctest invocation sets the env var; without it the test would silently no-op (registration path gated).
- `test_malloc_compression` allocates compressible chunks via the standard `malloc` path, sleeps past `SMASH_COLD_TIMEOUT_SEC`, sends `SIGUSR2` to itself, captures stderr, parses `compressed=N` from the smash stats line, asserts `N > 0`, then reads every byte back to verify integrity through fault-decompress. Catches regressions in the malloc-side compression path that the interposer-only tests would miss. Runs in **full mode** (`SMASH_LARGE_ONLY=0`).
- `test_large_only_compression` is the large-only sibling of the above — the only test that exercises `SMASH_LARGE_ONLY=1` (the production-supported config). It allocates large chunks (≥ 1 MiB, so they clear `kLargeAllocVmThreshold` and enter the compressible VmRegion) interleaved with small chunks (≤ 16 KiB, which pass through to the system allocator), asserts the large ones compress (`compressed>0`), and verifies a byte-exact read-back of **both** classes — proving large-only compresses without corrupting the passthrough path.
- `test_compression_ratio` (in-process, direct-linked, no compressor thread) compresses a realistically-compressible page with LZ4 and zstd and asserts the achieved ratio against the paper's RQ2 figures (evaluation.tex: LZ4 4.7–12.3×, zstd-1 8.8–20.4×). Two-tier: a hard floor that fails the test (2.5× LZ4 / 4× zstd — catches "stored uncompressed" regressions) plus a WARN if it falls short of the paper's best-case number. Also asserts byte-exact roundtrip. Unlike `test_malloc_compression` (which only checks compression *happened*), this pins the codec *ratio*.

After ctest, CI also runs `bench/run_quick_ci.py` which drives `bench_rss` (in-process: 64 MiB compressible alloc → ≥30 % peak-RSS reduction at t=10 s) and `bench_sqlite --quick` under the preloaded libsmash (≥5 % cooling-phase RSS reduction). Local baselines are ~46 % and ~13 % respectively, so the thresholds are well below noise; a real regression in the compressor or the malloc-interposed path will trip them. Configured with `-DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=OFF -DSMASH_BUILD_BENCH_ALLOCATORS=OFF` so the CI build skips Redis/memcached/RocksDB/tcmalloc/jemalloc/hoard/mesh/etc. — only the smash-internal benches are needed.

### Verifying paper claims end-to-end (`bench/verify_paper_claims.py`)

`run_quick_ci.py` is a regression tripwire; `verify_paper_claims.py` is the explicit "do we still match the paper?" harness. It drives the real workloads (`bench_rss`, `bench_sqlite`, `bench_rocksdb_builtin`) in **both** full and large-only mode, computes serve/cool-phase RSS reduction (`1 − min_rss/peak_rss` from the `METRIC` lines), and checks each against the paper's per-app figure (sqlite 69 %, rocksdb 80 %) with the same two-tier scheme as the ratio test: a conservative hard floor fails the run, a shortfall vs the published number only WARNs (the paper's reference box is a 192-core EPYC). Prints a `✓ BEATS paper` line when measured ≥ the claim. Needs `-DSMASH_BUILD_BENCH=ON`. Run from the bench build dir: `python3 ../bench/verify_paper_claims.py --build-dir .` (optionally `--apps sqlite,rocksdb --modes full,large_only`).

## Project Structure

```
include/smash/          Public API headers (config.h, smash.h)
src/
  core/                 Allocator core: bootstrap_alloc, size_classes, span, page_map, slab, large_alloc, thread_cache
  vm/                   Virtual memory: platform_mem, vm_region, page_state, fault_handler
  compress/             Compression: compress_engine (LZ4/zstd/zstd+dict), compress_store, compressor_thread
  util/                 Utilities: bitops, spinlock, intrusive_list
  smash_heap.h/.cpp     Main allocator singleton + alloc8 integration
tests/                  Unit + integration tests
bench/                  Benchmarks (throughput, compression ratio, RSS, latency)
```

## Architecture

- **Header-only internals**: Most code lives in headers under `src/`. Only `smash_heap.cpp` is compiled.
- **BootstrapAlloc**: All internal metadata allocated from a bump allocator (never calls malloc). Critical for avoiding reentrancy in the fault handler.
- **Arena-based allocation**: 4 arenas (`kNumArenas`). `callsiteArena()` hashes `__builtin_return_address(1)` to route allocations from the same call site to the same arena. Pages within an arena contain similar data → better compression ratios. `Slab slabs_[kNumArenas * kNumClasses]` flat 2D array.
- **Zero-on-free (deferred)**: All zeroing is deferred to the compressor thread's `zeroFreeSlots()` — no zeroing occurs in the `free()` critical path. Uses non-temporal stores (`ntZeroMemory`) to avoid cache pollution. This makes partial pages compress well (zero runs → high compression ratios).
- **PageState machine**: EMPTY → ACTIVE → ACTIVE_MONITORING → COMPRESSING → COMPRESSED → ACTIVE. CAS transitions ensure safe coordination between compressor thread and fault handler.
- **CompressEngine**: Supports LZ4, zstd, and zstd+dictionary. Algorithm packed in top 2 bits of `CompressedPageInfo::comp_size`. All zstd contexts pre-allocated via `ZSTD_customMem` routing to BootstrapAlloc.
- **Algorithm tiering (single-shot, ROI-driven)**: ROI model picks fast tier (zstd-1 by default; LZ4 if `SMASH_USE_LZ4`) or deep tier (zstd-9) per page **at initial compression time**. There is no later upgrade path; once a page is compressed, the chosen blob stays as-is until the page is decompressed by access. `AlgoProfile` carries `algo + zstd_level` so the calibrated profile reflects what's actually run; calibration in `compression_roi.h::calibrate()` benchmarks the actual fast-tier algorithm.
- **Adaptive per-bucket cost**: `SizeClassStats` tracks observed compression microseconds per tier (EMA, fixed-point). After 8 samples per (bucket, tier), the ROI model substitutes observed cost for the calibrated estimate via `selectProfile(..., observed_costs_us)`.
- **Parallel compressor**: Coordinator thread + worker threads. Chunk bitmap (`live_chunks_[]`) skips EMPTY pages. Sharded `CompressStore` (8 shards) eliminates lock contention. Per-worker compression contexts (LZ4 state, ZSTD CCtx, scratch buffers, SizeClassStats).
- **Adaptive worker count via Little's Law**: each tick the compressor sets active workers to `N = ⌈λ/μ⌉` where `λ` is pages-eligible/tick and `μ` is per-worker pages-compressed/tick (both EMA-smoothed). Workers pre-allocated up to `kMaxCompressorWorkers`; helpers lazily `pthread_create`'d on first scale-up.
- **Per-fault-slot DCtx**: Each of 32 fault slots has its own `ZSTD_DCtx*`, fixing data race between concurrent decompressions from app threads and prefetch.
- **Per-origin sliding-window stats**: `SizeClassStats` keyed by `(arena, size_class, worker)` — `sc_stats[kNumArenas * kNumClasses]` per worker, indexed via `statsIndex(arena, sc)`. Each bucket holds a 64-entry ratio window (0–255) plus per-tier compression-time EMAs. Aggregating across arenas would wash out the homogeneity arena routing produces.
- **Signal handler path**: No malloc allowed. Decompression uses pre-allocated per-slot contexts only.

## Syscall & Buffered I/O Compatibility

Smash's mprotect-based monitoring (PROT_READ) and compression (PROT_NONE) can conflict with kernel syscalls that access userspace buffers. **The kernel does not raise a signal for syscall-side faults**: when `copy_from_user`/`copy_to_user` (Linux) or `copyin`/`copyout` (macOS) hits a protected page during a syscall, the kernel uses the page-fault fixup path to convert the fault into `-EFAULT` and discard the address. The SIGSEGV/SIGBUS handler in `fault_handler.h` only fires for direct user-code accesses; for syscalls we have to detect EFAULT ourselves and trigger a userspace touch (which *does* go through the handler) so the page can be decompressed.

### EFAULT-driven decompress-and-retry (`syscall_compat.h::retryWithDecompress`)

Every buffer-taking syscall wrapper follows the same shape: call the real syscall, on `errno == EFAULT` walk the buffer pages (one byte per page; the SIGSEGV handler decompresses), then retry. Bounded to 8 attempts with µs-scale exponential backoff (1, 2, 4, …, 128 µs); the compressor tick fires at ~10 ms intervals so 8 attempts at 128 µs is well inside one tick. Bounded so unmapped-pointer bugs surface as EFAULT instead of livelocking. `retryMachOnInvalidData` is the same shape but keyed on `MACH_RCV_INVALID_DATA` / `MACH_SEND_INVALID_DATA` for the mach_msg trio.

The PageState CAS + per-page lock are sufficient for correctness — there is no separate pin counter. The fault handler takes the same per-page lock as the compressor's `transition(ACTIVE → COMPRESSING)`, so syscall-retry → walk → fault → handler → decompress → retry is correct without a pin counter.

### DYLD interposition limitations (macOS) — the buffered-I/O carve-out

`__DATA,__interpose` only intercepts **cross-dylib** GOT calls. Intra-libSystem calls are invisible:
- `fread()`/`fgetc()` → internal `read()`: NOT intercepted (same dylib)
- `getc_unlocked` is a macro (`__sgetc`) inlined into callers, calls `__srget` for refills
- C++ iostream (`std::cin`) → `getc_unlocked` → `__srget` → `read()` (all intra-libSystem)
- Direct `read()` from application code: IS intercepted (cross-dylib)

For these paths the EFAULT-retry model does not help — those syscalls happen inside libc and never enter our wrapper. Two carve-outs exist:

1. **Buffered I/O wrappers** (`fread`/`fgets`/`fgetc`/`getc`/`fwrite`/`fflush` on both platforms): proactively `warmPages` the user buffer + the FILE's internal buffer (macOS `stream->_bf._base`, Linux `stream->_IO_buf_base`) before delegating to libc. There is no retry path because libc's internal `__read` is intra-dylib.
2. **stdio buffer warming** (`compressor_thread.h::warmStdioBuffers`, macOS-only): re-warms `stdin`/`stdout`/`stderr` FILE struct + buffer each compressor tick so the kernel never finds them protected. Without this, intra-libSystem `getc_unlocked` / `__srget` would EFAULT inside libc with no retry surface.

### Interposed functions

- macOS (`smash_heap.cpp`): `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `recv`, `send`, `recvfrom`, `sendto`, `recvmsg`, `sendmsg`, `poll`, `kevent`, `kevent64`, `mach_msg`, `mach_msg_overwrite`, `mach_msg2_internal`, plus the buffered-I/O carve-out above.
- Linux (`linux_syscall_wrappers.cpp`): everything macOS has minus the Mach trio plus `ppoll`, `select`, `pselect`, `accept`, `accept4`, `recvmmsg`, `sendmmsg`, `getsockopt`, `getsockname`, `getpeername`, `getrandom`, `epoll_wait`, `epoll_pwait`.

### Platform-specific interposition patterns

**macOS**: Do NOT use `dlsym(RTLD_NEXT)` — it returns the wrapper itself. Instead read the `.original` field from the interpose struct:
```cpp
extern "C" ssize_t smash_read(int fd, void* buf, size_t count);
SMASH_INTERPOSE(smash_read, read);
extern "C" ssize_t smash_read(int fd, void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    return smash::vm::retryWithDecompress(
        [&] { return reinterpret_cast<read_fn>(smash_interpose_smash_read.original)(fd, buf, count); },
        [&] { if (vm && buf && count) smash::vm::walkPagesForFault(buf, count, vm); });
}
```

**Linux**: Use `dlsym(RTLD_NEXT)` with lazy resolution in `linux_syscall_wrappers.cpp`. For versioned glibc symbols (e.g., `epoll_wait@GLIBC_2.3.2` used by libevent), create aliased wrapper functions with `.symver` directives and export both versions in `smash_version_script.map`:
```cpp
// Wrapper for GLIBC_2.3.2 version
SMASH_VISIBLE int epoll_wait_232(...) { return epoll_wait(...); }
__asm__(".symver epoll_wait_232,epoll_wait@GLIBC_2.3.2");
```

## External-Mapping Tracking (`SMASH_TRACK_EXTERNAL=1`)

Standard smash compresses pages within its own `MAP_ANON` arena. Application code that calls `mmap()` / `mach_vm_allocate()` directly bypasses malloc and so escapes the compressor. The mmap and Mach VM interposers in `smash_heap.cpp` (macOS) and `linux_syscall_wrappers.cpp` (Linux) register such mappings with the VmRegion's external-page hash so the compressor's tick can walk them. **Opt-in**: set `SMASH_TRACK_EXTERNAL=1` to enable the registration path; default off.

### VmRegion full+tracking hybrid (`vm/vm_region.h`)

In full mode VmRegion keeps the contiguous bump-arena (indices `0..contig_pages_-1`) AND a tracking hash for external pages (indices `contig_pages_..contig_pages_+kTrackMaxPages-1`). `total_pages_ = contig_pages_ + kTrackMaxPages` so `PageStateTable` / `PageLockTable` cover both ranges; `committedPages()` returns the high-water across both; `pageAddress(idx)`, `pageIndex(addr)`, and `contains(addr)` route on idx range / address range. The compressor's existing tick / dispatch logic processes external pages without modification — they're just pages with high indices.

Cost: ~1 MB extra bootstrap memory (track hash + reverse map + page-state slots for `kTrackMaxPages = 128 K` external slots).

### Filter rules (deliberately strict)

- `mmap` only registers `MAP_ANON | PROT_WRITE` mappings. File-backed mappings are skipped — compressing them would break `msync` semantics and the OS already evicts them. Read-only mappings are skipped — no dirty bits, no compression value.
- `mach_vm_allocate` / `vm_allocate` only register when `target == mach_task_self()`. Cross-task allocations belong to children.
- `BootstrapAlloc`-routed `mmap` calls happen before `g_smash_vm_region` is set, so they're never tracked. Smash's own contiguous reservation uses `PROT_NONE` and is filtered by the writable-only rule.
- On `munmap` / `mach_vm_deallocate` we mark pages EMPTY before the actual unmap so the compressor stops scanning them. Compressed-buffer leakage on unmap is possible (we don't free the associated compressed bytes); bounded by workload churn.

### Why opt-in

Single-trial Firefox 5-tab Wikipedia at 90 s (full smash + DEFER 30 s + all-procs) showed the registration path possibly regressing stability (33–35 s lifetime vs 61 s with tracking off), but the variance across nominally-identical configs is too high to claim a regression with confidence — FIREFOX_STUDY's "10/10 alive at 60 s" baseline used N=10. Conservative default is off; targets with controlled allocation patterns (e.g. redb-style workloads) can opt in. The interposers themselves still install regardless of the env var, so the runtime cost when off is one branch per `mmap` / `mach_vm` call.

The "compressed=266K pages = 4.2 GB" figure observed under tracking-on on Firefox is misleading: most of those are virtual-address-space artifacts (SpiderMonkey + Skia reserve large `MAP_ANON` ranges that never fault in; the compressor processes zero pages to ~30-byte buffers). A correct Firefox-RSS measurement needs virt-vs-RSS reconciliation in the SIGUSR2 stats handler before claiming any Firefox win.

## Production Configuration

For applications with concurrent threads and significant slab/small-object traffic (e.g., neuron-cc, walrus C++ backend), the production-supported configuration is:

```
LD_PRELOAD=/path/to/libsmash.so PYTHONMALLOC=malloc SMASH_LARGE_ONLY=1
```

Full mode (without `SMASH_LARGE_ONLY=1`) is **experimental**.

**2026-06-05 — decompress-on-fault TOCTOU race found and fixed.** The dominant
full-mode failure on neuron-cc (nondeterministic ~67% failure rate, surfacing as
"overlapping memloc", BIR-verification, scheduler, or DenseMap assertions inside
the multithreaded `walrus_driver mod_parallel_pass`) was a real smash bug, not a
neuron-cc bug. `handleFault()` / `prefetchAdjacent()` restored a compressed page
by doing `mprotect(PROT_RW)` **then** `memcpy(decompressed)`, leaving a window in
which the page was readable but still held stale/zero bytes. A concurrent app
thread doing a plain load on that page does not fault and does not take the
per-page lock, so it read the wrong data and corrupted the compiler's state.
Fixed in `CompressorThread::restorePageContents()`: on Linux the decompressed
bytes are written through `/proc/self/mem` while the page is still `PROT_NONE`
(kernel `FOLL_FORCE` write honors the VMA's `VM_MAYWRITE`), then the page is
flipped to `PROT_RW` — concurrent readers keep faulting and block on the per-page
lock until the data is in place. Non-Linux keeps the legacy commit-then-copy.
Binary-search evidence that localized it: no-compression configs pass 100%; both
LZ4 and zstd fail (codec-independent); `SMASH_FIXAV=1` failed *worse* (0/3,
because it `madvise`s backing immediately, so the window always exposed zeros);
`SMASH_NO_DECOMMIT=1` turned corruption into OOM (backing never dropped → window
exposed correct data). Post-fix: 16/16 smash unit tests pass and full-mode
neuron-cc runs pass repeatedly (was ~1/3).

Status of the earlier-known blockers as of 2026-05-31:

**The "isl_id_free aborts in glibc free" failure is a neuron-cc bug, not a smash bug.** Root cause located in `neuronxcc/driver/JobRegistry.py:51`:

```python
sys.setdlopenflags(original_flags | os.RTLD_DEEPBIND)
```

That line was added to work around a TVM/LLVM symbol clash (TVM has since been deleted; the comment in the source admits it). With `RTLD_DEEPBIND`, the dynamic linker resolves the freshly-dlopen'd DSO's relocations against **its own symbol scope first**, before any LD_PRELOAD libraries. So `_isl.so`'s `free@plt` slot binds to `/lib64/libc.so.6 :: free` rather than to the LD_PRELOADed allocator's free. Same buffer was allocated through smash (or jemalloc/tcmalloc) via `strdup@plt` / `calloc@plt`, then freed through libc's free → glibc reads what it thinks is its own chunk header → "free(): invalid size" / "double free" / "munmap_chunk(): invalid pointer".

Verified empirically (2026-05-31) using `tools/free_probe.c` (a tiny LD_PRELOAD probe that walks the dynamic linker structures to read the actual GOT slot for `free@plt` in each loaded DSO):

- Standalone Python + jemalloc: `_isl.so :: free@plt -> jemalloc :: free` ✓
- Same with smash: `_isl.so :: free@plt -> libsmash :: free` ✓
- Inside neuron-cc's job-import path (after JobRegistry sets DEEPBIND): `_isl.so :: free@plt -> /lib64/libc.so.6 :: free` ✗ — bypasses every LD_PRELOAD allocator.

This explains why the original CLAUDE.md text talked about a "slab race in smash" — it's not. The same failure mode reproduces under jemalloc with no smash code involved at all, and it was happening to neuron-cc with non-glibc allocators long before smash existed.

**The fix is in `JobRegistry.__getJobFactory`**: drop the `RTLD_DEEPBIND` (TVM is gone) and gate the old behaviour behind `NEURON_KEEP_DEEPBIND=1` for anyone who still needs it. One-line change in neuron-cc, not in smash. After applying, the islpy crash disappears.

After the DEEPBIND fix, full mode hits a SECOND blocker: **`libwalrus.so` exports its own tcmalloc-built `malloc`/`free`/`calloc`/`realloc`** as strong global symbols. Verified 2026-06-01 via `nm` + `objdump -R`:

- `libwalrus.so` defines `T malloc` at `0x17c76c0`, `T free` at `0x17c58c0`, etc., as **non-versioned strong globals** (`@@Base`, not `@@GLIBC_*`).
- The full `tcmalloc::` C++ namespace appears in defined symbols: `tcmalloc::ThreadCache::BecomeIdle`, `tcmalloc::DLL_Remove`, `tcmalloc::Span`, etc.
- libwalrus is loaded by hlo2penguin and by other neuron-cc binaries that link `-lwalrus`.

(My earlier write-up incorrectly attributed this to `hlo2penguin` itself — that binary is *clean*, with 7191 `call malloc@plt` sites and `R_X86_64_JUMP_SLOT  malloc@GLIBC_2.2.5` relocations. The static tcmalloc lives in libwalrus.so.)

When libwalrus is loaded into a process that ALSO has `LD_PRELOAD=libsmash.so`, the dynamic linker resolves `malloc` from whichever DSO appears first in the symbol search order. With LD_PRELOAD smash should win — but for *intra-libwalrus* calls to `malloc`, the linker may bind directly to the local strong definition (especially under `-Bsymbolic` or RTLD_DEEPBIND combinations). And anything libwalrus allocates via its built-in tcmalloc has a tcmalloc chunk header that smash's free won't accept (and vice versa). Mismatched pairs surface as `src/tcmalloc.cc:333] Attempt to free invalid pointer` (signature captured 2026-06-01 with `SMASH_LARGE_ALLOC_VM_THRESHOLD=65536`).

Real fix: rebuild libwalrus.so without statically-linked tcmalloc, OR rebuild it with `-Wl,-Bsymbolic-functions` removed and ensure tcmalloc symbols are weak/not-exported so LD_PRELOAD wins. That's a CMake change in neuron-cc, not in smash.

Diagnostic for this class of bug: `tools/death_trace.c` catches every fatal signal AND every `_exit()`/`_Exit()` with non-zero status. Build with `gcc -O0 -fPIC -shared -o death_trace.so tools/death_trace.c -ldl`. Use as the second LD_PRELOAD entry. Was needed because the worker was leaving via `_exit(1)`, not abort, so `abort_trace.so` saw nothing.

Other findings, less load-bearing:
- Smash's interposers DO cover every allocator symbol `_isl.so` imports (audited via `nm -D --undefined-only`): `malloc`, `calloc`, `realloc`, `free`, `strdup`, plus the printf and qsort families that internally go through `*@plt`. No interposition gap.
- After the JobRegistry fix removes DEEPBIND, the islpy crash disappears, but a *different* failure surfaces — child workers in `parallelCompileSubGraphs` (concurrent.futures ProcessPoolExecutor) terminate without a captured signal. That's unrelated to islpy and looks like a fork/compressor-thread interaction; track separately.

Large-only mode bypasses both issues by leaving slab/small allocations to the system malloc and only managing allocations ≥ 16 KB. Verified 9/9 PASS at `SMASH_COLD_TIMEOUT_SEC ∈ {1, 5, 10}` on neuron-cc test7_full (largest HLO, 9.3 MB) post the `SMASH_DEFER_MADVISE` correctness fix.

Diagnostics added 2026-05-31:
- `SMASH_TRACE_FOREIGN_FREE=1` — log the first 32 frees that smash receives for pointers it does not recognise (i.e., where it is about to forward to `g_system_alloc.free`). Includes return address + caller DSO. Zero-overhead in steady state.
- `SMASH_COUNT_FREE=1` — count every free entry, log the count every ~1M frees. Useful for confirming the interposer is actually on the call path.
- `tools/free_probe.c` — standalone LD_PRELOAD probe that, post-`dlopen`, dumps the runtime target of `free@plt` for `_isl.so` / `libwalrus` / `libsmash`. Build with `gcc -shared -fPIC -O0 -o free_probe.so tools/free_probe.c -ldl`. Use as the second LD_PRELOAD entry: `LD_PRELOAD=libsmash.so:free_probe.so python3 …`.

The other knob that's load-bearing for correctness:

```
SMASH_DEFER_MADVISE=1   # default ON; do not disable in production
```

Defers `madvise(MADV_DONTNEED)` to a per-tick sweeper that only runs after a page has been quiescent for `SMASH_DEFER_MADVISE_TICKS=N` ticks (default 50, ≈500 ms). Closes a separate corruption race where in-flight loads/stores from a writer's stale TLB observed a recently-DROPPED page and saw zeros.

## Large-Only Mode (`SMASH_LARGE_ONLY=1`)

For applications with their own small-object allocator (e.g., Python 3.13+ uses mimalloc), set `SMASH_LARGE_ONLY=1` to only manage large allocations (> `kMaxSmallSize` = 16KB):

- `malloc(size <= 16KB)` → system malloc passthrough
- `malloc(size > 16KB)` → Smash's `LargeAlloc` → VmRegion (compressible)
- `free(ptr)` checks `page_map_`; non-Smash pointers forwarded to system free
- `getSize(ptr)` checks `page_map_` first (returns Smash size class or `span->large_size`); falls back to system `malloc_size`/`malloc_usable_size` only for non-Smash pointers

### Resolving original system malloc on macOS

On macOS, `dlsym(RTLD_NEXT, "malloc")` returns the interposed wrapper — DYLD interposition is truly global and cannot be bypassed via dlsym (even `dlopen("libsystem_malloc.dylib") + dlsym` returns the wrapper). To get the real system malloc, `SystemAllocFns::findOriginal()` scans the `__interpose` Mach-O section at runtime. On ARM64e (Apple Silicon), this section lives in `__AUTH_CONST` (not `__DATA`) due to pointer authentication. Each entry is `{replacement, original}` — match by replacement address, read the original.

## macOS Page Reclamation

`mprotect(PROT_NONE)` does NOT release physical memory on macOS — the RSS drop visible in `task_info` is a reporting artifact. To actually reclaim physical pages, use `MADV_FREE_REUSABLE` (madvise hint 7), which is what jemalloc and WebKit use. `MADV_FREE_REUSABLE` requires pages to be accessible (PROT_READ or PROT_RW); it fails with EPERM on PROT_NONE pages.

The compressor flow calls `decommitPages()` (MADV_FREE_REUSABLE) **before** `mprotect(PROT_NONE)`, after the page data has been copied to the scratch buffer. On Linux, `MADV_DONTNEED` works regardless of protection.

## Key Conventions

- Never allocate from the managed heap inside smash internals — use BootstrapAlloc
- Data pages never contain metadata (bitmap-based free tracking, pointer arrays in thread cache)
- Fine-grained locking: per-slab spinlocks, per-page spinlocks. No global heap lock. 4 arenas reduce slab lock contention.
- `PageLockTable::tryLock()` used for prefetch to avoid deadlock
- Compressor startup: constructor at priority 201 (after alloc8 pthread hooks at 200) calls `xxthread_init()` twice on macOS, once on Linux, ensuring the compressor starts even for non-ObjC programs (e.g., Python) and single-threaded programs
- Fault handler handles ACTIVE state (not just ACTIVE_MONITORING) to cover the race where Phase 3's batched mprotect overwrites a per-page PROT_RW restoration
- ThreadCache `drain()`/`drainAll()` bucket pointers by `span->arena_id` before returning to correct arena's slab

## Dependencies

- **alloc8**: Interposition framework (sibling directory)
- **LZ4 v1.9.4**: Fast compression (fetched via CMake FetchContent)
- **Zstandard v1.5.6**: Dictionary compression (fetched via CMake FetchContent)

## Paper Experiments

All experiments are run from the `build/` directory. Results go into `paper_results/`.

### Prerequisites

```bash
# Build with benchmarks enabled
cd build
cmake .. -DSMASH_BUILD_BENCH=ON && make -j$(nproc)

# External tool dependencies
brew install memcached redis rocksdb duckdb
# Allocator compare also needs: mimalloc, jemalloc, tcmalloc, hoard (built via FetchContent/find_library)
```

`SMASH_BUILD_BENCH=ON` is the master switch. Two sub-flags gate heavy chunks of the bench tree, both default `ON`:

| Flag | Gates |
|------|-------|
| `SMASH_BUILD_BENCH_DEPS` | The `bench_deps` target (Redis, memcached, DuckDB, RocksDB built from source via ExternalProject_Add). |
| `SMASH_BUILD_BENCH_ALLOCATORS` | The allocator-comparison block — mimalloc + jemalloc + tcmalloc + hoard + mesh + diehard + dieharder targets, plus `bench_allocator_compare.py.in`. Pulls in tcmalloc via Bazel (when `bazel`/`bazelisk` is on PATH and glibc ≥ 2.35) which uses `bench/tcmalloc_patch_build.cmake` to inject a `cc_binary(libtcmalloc_preload.so)` rule into the upstream `//tcmalloc:BUILD` file. |

Paper experiments need both `=ON`. CI regression runs (`.github/workflows/ci.yml`) set both `=OFF` because the quick benches (`bench_rss`, `bench_sqlite`) don't need any of those allocators or external services, and skipping the heavy paths cuts CI build time from ~10 min to ~3 min.

### Unified Experiment Runner (ablation + compress-only)

```bash
cd build

# Run all experiments (full — for paper-quality results)
python3 ../bench/run_paper_experiments.py --runs 3

# Quick smoke test (smaller datasets, 1 run)
python3 ../bench/run_paper_experiments.py --quick --runs 1

# Ablation only
python3 ../bench/run_paper_experiments.py --ablation-only --runs 3

# Compress-only only
python3 ../bench/run_paper_experiments.py --compress-only-only --runs 3

# Subset of apps
python3 ../bench/run_paper_experiments.py --apps sqlite,rocksdb --runs 3
```

**Ablation configs** (9 variants, each rebuilds libsmash with different CMake defines):
- B1: Default (baseline Smash)
- B0: System malloc (no Smash)
- DICT: With dictionary training (`SMASH_DICT_TRAIN_SAMPLES=16`)
- T1a: No arenas (`SMASH_NUM_ARENAS=1`)
- T1c: Fast tier only (`SMASH_VERY_COLD_TICKS=9999`)
- T2a: No zero-deferred (`SMASH_ABLATION_NO_ZERO_DEFERRED=ON`)
- T1e: No prefetch (`SMASH_PREFETCH_WINDOW=0`)
- T1f: Single worker (`SMASH_COMPRESSOR_WORKERS=1`)
- B2: No compression (`SMASH_COLD_TICKS=9999`)

**Compress-only** tests 3 configs per app: baseline (system malloc), compress-only (`libsmash_compress_only.dylib`), full Smash.

**Output**: `paper_results/ablation_results.json`, `paper_results/compress_only_results.json`, `paper_results/paper_tables.txt`

### Application Benchmark Shell Scripts

Individual app benchmarks with detailed output (A/B comparison tables):

```bash
cd build

# RocksDB (compares baseline, smash, rocksdb-lz4, rocksdb-zstd, smash+lz4)
bash bench/bench_rocksdb.sh [--quick]

# Memcached (fill → cool → serve → cold re-access)
bash bench/bench_memcached.sh [--quick]

# Redis
bash bench/bench_redis.sh [--quick]

# DuckDB (TPC-H queries)
bash bench/bench_duckdb.sh [--quick]

# Multi-allocator comparison on Redis/Memcached
bash bench/bench_redis_alloc.sh
bash bench/bench_memcached_alloc.sh
bash bench/bench_duckdb_alloc.sh
```

### Allocator Substrate Comparison (RQ5)

Standalone benchmark measuring page compressibility across allocators:

```bash
cd build

# Run the configured Python runner
python3 bench/bench_allocator_compare.py

# Or run individual allocator benchmarks directly
./bench/bench_alloc_system --data json --size 64 --count 100000
./bench/bench_alloc_mimalloc --data kv --size 256
# With Smash interposition:
DYLD_INSERT_LIBRARIES=./libsmash.dylib ./bench/bench_alloc_system --data mixed --size 128
```

Available allocator binaries: `bench_alloc_{system,mimalloc,jemalloc,tcmalloc,hoard,diehard,dieharder}` and `*_zero` variants.

### Algorithm Comparison (RQ3)

```bash
cd build
./bench/bench_algo_compare    # Compression ratios + throughput across LZ4/zstd/WKdm
```

### In-Process Benchmarks (C++)

```bash
cd build
./bench/bench_sqlite [--quick]    # SQLite in-memory DB benchmark
./bench/bench_rocksdb [--quick]   # RocksDB block cache benchmark
```

### Generating Figures

```bash
cd paper/figures
python3 plot_all.py              # Main figures (rss_reduction, ablation, algo_compare, etc.)
python3 plot_rss_timeline.py     # RSS over time (Figure 7)
python3 plot_cdf.py              # Cold-access latency CDF (Figure 8)
```

### Building the Paper

```bash
cd paper && pdflatex paper && bibtex paper && pdflatex paper && pdflatex paper
```

## Application-Specific Configuration

### Redis

Redis's event loop and background tasks can prevent Smash from compressing pages effectively. By default, Redis touches heap pages frequently via:

- **Event loop timer** (`hz` setting): Runs background tasks at 10 Hz by default
- **Active defragmentation** (`activedefrag`): Scans memory for fragmentation
- **Incremental rehashing** (`activerehashing`): Resizes hash tables incrementally
- **Lazy-free operations**: Background deletion of large objects

To achieve effective compression with Smash, disable these background activities:

```bash
redis-server --port 6379 \
    --hz 1 --dynamic-hz no \          # Minimize event loop frequency
    --activedefrag no \               # Disable active defragmentation
    --activerehashing no \            # Disable incremental rehashing
    --lazyfree-lazy-user-del no \     # Synchronous deletes
    --lazyfree-lazy-expire no \       # Synchronous expirations
    --lazyfree-lazy-eviction no \     # Synchronous evictions
    --maxmemory-policy noeviction \   # Prevent LRU eviction touching pages
    --save "" --appendonly no         # Disable persistence
```

**EC2 benchmark results (200K ops, 2KB values, 20s cooling):**

**Standard workload (SET → cool → GET):**
| Config | Fill RSS | Min RSS | Reduction | AUC |
|--------|----------|---------|-----------|-----|
| jemalloc (default) | 333 MB | 332 MB | 0.4% | 6651 MB*s |
| jemalloc (bg disabled) | 334 MB | 333 MB | 0.4% | 6672 MB*s |
| **Smash (bg disabled)** | 382 MB | 204 MB | **47%** | **4388 MB*s** |

**Extended workload (SET → DELETE 50% → cool → GET):**
| Config | Fill RSS | Min RSS | Reduction | AUC |
|--------|----------|---------|-----------|-----|
| jemalloc (bg disabled) | 333 MB | 331 MB | 0.7% | 6623 MB*s |
| Smash (bg disabled) | 386 MB | 637 MB | **-65%** | 12750 MB*s |

Key findings:
- **Disabling background tasks has no effect on jemalloc** (RSS, AUC identical)
- **Standard workload: Smash achieves 47% RSS reduction and 34% lower AUC**
- **Extended workload: Smash shows NEGATIVE benefit** (-65% RSS, +93% AUC) because DELETE operations cause decompression, and the fragmented pages don't re-compress well

Without these flags, Redis's background tasks keep pages warm and Smash cannot compress them effectively.

## Config Tuning

Key constants in `include/smash/config.h`:
- `kColdTicks = 2`: Ticks without access before fast-tier compression considered
- `kVeryColdTicks = 60`: Cold-tick threshold for the deep-tier (zstd-9) profile in the ROI model
- `kMinCompressRatio = 0.75`: Only store if compressed < 75% of original
- `kPrefetchWindow = 2`: Pages prefetched in each direction on fault
- `kDictTrainSamples = 0`: Pages before dictionary training (disabled by default; dicts net-negative)
- `kNumArenas = 4`: Call-site arena count (must be power of 2)
- `kCompressorWorkers = 2`: Initial compression worker count
- `kMaxCompressorWorkers = 8`: Cap for adaptive worker scaling (Little's Law)
- `kCompressStoreShards = 8`: CompressStore lock shards
- `kChunkSize = 64`: Pages per chunk for scan bitmap
- `kLargeAllocVmThreshold = 1MB`: Only large allocs above this go in VmRegion

Runtime environment variables:
- `SMASH_LARGE_ONLY=1`: Large-only mode — small allocations (≤16KB) pass through to system malloc. Production-supported config for concurrent workloads
- `SMASH_DEFER_MADVISE=1`: Default ON. Defers `madvise(MADV_DONTNEED)` to a per-tick sweeper after `SMASH_DEFER_MADVISE_TICKS` quiescent ticks (default 50). Load-bearing for correctness; do not disable in production
- `SMASH_DEFER_MADVISE_TICKS=N`: Number of ticks (default 50, ≈500 ms) a page must be quiescent before sweeper madvises it
- `SMASH_MODE=compress_only`: Compress-only mode — track pages without replacing malloc
- `SMASH_TRACK_EXTERNAL=1`: Register application-direct `mmap` / `mach_vm_allocate` results so the compressor sees them. Opt-in (see "External-Mapping Tracking" above)
- `SMASH_DEFER_PHASES_MS=N`: Skip Phase 2 (compress) + Phase 3 (monitor) for the first N ms after start. Useful for workloads that establish IPC channels at startup with buffers in smash-managed pages (Firefox sweet spot is 30000)
- `SMASH_NO_MONITOR=1`: Disable Phase 3 (PROT_READ access tracking) entirely. Trades cold-detection accuracy for compatibility with code paths that synchronously check page protection
- `SMASH_COLD_TIMEOUT_SEC=N`: Override cold timeout at runtime
- `SMASH_VERY_COLD_TICKS=N`: Override deep-tier cold-tick threshold (ROI model). `9999` disables the deep tier entirely (fast tier only).
- `SMASH_ROI_THRESHOLD=N`: Override ROI cutoff (default 1024)
- `SMASH_FAST_COMP_MBS_HI/LO`, `SMASH_FAST_DECOMP_MBS_HI/LO`: Override fast-tier calibration
- `SMASH_DEEP_COMP_MBS_HI/LO`, `SMASH_DEEP_DECOMP_MBS_HI/LO`: Override deep-tier calibration
- `SMASH_CALIBRATE=always|never`: Force/skip startup calibration
- `SMASH_CALIBRATION_FILE=path`: Cache and reload calibration

## Benchmark Result Provenance

Every results JSON written by `bench/run_paper_experiments.py` (`ablation_results.json`, `compress_only_results.json`, `duckdb_compression_results.json`) carries:

- **Top-level `_sessions[]`** appended per runner invocation: timestamp_utc, runs_requested, quick flag, apps list, `system_info` (hostname, platform, CPU, cores, mem_gib, page_size, tool versions for cmake/gcc/clang/redis-server/memcached), `smash_env_at_start` (snapshot of all `SMASH_*` env vars), and `bench_params` (the actual keys/value_size/num_clients/cool_sec/server_flags used by each `run_*` function).
- **Per-(app, config) `provenance`**: `cmake_flags`, `smash_env`, `source_hash` (SHA-256 of `src/` + `include/` + top-level `CMakeLists.txt`; catches uncommitted edits), `libsmash_sha256` and `libsmash_mtime`, `git_head` and `git_dirty`.

When `git_head` is `null` (e.g., directory populated via rsync), `source_hash` is the authoritative "what code was measured" value. Helpers live in the runner: `collect_system_info()`, `collect_source_hash()`, `collect_git_info()`, `collect_smash_env()`, `build_provenance()`.
