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

Smash's mprotect-based monitoring (PROT_READ) and compression (PROT_NONE) can conflict with kernel syscalls that access userspace buffers. The kernel returns EFAULT instead of triggering SIGSEGV.

### DYLD interposition limitations (macOS)

`__DATA,__interpose` only intercepts **cross-dylib** GOT calls. Intra-libSystem calls are invisible:
- `fread()`/`fgetc()` → internal `read()`: NOT intercepted (same dylib)
- `getc_unlocked` is a macro (`__sgetc`) inlined into callers, calls `__srget` for refills
- C++ iostream (`std::cin`) → `getc_unlocked` → `__srget` → `read()` (all intra-libSystem)
- Direct `read()` from application code: IS intercepted (cross-dylib)

### Solutions implemented in `smash_heap.cpp`

1. **Syscall interposition**: Interpose `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `recv`, `send`, `recvfrom`, `sendto`, `recvmsg`, `sendmsg`, `poll`, `kevent` (macOS), `epoll_wait`, `epoll_pwait` (Linux) — warm and pin buffer pages before calling real syscall
2. **Buffered I/O interposition** (macOS): Interpose `fread`, `fgets`, `fgetc`, `getc`, `fwrite`, `fflush` — warm and pin both user buffer AND the FILE's internal buffer (`stream->_bf._base`)
3. **stdio buffer pinning** (`compressor_thread.h`): `pinStdioBuffers()` permanently pins stdin/stdout/stderr FILE struct + buffer pages at first compressor tick. This covers the intra-libSystem blind spot for standard streams.

### Platform-specific interposition patterns

**macOS**: Do NOT use `dlsym(RTLD_NEXT)` — it returns the wrapper itself. Instead read the `.original` field from the interpose struct:
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

**Linux**: Use `dlsym(RTLD_NEXT)` with lazy resolution in `linux_syscall_wrappers.cpp`. For versioned glibc symbols (e.g., `epoll_wait@GLIBC_2.3.2` used by libevent), create aliased wrapper functions with `.symver` directives and export both versions in `smash_version_script.map`:
```cpp
// Wrapper for GLIBC_2.3.2 version
SMASH_VISIBLE int epoll_wait_232(...) { return epoll_wait(...); }
__asm__(".symver epoll_wait_232,epoll_wait@GLIBC_2.3.2");
```

### Page pinning (`syscall_compat.h`)

`g_page_pins` is a per-page atomic counter. Pages with pin count > 0 are skipped by both Phase 2 (compression) and Phase 3 (monitoring). Used by syscall wrappers to protect kernel buffers during blocking calls.

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
- `SMASH_LARGE_ONLY=1`: Large-only mode — small allocations (≤16KB) pass through to system malloc
- `SMASH_MODE=compress_only`: Compress-only mode — track pages without replacing malloc
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
