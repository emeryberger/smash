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
- **PageState machine**: EMPTY → ACTIVE → ACTIVE_MONITORING → COMPRESSING → COMPRESSED → ACTIVE. CAS transitions ensure safe coordination between compressor thread and fault handler.
- **CompressEngine**: Supports LZ4, zstd, and zstd+dictionary. Algorithm packed in top 2 bits of `CompressedPageInfo::comp_size`. All zstd contexts pre-allocated via `ZSTD_customMem` routing to BootstrapAlloc.
- **Signal handler path**: No malloc allowed. Decompression uses pre-allocated contexts only.

## Key Conventions

- Never allocate from the managed heap inside smash internals — use BootstrapAlloc
- Data pages never contain metadata (bitmap-based free tracking, pointer arrays in thread cache)
- Fine-grained locking: per-slab spinlocks, per-page spinlocks. No global heap lock.
- `PageLockTable::tryLock()` used for prefetch to avoid deadlock
- Compression deferred until second `threadInit()` call (avoids macOS ObjC runtime crash during early DYLD_INSERT init)

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
