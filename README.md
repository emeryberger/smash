# smash

**S**elective **M**emory **A**llocation with **S**mart **H**andling — a compression-aware memory allocator that transparently compresses cold pages to reduce resident set size (RSS).

## Overview

smash is a drop-in malloc replacement that monitors page access patterns and compresses pages that haven't been touched recently. When compressed pages are accessed again, a signal handler transparently decompresses them before the application sees the data. This reduces physical memory usage for applications with large working sets where significant portions of allocated memory are idle at any given time.

### Key Features

- **Transparent compression**: No application changes required — works via malloc interposition
- **Adaptive multi-algorithm**: LZ4 for recently cold pages, zstd for very cold pages, zstd+dictionary for homogeneous size classes
- **Per-size-class dictionaries**: Automatically trains compression dictionaries from page samples, exploiting structural similarity within size classes
- **Prefetching**: On fault, decompresses adjacent pages in the same span to reduce future faults
- **Signal-safe decompression**: All decompression state pre-allocated from a bootstrap allocator — no malloc calls in the fault handler path
- **Fine-grained locking**: Per-page spinlocks and per-slab locks minimize contention

## How It Works

1. **Allocation**: smash replaces malloc/free via [alloc8](https://github.com/emeryberger/alloc8) interposition. All slab data pages come from a single large virtual memory reservation (VmRegion).

2. **Access tracking**: A background compressor thread periodically sets active pages to read-only (`mprotect PROT_READ`). Write faults mark pages as "accessed"; pages without writes across multiple intervals are considered cold.

3. **Compression**: Cold pages are compressed (LZ4, zstd, or zstd+dictionary depending on coldness duration) and their physical backing is released. Compressed data is stored in a separate region.

4. **Decompression**: When the application accesses a compressed page, a SIGSEGV/SIGBUS handler recommits the page, decompresses the data, and resumes execution transparently.

## Building

### Prerequisites

- C++20 compiler (Clang 14+ or GCC 12+)
- CMake 3.15+
- [alloc8](https://github.com/emeryberger/alloc8) source as a sibling directory (or specify `-DALLOC8_DIR=...`)

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

This produces `libsmash.dylib` (macOS) or `libsmash.so` (Linux).

### Build with benchmarks

```bash
cmake .. -DSMASH_BUILD_BENCH=ON
make -j$(nproc)
```

## Usage

### macOS

```bash
DYLD_INSERT_LIBRARIES=./build/libsmash.dylib DYLD_FORCE_FLAT_NAMESPACE=1 ./your_application
```

### Linux

```bash
LD_PRELOAD=./build/libsmash.so ./your_application
```

### Optional API

Applications can provide hints for better compression behavior:

```c
#include <smash/smash.h>

smash_hint_cold(ptr, size);   // Suggest region for immediate compression
smash_hint_hot(ptr, size);    // Suggest region should stay decompressed

SmashStats stats;
smash_get_stats(&stats);      // Query compression statistics
```

## Testing

```bash
cd build
ctest --output-on-failure
```

12 tests covering:

| Test | What it verifies |
|------|-----------------|
| `test_bootstrap` | Bootstrap bump allocator |
| `test_size_classes` | Size class mapping and ordering |
| `test_span` | Bitmap-based span allocation |
| `test_slab` | Per-class slab management |
| `test_vm_region` | Virtual memory reservation and page states |
| `test_compression` | LZ4 compress/decompress roundtrip, access tracking |
| `test_integration` | Full SmashHeap malloc/free/memalign |
| `test_interpose` | malloc interposition via DYLD_INSERT |
| `test_dictionary` | Dictionary training, ratio improvement, fallback |
| `test_prefetch` | Adjacent page prefetch, span boundary clipping |
| `test_contention` | 8-thread concurrent alloc/free stress test |
| `test_fault_cycle` | Real SIGSEGV → decompress → verify data integrity |

## Benchmarks

```bash
cd build

# Compression ratio comparison: LZ4 vs zstd vs zstd+dict
./bench/bench_compression

# Malloc/free throughput (ops/sec)
./bench/bench_throughput

# Alloc/free latency percentiles (p50/p99/p999)
./bench/bench_latency

# RSS reduction over time
./bench/bench_rss
```

## Architecture

```
┌─────────────────────────────────────────────┐
│              Application                     │
│         malloc() / free()                    │
├─────────────────────────────────────────────┤
│  alloc8 interposition layer                  │
├─────────────────────────────────────────────┤
│  SmashHeap                                   │
│  ┌──────────┐ ┌──────────┐ ┌──────────────┐ │
│  │ Thread   │ │ Slab[36] │ │ LargeAlloc   │ │
│  │ Cache    │→│ (per-sc) │ │ (>16KB)      │ │
│  └──────────┘ └────┬─────┘ └──────────────┘ │
│                    │                         │
│  ┌─────────────────▼───────────────────────┐ │
│  │           VmRegion                       │ │
│  │  (single contiguous virtual reservation) │ │
│  └─────────────────┬───────────────────────┘ │
│                    │                         │
│  ┌────────────┐ ┌──▼──────────┐ ┌─────────┐ │
│  │ PageState  │ │ Compressor  │ │ Fault   │ │
│  │ Table      │ │ Thread      │ │ Handler │ │
│  └────────────┘ └──┬──────────┘ └────┬────┘ │
│                    │                 │       │
│  ┌─────────────────▼─────────────────▼─────┐ │
│  │         CompressEngine                   │ │
│  │    LZ4 │ zstd │ zstd+dict              │ │
│  └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

## Configuration

Key tuning constants in `include/smash/config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `kCompressIntervalMs` | 1000 | Compression scan interval (ms) |
| `kColdTicks` | 2 | Ticks without access → compress with LZ4 |
| `kVeryColdTicks` | 60 | Ticks → escalate to zstd/zstd+dict |
| `kMinCompressRatio` | 0.75 | Only keep compressed if < 75% of original |
| `kPrefetchWindow` | 2 | Pages prefetched in each direction on fault |
| `kDictTrainSamples` | 16 | Pages collected before dictionary training |
| `kNumClasses` | 36 | Size classes (16B to 16KB) |

## License

TBD
