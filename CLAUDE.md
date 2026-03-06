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

All 12 tests must pass. The interpose test requires macOS (DYLD_INSERT_LIBRARIES).

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
- **Zero-on-free**: Objects ≤ `kZeroOnFreeMaxSize` (128B) are zeroed eagerly in `free()`. Larger objects are zeroed by the compressor in `zeroFreeSlots()` using non-temporal stores (`ntZeroMemory`) to avoid cache pollution. This makes partial pages compress well (zero runs → high LZ4 ratios).
- **PageState machine**: EMPTY → ACTIVE → ACTIVE_MONITORING → COMPRESSING → COMPRESSED → ACTIVE. CAS transitions ensure safe coordination between compressor thread and fault handler.
- **CompressEngine**: Supports LZ4, zstd, and zstd+dictionary. Algorithm packed in top 2 bits of `CompressedPageInfo::comp_size`. All zstd contexts pre-allocated via `ZSTD_customMem` routing to BootstrapAlloc.
- **Parallel compressor**: Coordinator thread + worker threads (`kCompressorWorkers`). Chunk bitmap (`live_chunks_[]`) skips EMPTY pages. Sharded `CompressStore` (8 shards) eliminates lock contention. Per-worker compression contexts (LZ4 state, ZSTD CCtx, scratch buffers, SizeClassStats).
- **Per-fault-slot DCtx**: Each of 32 fault slots has its own `ZSTD_DCtx*`, fixing data race between concurrent decompressions from app threads and prefetch.
- **Sliding-window stats**: `SizeClassStats` is a 64-entry ring buffer tracking compression ratios (0-255). Size classes can recover after data patterns change.
- **Signal handler path**: No malloc allowed. Decompression uses pre-allocated per-slot contexts only.

## Syscall & Buffered I/O Compatibility

Smash's mprotect-based monitoring (PROT_READ) and compression (PROT_NONE) can conflict with kernel syscalls that access userspace buffers. The kernel returns EFAULT instead of triggering SIGSEGV.

### DYLD interposition limitations (macOS)

`__DATA,__interpose` only intercepts **cross-dylib** GOT calls. Intra-libSystem calls are invisible:
- `fread()`/`fgetc()` → internal `read()`: NOT intercepted (same dylib)
- `getc_unlocked` is a macro (`__sgetc`) inlined into callers, calls `__srget` for refills
- C++ iostream (`std::cin`) → `getc_unlocked` → `__srget` → `read()` (all intra-libSystem)
- Direct `read()` from application code: IS intercepted (cross-dylib)

### Solutions implemented in `smash_heap.cpp`

1. **Syscall interposition**: Interpose `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `recv`, `send`, `recvfrom`, `sendto`, `recvmsg`, `sendmsg`, `poll`, `kevent` — warm and pin buffer pages before calling real syscall via `.original` field
2. **Buffered I/O interposition**: Interpose `fread`, `fgets`, `fgetc`, `getc`, `fwrite`, `fflush` — warm and pin both user buffer AND the FILE's internal buffer (`stream->_bf._base`)
3. **stdio buffer pinning** (`compressor_thread.h`): `pinStdioBuffers()` permanently pins stdin/stdout/stderr FILE struct + buffer pages at first compressor tick. This covers the intra-libSystem blind spot for standard streams.

### Pattern for calling original functions

Do NOT use `dlsym(RTLD_NEXT)` — on macOS it returns the wrapper itself. Instead read the `.original` field from the interpose struct:
```cpp
extern "C" ssize_t smash_read(int fd, void* buf, size_t count);
SMASH_INTERPOSE(smash_read, read);
extern "C" ssize_t smash_read(int fd, void* buf, size_t count) {
    // ... warm/pin ...
    ssize_t ret = reinterpret_cast<read_fn>(smash_interpose_smash_read.original)(fd, buf, count);
    // ... unpin ...
    return ret;
}
```

### Page pinning (`syscall_compat.h`)

`g_page_pins` is a per-page atomic counter. Pages with pin count > 0 are skipped by both Phase 2 (compression) and Phase 3 (monitoring). Used by syscall wrappers to protect kernel buffers during blocking calls.

## Key Conventions

- Never allocate from the managed heap inside smash internals — use BootstrapAlloc
- Data pages never contain metadata (bitmap-based free tracking, pointer arrays in thread cache)
- Fine-grained locking: per-slab spinlocks, per-page spinlocks. No global heap lock. 4 arenas reduce slab lock contention.
- `PageLockTable::tryLock()` used for prefetch to avoid deadlock
- Compression deferred until second `threadInit()` call (avoids macOS ObjC runtime crash during early DYLD_INSERT init)
- Fault handler handles ACTIVE state (not just ACTIVE_MONITORING) to cover the race where Phase 3's batched mprotect overwrites a per-page PROT_RW restoration
- ThreadCache `drain()`/`drainAll()` bucket pointers by `span->arena_id` before returning to correct arena's slab

## Dependencies

- **alloc8**: Interposition framework (sibling directory)
- **LZ4 v1.9.4**: Fast compression (fetched via CMake FetchContent)
- **Zstandard v1.5.6**: Dictionary compression (fetched via CMake FetchContent)

## Config Tuning

Key constants in `include/smash/config.h`:
- `kColdTicks = 2`: Ticks without access before LZ4 compression
- `kVeryColdTicks = 60`: Ticks before escalating to zstd/zstd+dict
- `kMinCompressRatio = 0.75`: Only store if compressed < 75% of original
- `kPrefetchWindow = 2`: Pages prefetched in each direction on fault
- `kDictTrainSamples = 16`: Pages collected before training a dictionary
- `kNumArenas = 4`: Call-site arena count (must be power of 2)
- `kCompressorWorkers = 2`: Parallel compression worker threads
- `kCompressStoreShards = 8`: CompressStore lock shards
- `kChunkSize = 64`: Pages per chunk for scan bitmap
- `kZeroOnFree = true`: Enable zero-on-free for better compression
- `kZeroOnFreeMaxSize = 128`: Eager memset threshold (larger objects zeroed by compressor)
- `kLargeAllocVmThreshold = 1MB`: Only large allocs above this go in VmRegion
