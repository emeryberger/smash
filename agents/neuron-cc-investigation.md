# Neuron-CC Investigation Notes

This document captures the detailed investigation into running smash under neuron-cc (AWS Neuron compiler). The production-supported configuration for neuron-cc is `SMASH_LARGE_ONLY=1` (see CLAUDE.md).

## Decompress-on-fault TOCTOU race (2026-06-05, FIXED)

The dominant full-mode failure on neuron-cc (nondeterministic ~67% failure rate, surfacing as "overlapping memloc", BIR-verification, scheduler, or DenseMap assertions inside the multithreaded `walrus_driver mod_parallel_pass`) was a real smash bug, not a neuron-cc bug. `handleFault()` / `prefetchAdjacent()` restored a compressed page by doing `mprotect(PROT_RW)` **then** `memcpy(decompressed)`, leaving a window in which the page was readable but still held stale/zero bytes. A concurrent app thread doing a plain load on that page does not fault and does not take the per-page lock, so it read the wrong data and corrupted the compiler's state.

Fixed in `CompressorThread::restorePageContents()`: on Linux the decompressed bytes are written through `/proc/self/mem` while the page is still `PROT_NONE` (kernel `FOLL_FORCE` write honors the VMA's `VM_MAYWRITE`), then the page is flipped to `PROT_RW` — concurrent readers keep faulting and block on the per-page lock until the data is in place. Non-Linux keeps the legacy commit-then-copy.

Binary-search evidence that localized it: no-compression configs pass 100%; both LZ4 and zstd fail (codec-independent); `SMASH_FIXAV=1` failed *worse* (0/3, because it `madvise`s backing immediately, so the window always exposed zeros); `SMASH_NO_DECOMMIT=1` turned corruption into OOM (backing never dropped -> window exposed correct data). Post-fix: 16/16 smash unit tests pass and full-mode neuron-cc runs pass repeatedly (was ~1/3).

## Async-signal-safety bugs in the fault handler (2026-06-10, FIXED)

Both in `FaultHandler::signalHandler()` (`src/vm/fault_handler.h`), both caused by touching not-async-signal-safe machinery inside the handler under `SA_NODEFER` (a fault inside the handler re-enters it -> unbounded recursion or trap).

- **`getenv()` recursion (Linux).** The handler's chain-to-default path called `std::getenv()` (`SMASH_SIGTRACE`, `SMASH_DUMP_CRASH_BT`). `getenv()` is not async-signal-safe; a fault during libc's `environ` walk re-enters the handler, which calls `getenv()` again -> stack-overflow SIGSEGV. **This crashed redis under full smash** (and any app whose fault chains to default). Diagnosed via gdb on a `redis-server` core: backtrace `getenv -> signalHandler -> __restore_rt -> getenv -> ...`. Fixed by caching both env flags once at handler-install time (`start()`); the handler reads cached bools only. The other `getenv()` calls in the file (Mach listener thread, install time) are off the signal path.
- **macOS SIGILL.** `faultWasWrite()`'s function-local `thread_local` carries a lazy-init guard that traps with SIGILL when first run inside the signal handler (macOS CI: `bench_sqlite` exited -4 in the fault/decompress phase, on every commit — an earlier fix removed the initial-exec TLS attribute but not the guard). The value is only set meaningfully on Linux x86-64 and read once (defaulting unset -> "treat as write"), so the in-handler access is now guarded to `__linux__`; macOS never touches it in the handler. macOS CI green post-fix.

Post-fix all four redis bench configurations (stock / extended-DELETE / patched / ext-patched) run to completion under full smash and match or beat the paper (54-77% RSS reduction); before the `getenv` fix redis crashed during the run.

## The "isl_id_free aborts in glibc free" failure (neuron-cc bug, NOT smash)

Root cause located in `neuronxcc/driver/JobRegistry.py:51`:

```python
sys.setdlopenflags(original_flags | os.RTLD_DEEPBIND)
```

That line was added to work around a TVM/LLVM symbol clash (TVM has since been deleted; the comment in the source admits it). With `RTLD_DEEPBIND`, the dynamic linker resolves the freshly-dlopen'd DSO's relocations against **its own symbol scope first**, before any LD_PRELOAD libraries. So `_isl.so`'s `free@plt` slot binds to `/lib64/libc.so.6 :: free` rather than to the LD_PRELOADed allocator's free. Same buffer was allocated through smash (or jemalloc/tcmalloc) via `strdup@plt` / `calloc@plt`, then freed through libc's free -> glibc reads what it thinks is its own chunk header -> "free(): invalid size" / "double free" / "munmap_chunk(): invalid pointer".

Verified empirically (2026-05-31) using `tools/free_probe.c` (a tiny LD_PRELOAD probe that walks the dynamic linker structures to read the actual GOT slot for `free@plt` in each loaded DSO):

- Standalone Python + jemalloc: `_isl.so :: free@plt -> jemalloc :: free` (correct)
- Same with smash: `_isl.so :: free@plt -> libsmash :: free` (correct)
- Inside neuron-cc's job-import path (after JobRegistry sets DEEPBIND): `_isl.so :: free@plt -> /lib64/libc.so.6 :: free` (WRONG — bypasses every LD_PRELOAD allocator)

This explains why the original CLAUDE.md text talked about a "slab race in smash" — it's not. The same failure mode reproduces under jemalloc with no smash code involved at all, and it was happening to neuron-cc with non-glibc allocators long before smash existed.

**The fix is in `JobRegistry.__getJobFactory`**: drop the `RTLD_DEEPBIND` (TVM is gone) and gate the old behaviour behind `NEURON_KEEP_DEEPBIND=1` for anyone who still needs it. One-line change in neuron-cc, not in smash. After applying, the islpy crash disappears.

## libwalrus.so static tcmalloc conflict

After the DEEPBIND fix, full mode hits a SECOND blocker: **`libwalrus.so` exports its own tcmalloc-built `malloc`/`free`/`calloc`/`realloc`** as strong global symbols. Verified 2026-06-01 via `nm` + `objdump -R`:

- `libwalrus.so` defines `T malloc` at `0x17c76c0`, `T free` at `0x17c58c0`, etc., as **non-versioned strong globals** (`@@Base`, not `@@GLIBC_*`).
- The full `tcmalloc::` C++ namespace appears in defined symbols: `tcmalloc::ThreadCache::BecomeIdle`, `tcmalloc::DLL_Remove`, `tcmalloc::Span`, etc.
- libwalrus is loaded by hlo2penguin and by other neuron-cc binaries that link `-lwalrus`.

(The earlier write-up incorrectly attributed this to `hlo2penguin` itself — that binary is *clean*, with 7191 `call malloc@plt` sites and `R_X86_64_JUMP_SLOT  malloc@GLIBC_2.2.5` relocations. The static tcmalloc lives in libwalrus.so.)

When libwalrus is loaded into a process that ALSO has `LD_PRELOAD=libsmash.so`, the dynamic linker resolves `malloc` from whichever DSO appears first in the symbol search order. With LD_PRELOAD smash should win — but for *intra-libwalrus* calls to `malloc`, the linker may bind directly to the local strong definition (especially under `-Bsymbolic` or RTLD_DEEPBIND combinations). And anything libwalrus allocates via its built-in tcmalloc has a tcmalloc chunk header that smash's free won't accept (and vice versa). Mismatched pairs surface as `src/tcmalloc.cc:333] Attempt to free invalid pointer` (signature captured 2026-06-01 with `SMASH_LARGE_ALLOC_VM_THRESHOLD=65536`).

Real fix: rebuild libwalrus.so without statically-linked tcmalloc, OR rebuild it with `-Wl,-Bsymbolic-functions` removed and ensure tcmalloc symbols are weak/not-exported so LD_PRELOAD wins. That's a CMake change in neuron-cc, not in smash.

## Large-only mode bypasses all of the above

Large-only mode bypasses both issues by leaving slab/small allocations to the system malloc and only managing allocations >= 16 KB. Verified 9/9 PASS at `SMASH_COLD_TIMEOUT_SEC in {1, 5, 10}` on neuron-cc test7_full (largest HLO, 9.3 MB) post the `SMASH_DEFER_MADVISE` correctness fix.

## Diagnostics

- `SMASH_TRACE_FOREIGN_FREE=1` — log the first 32 frees that smash receives for pointers it does not recognise (i.e., where it is about to forward to `g_system_alloc.free`). Includes return address + caller DSO. Zero-overhead in steady state.
- `SMASH_COUNT_FREE=1` — count every free entry, log the count every ~1M frees. Useful for confirming the interposer is actually on the call path.
- `tools/free_probe.c` — standalone LD_PRELOAD probe that, post-`dlopen`, dumps the runtime target of `free@plt` for `_isl.so` / `libwalrus` / `libsmash`. Build with `gcc -shared -fPIC -O0 -o free_probe.so tools/free_probe.c -ldl`. Use as the second LD_PRELOAD entry: `LD_PRELOAD=libsmash.so:free_probe.so python3 ...`.
- `tools/death_trace.c` — catches every fatal signal AND every `_exit()`/`_Exit()` with non-zero status. Build with `gcc -O0 -fPIC -shared -o death_trace.so tools/death_trace.c -ldl`. Use as the second LD_PRELOAD entry. Was needed because the worker was leaving via `_exit(1)`, not abort, so `abort_trace.so` saw nothing.

Other findings, less load-bearing:
- Smash's interposers DO cover every allocator symbol `_isl.so` imports (audited via `nm -D --undefined-only`): `malloc`, `calloc`, `realloc`, `free`, `strdup`, plus the printf and qsort families that internally go through `*@plt`. No interposition gap.
- After the JobRegistry fix removes DEEPBIND, the islpy crash disappears, but a *different* failure surfaces — child workers in `parallelCompileSubGraphs` (concurrent.futures ProcessPoolExecutor) terminate without a captured signal. That's unrelated to islpy and looks like a fork/compressor-thread interaction; track separately.
