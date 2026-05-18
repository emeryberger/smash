# smash

**smash** — a compression-aware memory allocator that transparently compresses cold pages to reduce resident set size (RSS).

## Overview

Smash is a drop-in malloc replacement that monitors page access patterns and compresses pages that haven't been touched recently. When compressed pages are accessed again, a signal handler transparently decompresses them before the application sees the data. Smash reduces physical memory usage for applications with large working sets where significant portions of allocated memory are idle at any given time.

### Key Features

- **Transparent compression**: No application changes required, works via malloc interposition.
- **ROI-driven algorithm selection**: A return-on-investment model picks zstd-1 (fast tier) or zstd-9 (deep tier) per page at compression time, weighing observed compressibility and observed compression cost against the page's accumulated cold time.
- **Per-origin learning**: Compression-ratio and compression-cost statistics are kept per `(arena, size class)`, so call-site arena routing translates directly into more accurate ROI decisions.
- **Adaptive worker pool**: The compressor scales its active worker count each tick using Little's Law (`N = ⌈λ/μ⌉`), so an idle process uses one worker and a bulk-allocation phase uses several.

## How It Works

1. **Allocation**: smash replaces malloc/free via [alloc8](https://github.com/emeryberger/alloc8) interposition. All slab data pages come from a single large virtual memory reservation (VmRegion). A return-address hash routes calls from the same call site into the same arena, producing structurally homogeneous pages.

2. **Access tracking**: A background compressor thread periodically sets active pages to read-only (`mprotect PROT_READ`). Write faults mark pages as "accessed"; pages without writes across multiple intervals are considered cold.

3. **Compression**: Cold pages are compressed with zstd-1 or zstd-9, selected by the ROI model based on the page's `(arena, size class)` observed ratio and the per-tier observed compression cost. Physical backing is released (`MADV_FREE_REUSABLE` on macOS, `MADV_DONTNEED` on Linux); compressed data is stored in a separate sharded region.

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

`SMASH_BUILD_BENCH=ON` enables three groups of benchmark targets, each with its own toggle:

| Option | Default | What it gates |
|--------|---------|---------------|
| `SMASH_BUILD_BENCH_DEPS` | `ON` | Build Redis, memcached, DuckDB, RocksDB from source via `make bench_deps`. See "Build with benchmark dependencies" below. Set `OFF` to skip — the rest of the benchmark targets still build. |
| `SMASH_BUILD_BENCH_ALLOCATORS` | `ON` | Build the allocator-comparison benches (mimalloc, jemalloc, tcmalloc, hoard, mesh, diehard, dieharder) and the `bench_allocator_compare.py` runner. Pulls in tcmalloc / mimalloc via FetchContent + ExternalProject_Add, which adds significant build time and several optional `find_library` probes. Set `OFF` for fast smash-only builds (e.g. CI regression runs). |

To build only the smash-internal benches (`bench_rss`, `bench_sqlite`, `bench_throughput`, `bench_compression`, `bench_algo_compare`, etc.) without external services or competing allocators:

```bash
cmake .. -DSMASH_BUILD_BENCH=ON \
         -DSMASH_BUILD_BENCH_DEPS=OFF \
         -DSMASH_BUILD_BENCH_ALLOCATORS=OFF
make -j$(nproc)
```

This is what the CI regression-spotting workflow uses (see `.github/workflows/ci.yml`).

### Build with benchmark dependencies (Redis, memcached, DuckDB, RocksDB)

For full A/B benchmarking, build the external dependencies from source. This ensures they use system malloc (libc) instead of their default allocators (jemalloc), which is required for Smash to effectively compress their memory.

```bash
cmake .. -DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=ON
make -j$(nproc)
make bench_deps   # Builds Redis, memcached, DuckDB, RocksDB from source
```

**Note**: Building DuckDB from source takes significant time (10-20 minutes). The `bench_deps` target builds:
- Redis 8.0.2 with `MALLOC=libc` (instead of jemalloc)
- memcached 1.6.34 (requires libevent-devel)
- DuckDB 1.2.0 CLI
- RocksDB 9.8.4 static library

## Usage

### macOS

```bash
DYLD_INSERT_LIBRARIES=./build/libsmash.dylib DYLD_FORCE_FLAT_NAMESPACE=1 ./your_application
```

### Linux

```bash
LD_PRELOAD=./build/libsmash.so ./your_application
```

### Large-Only Mode

For applications with their own small-object allocator (e.g., Python 3.13+ uses mimalloc internally), Smash can manage only large allocations while letting the native allocator handle small objects:

```bash
# macOS
SMASH_LARGE_ONLY=1 DYLD_INSERT_LIBRARIES=./build/libsmash.dylib ./your_application

# Linux
SMASH_LARGE_ONLY=1 LD_PRELOAD=./build/libsmash.so ./your_application
```

Allocations <= 16KB pass through to the system allocator; larger allocations go through Smash and are eligible for compression. This avoids interfering with language runtimes that have their own optimized small-object allocators.

### Compress-Only Mode

For applications that use custom allocators (jemalloc, tcmalloc, etc.), Smash can run in compress-only mode where it only monitors and compresses pages without replacing malloc:

```bash
# macOS
SMASH_MODE=compress_only DYLD_INSERT_LIBRARIES=./build/libsmash.dylib ./your_application

# Linux
SMASH_MODE=compress_only LD_PRELOAD=./build/libsmash.so ./your_application
```

This mode tracks all heap pages via `/proc/self/maps` (Linux) or `vm_region` (macOS) and compresses cold regions regardless of which allocator manages them.

### External-Mapping Tracking (`SMASH_TRACK_EXTERNAL=1`)

Standard smash compresses pages within its own `MAP_ANON` arena (where malloc-routed allocations live). Application code that calls `mmap()` / `mach_vm_allocate()` directly bypasses malloc and so escapes the compressor — this is the long pole on Firefox, where SpiderMonkey JS GC arenas, Skia / `mozalloc_aligned` graphics surfaces, and IPC-aligned shared memory all bypass `malloc`.

`SMASH_TRACK_EXTERNAL=1` registers anonymous-writable application-direct mappings with smash so the compressor can see them too:

```bash
# macOS
SMASH_TRACK_EXTERNAL=1 DYLD_INSERT_LIBRARIES=./build/libsmash.dylib ./your_application

# Linux
SMASH_TRACK_EXTERNAL=1 LD_PRELOAD=./build/libsmash.so ./your_application
```

Filter rules: `mmap` is tracked only with `MAP_ANON | PROT_WRITE` (file-backed and read-only mappings are skipped). On macOS `mach_vm_allocate` and `vm_allocate` are tracked when allocated in the current task. The interposers themselves always install (cost: one branch per `mmap` / `mach_vm` call); only the page registration path is gated by the env var.

This is **opt-in** because the registration path has not yet been validated for stability on Firefox-class workloads — see `smash-benchmarks/FIREFOX_STUDY.md` for context.

### Runtime configuration (environment variables)

All runtime behavior is controlled via `SMASH_*` environment variables read once at process start. Tuning constants live in `include/smash/config.h` (see "Configuration" below); env vars are how you override them without rebuilding.

**Modes** — pick at most one, otherwise full mode:

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_MODE=compress_only` | unset | Track and compress pages without replacing malloc — for apps with their own allocator (jemalloc, tcmalloc, …). |
| `SMASH_LARGE_ONLY=1` | unset | Pass small allocations (≤ 16 KB) to system malloc; only large allocations go through smash. Use with language runtimes that ship optimized small-object allocators (Python 3.13+ mimalloc, …). |
| `SMASH_LARGE_ONLY_THRESHOLD=N` | 16384 | Override the small/large cutoff (bytes) when `SMASH_LARGE_ONLY=1`. |
| `SMASH_TRACK_EXTERNAL=1` | unset | Register application-direct `mmap` / `mach_vm_allocate` results so the compressor sees them. Opt-in; see the External-Mapping section above. |

**Compression decision** (ROI model):

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_COLD_TIMEOUT_SEC=N` | 2 | Minimum cold-time floor (seconds) before fast-tier compression considered. CPU-pressure adaptive cap raises this on busy systems. |
| `SMASH_COLD_TICKS=N` | 2 | Override the fast-tier cold-tick threshold directly (alternative to `SMASH_COLD_TIMEOUT_SEC`). |
| `SMASH_VERY_COLD_TICKS=N` | 60 | Cold-tick threshold for the deep-tier (zstd-9) profile. `9999` disables the deep tier entirely (fast tier only). |
| `SMASH_ROI_THRESHOLD=N` | 1024 | Bytes-saved-per-microsecond cutoff for the ROI model. |
| `SMASH_MIN_COMPRESS_RATIO=F` | 0.75 | Reject compressed page unless `comp_size < F × original_size`. |
| `SMASH_RECOMPRESS_BACKOFF=0` | 1 (active) | Disable per-bucket recompression-thrash back-off (ablation switch); phase 2 then ignores `recompress_count_` and bucket EMAs. |

**Tier-selection bandit (opt-in, experimental):**

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_UCB=1` | unset | Replace the calibrated ROI model with a UCB1-Tuned bandit that picks fast vs deep tier per `(arena, size class)` from observed bytes-saved/µs. Default off — the in-process benches don't show enough per-bucket heterogeneity to differentiate it from ROI. |
| `SMASH_UCB_VARIANT=N` | 0 | UCB variant selector (see `compression_roi.h`). |
| `SMASH_UCB_MIN_PULLS=N` | 4 | Force-pulls per arm before the UCB formula kicks in. |
| `SMASH_UCB_WARMSTART=1` | 0 | Seed UCB priors from the calibrated ROI estimates instead of cold-starting. |
| `SMASH_UCB_FORCE_DEEP_EVERY=N` | 0 | Periodically force a deep-tier sample for exploration even when fast-tier dominates. |

**Calibration** — startup compression-throughput benchmarks that feed the ROI model:

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_CALIBRATE=always\|never` | auto | Force or skip startup calibration. |
| `SMASH_CALIBRATION_FILE=path` | unset | Cache calibration results to disk and reload on subsequent runs (much faster startup). |
| `SMASH_FAST_COMP_MBS_HI/LO` | calibrated | Override fast-tier compression-rate estimate (MB/s) for high/low-compressibility pages. |
| `SMASH_FAST_DECOMP_MBS_HI/LO` | calibrated | Override fast-tier decompression-rate estimate. |
| `SMASH_DEEP_COMP_MBS_HI/LO` | calibrated | Override deep-tier (zstd-9) compression-rate estimate. |
| `SMASH_DEEP_DECOMP_MBS_HI/LO` | calibrated | Override deep-tier decompression-rate estimate. |

**Compressor thread / startup behavior:**

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_DEFER_PHASES_MS=N` | 0 | Skip Phase 2 (compress) + Phase 3 (monitor) for the first N ms after start. Useful for workloads that establish IPC channels at startup with buffers in smash-managed pages (Firefox sweet spot is 30000). |
| `SMASH_NO_MONITOR=1` | unset | Disable Phase 3 (PROT_READ access tracking) entirely. Trades cold-detection accuracy for compatibility with code paths that synchronously check page protection. |
| `SMASH_CPU_PRESSURE_CAP=0` | active | Disable the CPU-pressure cap on adaptive worker count. Default (active) caps `N = ⌈λ/μ⌉` so smash doesn't compete with a saturated app for cores. |
| `SMASH_EAGER_ZERO=1` | unset | Memset newly-allocated buffers to zero on the malloc fast path instead of relying on the compressor thread's deferred zero-on-free pass. Trades throughput for correctness with callers that assume malloc returns zeroed memory (technically UB, but widely relied on). |

**Diagnostics / observability:**

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_BANNER=1` | unset | Print a one-line banner at library-load time confirming `DYLD_INSERT_LIBRARIES` / `LD_PRELOAD` actually loaded libsmash. Useful for multi-process apps. |
| `SMASH_STATS=1` | unset | Emit a stats line on every normal process exit (atexit). Inherited across `fork()`, so each child of a multi-process app prints its own line. `_exit()` and `SIGKILL` skip atexit by design. |
| `SMASH_DEBUG=1` | unset | Emit a stats line every Nth tick during the run. Distinct from `SMASH_STATS` (atexit-only) — for watching live activity without chasing PIDs and `SIGUSR2`. |
| `SIGUSR1` / `SIGUSR2` | — | (Not env vars; sending these signals to a smash-loaded process prints stats on demand.) |

**macOS-specific:**

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_USE_MACH_EXCEPTIONS=1` | unset | Install task-level Mach exception ports for `EXC_BAD_ACCESS` with a dedicated handler thread. Lets smash intercept protection faults before they're converted to signals — required for Objective-C runtimes that swallow `SIGSEGV`. |
| `SMASH_MACH_TRACE=1` | unset | One-line stderr trace per Mach exception (only meaningful with `SMASH_USE_MACH_EXCEPTIONS=1`). |

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

14 tests covering:

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
| `test_external_mapping` | `mmap` + `mach_vm_allocate` round-trip through the compressor (under `SMASH_TRACK_EXTERNAL=1`); negative tests confirm file-backed and read-only mappings are not tracked |
| `test_malloc_compression` | End-to-end compression on the malloc/free path: `SIGUSR2` stats line shows `compressed > 0`, then read-back verifies fault-decompress integrity |

The two end-to-end tests run under `DYLD_INSERT_LIBRARIES` (macOS) / `LD_PRELOAD` (Linux) with a live compressor, so they exercise the full Phase 1 → Phase 2 → fault-decompress cycle, not just unit-level invariants.

Continuous integration: `.github/workflows/ci.yml` builds and runs the full ctest suite on `ubuntu-latest` and `macos-latest` for every push to master and every pull request.

## Benchmarks

### Micro-benchmarks

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

# Algorithm comparison: WKdm vs LZ4 vs zstd
./bench/bench_algo_compare
```

### Application Benchmarks

These scripts run A/B comparisons (baseline vs Smash) on real applications:

```bash
cd build

# Redis (SET → cool → GET workload)
bash bench/bench_redis.sh [--quick]

# Memcached (fill → cool → serve → cold re-access)
bash bench/bench_memcached.sh [--quick]

# DuckDB (TPC-H OLAP queries)
bash bench/bench_duckdb.sh [--quick]

# RocksDB (block cache with hot/cold access)
bash bench/bench_rocksdb.sh [--quick]
```

### Paper Experiments

For reproducible research results:

```bash
cd build

# Run all experiments (full — for paper-quality results)
python3 ../bench/run_paper_experiments.py --runs 3

# Quick smoke test
python3 ../bench/run_paper_experiments.py --quick --runs 1

# Results written to paper_results/
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
| `kColdTicksDefault` | 10 | Ticks without access before fast-tier compression considered (override via `SMASH_COLD_TIMEOUT_SEC` or `SMASH_COLD_TICKS`) |
| `kVeryColdTicks` | 60 | Cold-tick threshold for the deep-tier (zstd-9) profile in the ROI model (override via `SMASH_VERY_COLD_TICKS`) |
| `kMinCompressRatio` | 0.75 | Only keep compressed if < 75% of original |
| `kPrefetchWindow` | 2 | Pages prefetched in each direction on fault |
| `kDictTrainSamples` | 0 | Pages before dictionary training (disabled by default) |
| `kNumClasses` | 36 | Size classes (16B to 16KB) |
| `kNumArenas` | 4 | Call-site arenas (must be a power of 2) |
| `kCompressorWorkers` | 2 | Initial compressor worker count |
| `kMaxCompressorWorkers` | 8 | Cap for adaptive worker scaling |
| `kCompressStoreShards` | 8 | Sharded lock count in CompressStore |

## License

TBD
