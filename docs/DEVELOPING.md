# Developing and benchmarking smash

## Building

```bash
cmake -B build && cmake --build build -j
```

This produces `build/libsmash.so` (Linux) or `build/libsmash.dylib` (macOS), and `build/smash-idled` on Linux. The first configure fetches [alloc8](https://github.com/emeryberger/alloc8), zstd and LZ4 with CMake `FetchContent`. To build against a local alloc8 checkout (offline builds, alloc8 development):

```bash
cmake -B build -DALLOC8_DIR=/path/to/alloc8
```

Never build smash or its dependencies with `-march=native`: the library must run on any machine of the target architecture, and CI checks for it.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

The suite covers the allocator pieces (size classes, spans, slabs, the virtual-memory region) and the compressor end to end. The end-to-end tests run the real library under `LD_PRELOAD` / `DYLD_INSERT_LIBRARIES` with a live compressor: allocation, cold detection, compression, fault, decompression and byte-exact verification, in both full and large-only mode, plus real syscalls (`read`, `writev`) on compressed buffers.

CI ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) runs the suite on `ubuntu-latest` and `macos-latest` for every push to master and every pull request, plus two quick regression benchmarks (`bench_rss`, `bench_sqlite`). A second workflow ([`paper-bench.yml`](../.github/workflows/paper-bench.yml)) runs the five paper workloads and fails if any falls below its regression floor.

## Benchmarks

```bash
cmake -B build -DSMASH_BUILD_BENCH=ON && cmake --build build -j
```

`SMASH_BUILD_BENCH=ON` has two sub-options:

| Option | Default | What it gates |
|--------|---------|---------------|
| `SMASH_BUILD_BENCH_DEPS` | `ON` | Build Redis, memcached and RocksDB from source (`make bench_deps`, below). |
| `SMASH_BUILD_BENCH_ALLOCATORS` | `ON` | Build the allocator-comparison benchmarks (mimalloc, jemalloc, tcmalloc, Hoard, Mesh, DieHard, DieHarder). Adds significant build time. |

For only smash's own benchmarks (`bench_rss`, `bench_sqlite`, `bench_throughput`, `bench_compression`, `bench_alloc_speed`, …), as CI does:

```bash
cmake -B build -DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=OFF -DSMASH_BUILD_BENCH_ALLOCATORS=OFF
```

### Micro-benchmarks

```bash
cd build
./bench/bench_rss            # RSS over time for a cooling heap
./bench/bench_alloc_speed    # threadtest, larson, linux-scalability (use with LD_PRELOAD)
./bench/bench_throughput     # malloc/free throughput
./bench/bench_latency        # malloc/free latency percentiles
./bench/bench_compression    # LZ4 vs zstd vs zstd+dictionary
./bench/bench_algo_compare   # WKdm vs LZ4 vs zstd
```

### Applications (Redis, memcached, RocksDB)

The application benchmarks need these built from source so they use the system `malloc` (Redis otherwise bundles jemalloc, which smash cannot see into):

```bash
cmake -B build -DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=ON
cmake --build build -j && cmake --build build --target bench_deps
```

This builds Redis 8.0.2 with `MALLOC=libc`, memcached 1.6.34 (needs libevent) and RocksDB 9.8.4. Then:

```bash
cd build
bash bench/bench_redis.sh [--quick]       # SET, cool, GET
bash bench/bench_memcached.sh [--quick]   # fill, cool, serve, cold re-access
bash bench/bench_rocksdb.sh [--quick]     # block cache with hot/cold access
```

### Paper experiments

```bash
cd build
python3 ../bench/run_paper_experiments.py --runs 3                 # everything
python3 ../bench/run_paper_experiments.py --quick --runs 1         # smoke test
python3 ../bench/run_paper_experiments.py --compress-only-only --runs 3 \
    --apps sqlite,rocksdb,memcached,redis,redis_ext                 # the CI paper gate
```

Results go to `paper_results/`. `bench/plot_results.py` renders the RSS, AUC and throughput charts from a results JSON. `bench/check_paper_regression.py` applies the CI floors to a results file. `bench/verify_paper_claims.py` checks measured reductions against the paper's per-app figures: a shortfall below a hard floor fails, a shortfall below the published number only warns.

Each workload fills, cools with the program idle, then serves queries on a hot 5% of the data. The harness sets `SMASH_COLD_TIMEOUT_SEC=1` so runs stay short. Reported metrics:
- RSS reduction: peak to minimum RSS during cooling.
- AUC: RSS integrated over the serve phase.
- Throughput during serve.

The paper's numbers were measured on an AMD EPYC 9R14 with 192 vCPUs and 1.5 TB of RAM (Amazon Linux 2023, kernel 6.12), with seven workloads including Redis builds patched to cooperate with smash: 73% (SQLite), 84% (RocksDB), 88% (memcached), 58% (Redis), 69% (Redis, half the keys deleted), and 79% for the patched Redis with deletions.

### Competing allocators as preloadable libraries

To compare against other allocators on an unmodified program, build them as shared libraries:

```bash
cmake -B build -DBUILD_ALL_ALLOCATORS=ON     # or -DBUILD_MIMALLOC=ON, etc.
cmake --build build --target allocators      # → build/allocators/lib/
```

| Option | Builds | Source |
|--------|--------|--------|
| `BUILD_MIMALLOC` | `libmimalloc` | microsoft/mimalloc, tag `MIMALLOC_TAG` (default `v2.1.7`) |
| `BUILD_JEMALLOC` | `libjemalloc` | jemalloc/jemalloc, tag `JEMALLOC_TAG` (default `5.3.0`) |
| `BUILD_TCMALLOC` | `libtcmalloc`, `libtcmalloc_minimal` | gperftools, tag `GPERFTOOLS_TAG` (default `gperftools-2.16`) |
| `BUILD_HOARD` | `libhoard` | emeryberger/Hoard, tag `HOARD_TAG`; `-DHOARD_DIR=…` for a local checkout |

On Linux all four interpose under `LD_PRELOAD`. On macOS only mimalloc and Hoard interpose under `DYLD_INSERT_LIBRARIES`. jemalloc and gperftools register their malloc zone lazily from their own first `malloc`, which never runs under the two-level namespace.

## Architecture

```
┌─────────────────────────────────────────────┐
│  Application: malloc() / free()             │
├─────────────────────────────────────────────┤
│  alloc8 interposition                       │
├─────────────────────────────────────────────┤
│  SmashHeap                                  │
│   Thread cache → Slabs (≤16 KB, per size    │
│   class and call-site arena) · Large allocs │
│                    │                        │
│   VmRegion: one contiguous reservation      │
│                    │                        │
│   PageState table · Compressor thread ·     │
│   Fault handler (signals or userfaultfd)    │
│                    │                        │
│   CompressEngine (zstd-1) → CompressStore   │
└─────────────────────────────────────────────┘
```

- **Placement.** A hash of the caller's return address routes each call site to an arena, so a page holds objects from one place in the program. Temperature-aware placement (TAP) keeps new objects off cold pages.
- **Compressor tick (1 s).** Phase 1 records which pages were accessed. Phase B releases the memory of compressed pages that stayed unwritten through the deferred-reclaim window. Phase 2 compresses pages that are cold and pass the cost model. Phase 3 write-protects pages to track the next interval's writes. Work is spread over an adaptive pool of workers, `N = ⌈λ/μ⌉` with a CPU-pressure cap.
- **Cost model.** Statistics are kept per (arena, size class): observed compression ratio, compression cost and how often pages come back. Compression proceeds when the bytes saved per microsecond of CPU clear a threshold.
- **Faults.** Touching a compressed page traps (`SIGSEGV`/`SIGBUS`, or a `userfaultfd` event on Linux 6.8+). The handler restores the page, plus up to two neighbors in each direction within the same span, and the access resumes.
- **Guards.** The compressor budgets new memory mappings against `vm.max_map_count` and backs off as the process nears it.

See [CONFIGURATION.md](CONFIGURATION.md) for every knob.
