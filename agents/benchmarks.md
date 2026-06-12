# Benchmarks — Full Procedures

## Quick Reference: Complete Benchmark Procedure

| Task | Command | Notes |
|------|---------|-------|
| Full paper experiments | `python3 bench/run_paper_experiments.py --runs 3` | From `build/` dir; writes `paper_results/` |
| Compress-only comparison | `python3 bench/run_paper_experiments.py --compress-only-only --runs 3` | 3 configs per app |
| Ablation study | `python3 bench/run_paper_experiments.py --ablation-only --runs 3` | 9 smash variants |
| Quick CI regression | `python3 bench/run_quick_ci.py` | bench_rss + bench_sqlite --quick |
| Paper claims verification | `python3 bench/verify_paper_claims.py` | In-process apps, both modes |
| Allocation speed | `bash bench/run_alloc_speed.sh` | Throughput comparison |
| Standalone RSS comparison | `./bench/bench_rss_standalone` | Direct RSS measurement |
| Figure generation | `python3 bench/plot_results.py <results.json>` | From paper_results/ JSON |
| Key env var for all runners | `SMASH_COLD_TIMEOUT_SEC=1` | Speed up cooling for faster bench cycles |

## Prerequisites

```bash
# Build with benchmarks enabled
cd build
cmake .. -DSMASH_BUILD_BENCH=ON && make -j$(nproc)

# External tool dependencies
brew install memcached redis rocksdb
# Allocator compare also needs: mimalloc, jemalloc, tcmalloc, hoard (built via FetchContent/find_library)
```

`SMASH_BUILD_BENCH=ON` is the master switch. Two sub-flags gate heavy chunks of the bench tree, both default `ON`:

| Flag | Gates |
|------|-------|
| `SMASH_BUILD_BENCH_DEPS` | The `bench_deps` target (Redis, memcached, RocksDB built from source via ExternalProject_Add). |
| `SMASH_BUILD_BENCH_ALLOCATORS` | The allocator-comparison block — mimalloc + jemalloc + tcmalloc + hoard + mesh + diehard + dieharder targets, plus `bench_allocator_compare.py.in`. Pulls in tcmalloc via Bazel (when `bazel`/`bazelisk` is on PATH and glibc >= 2.35) which uses `bench/tcmalloc_patch_build.cmake` to inject a `cc_binary(libtcmalloc_preload.so)` rule into the upstream `//tcmalloc:BUILD` file. |

Paper experiments need both `=ON`. CI regression runs (`.github/workflows/ci.yml`) set both `=OFF` because the quick benches (`bench_rss`, `bench_sqlite`) don't need any of those allocators or external services, and skipping the heavy paths cuts CI build time from ~10 min to ~3 min.

## Unified Experiment Runner (ablation + compress-only)

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

## Verifying Paper Claims (`bench/verify_paper_claims.py`)

`run_quick_ci.py` is a regression tripwire; `verify_paper_claims.py` is the explicit "do we still match the paper?" harness. It computes serve/cool-phase RSS reduction (`1 - min_rss/peak_rss`) and checks each workload against the paper's per-app figure with a two-tier scheme: a conservative hard floor fails the run; a shortfall vs the published number only WARNs (a shortfall usually means the bench profile undershoots the full paper workload — the WARN points to `run_paper_experiments.py` for the full-dataset re-check — not a hardware gap). Prints `BEATS paper` when measured >= the claim.

- **In-process apps** (`rss`, `sqlite`, `rocksdb`) run their bench binary directly, in **both** full and large-only mode. Large-only on these is a *no-regression* check, not a paper-reduction check: their allocations are < 1 MiB so large-only passes them through to the system allocator (the large-only compression mechanism is proven by `test_large_only_compression`). This is the default app set.
- **External-service apps** (`memcached`, `redis`, `redis_ext`, `redis_patched`) are full-mode only and slower; they're driven through `run_paper_experiments.py` (which owns the server + protocol-client lifecycle) and read back from the JSON it writes. Opt in via `--apps`.

Needs `-DSMASH_BUILD_BENCH=ON`. The build dir is autodetected from `./build*` (newest libsmash with a `bench/` tree); override with `--build-dir` or `$BUILD_DIR`. Examples:
```
python3 bench/verify_paper_claims.py                      # in-process apps, both modes
python3 bench/verify_paper_claims.py --apps redis,memcached   # external services
```
Measured on the EPYC 9R14 (full datasets): memcached 86%, rocksdb 76%, sqlite 64%, redis 54%, redis-ext 64%, redis-patched 55%, redis-ext-patched 77% — five of seven beat the paper, two match.

## Application Benchmark Shell Scripts

Individual app benchmarks with detailed output (A/B comparison tables):

```bash
cd build

# RocksDB (compares baseline, smash, rocksdb-lz4, rocksdb-zstd, smash+lz4)
bash bench/bench_rocksdb.sh [--quick]

# Memcached (fill -> cool -> serve -> cold re-access)
bash bench/bench_memcached.sh [--quick]

# Redis
bash bench/bench_redis.sh [--quick]

# Multi-allocator comparison on Redis/Memcached
bash bench/bench_redis_alloc.sh
bash bench/bench_memcached_alloc.sh
```

## Allocator Substrate Comparison (RQ5)

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

## Algorithm Comparison (RQ3)

```bash
cd build
./bench/bench_algo_compare    # Compression ratios + throughput across LZ4/zstd/WKdm
```

## In-Process Benchmarks (C++)

```bash
cd build
./bench/bench_sqlite [--quick]    # SQLite in-memory DB benchmark
./bench/bench_rocksdb [--quick]   # RocksDB block cache benchmark
```

## Generating Figures

```bash
cd paper/figures
python3 plot_all.py              # Main figures (rss_reduction, ablation, algo_compare, etc.)
python3 plot_rss_timeline.py     # RSS over time (Figure 7)
python3 plot_cdf.py              # Cold-access latency CDF (Figure 8)
```

## Building the Paper

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

**Standard workload (SET -> cool -> GET):**
| Config | Fill RSS | Min RSS | Reduction | AUC |
|--------|----------|---------|-----------|-----|
| jemalloc (default) | 333 MB | 332 MB | 0.4% | 6651 MB*s |
| jemalloc (bg disabled) | 334 MB | 333 MB | 0.4% | 6672 MB*s |
| **Smash (bg disabled)** | 382 MB | 204 MB | **47%** | **4388 MB*s** |

**Extended workload (SET -> DELETE 50% -> cool -> GET):**
| Config | Fill RSS | Min RSS | Reduction | AUC |
|--------|----------|---------|-----------|-----|
| jemalloc (bg disabled) | 333 MB | 331 MB | 0.7% | 6623 MB*s |
| Smash (bg disabled) | 386 MB | 637 MB | **-65%** | 12750 MB*s |

Key findings:
- **Disabling background tasks has no effect on jemalloc** (RSS, AUC identical)
- **Standard workload: Smash achieves 47% RSS reduction and 34% lower AUC**
- **Extended workload: Smash shows NEGATIVE benefit** (-65% RSS, +93% AUC) because DELETE operations cause decompression, and the fragmented pages don't re-compress well

Without these flags, Redis's background tasks keep pages warm and Smash cannot compress them effectively.

## Benchmark Result Provenance

Every results JSON written by `bench/run_paper_experiments.py` (`ablation_results.json`, `compress_only_results.json`) carries:

- **Top-level `_sessions[]`** appended per runner invocation: timestamp_utc, runs_requested, quick flag, apps list, `system_info` (hostname, platform, CPU, cores, mem_gib, page_size, tool versions for cmake/gcc/clang/redis-server/memcached), `smash_env_at_start` (snapshot of all `SMASH_*` env vars), and `bench_params` (the actual keys/value_size/num_clients/cool_sec/server_flags used by each `run_*` function).
- **Per-(app, config) `provenance`**: `cmake_flags`, `smash_env`, `source_hash` (SHA-256 of `src/` + `include/` + top-level `CMakeLists.txt`; catches uncommitted edits), `libsmash_sha256` and `libsmash_mtime`, `git_head` and `git_dirty`.

When `git_head` is `null` (e.g., directory populated via rsync), `source_hash` is the authoritative "what code was measured" value. Helpers live in the runner: `collect_system_info()`, `collect_source_hash()`, `collect_git_info()`, `collect_smash_env()`, `build_provenance()`.
