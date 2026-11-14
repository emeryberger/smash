# Configuring smash

smash needs no configuration: the defaults are the tuned settings. Everything here is for special cases, experiments and diagnosis.

All runtime behavior is controlled by `SMASH_*` environment variables, read once at process start. Compile-time constants live in [`include/smash/config.h`](../include/smash/config.h).

## Modes

Pick at most one; otherwise smash runs in full mode (it is the program's `malloc`).

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_MODE=compress_only` | unset | Track and compress heap pages without replacing `malloc`, for programs with their own allocator (jemalloc, tcmalloc, …). Pages are found through `/proc/self/maps` (Linux) or `vm_region` (macOS). |
| `SMASH_LARGE_ONLY=1` | unset | Pass allocations of 16 KB or less to the system allocator; only larger ones go through smash. For runtimes with an optimized small-object allocator (Python 3.13+ uses mimalloc). |
| `SMASH_LARGE_ONLY_THRESHOLD=N` | 16384 | The small/large cutoff in bytes when `SMASH_LARGE_ONLY=1`. |
| `SMASH_TRACK_EXTERNAL=1` | unset | Also track anonymous, writable memory the program maps itself (`mmap` with `MAP_ANON \| PROT_WRITE`; `mach_vm_allocate`/`vm_allocate` on macOS), so the compressor sees it. File-backed and read-only mappings are never tracked. Opt-in because it has not been validated on Firefox-class workloads, where JS GC arenas and graphics surfaces bypass `malloc`. |

## When pages are compressed

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_COLD_TIMEOUT_SEC=N` | 10 | Seconds a page must go untouched before it can be compressed. The main time/space dial. A CPU-pressure cap can raise it on a busy machine. |
| `SMASH_COLD_TICKS=N` | 10 | The same threshold in compressor ticks (one per second); used when `SMASH_COLD_TIMEOUT_SEC` is unset. |
| `SMASH_ROI_THRESHOLD=N` | 1024 | Bytes saved per microsecond of compression CPU a page group must offer to be compressed. |
| `SMASH_MIN_COMPRESS_RATIO=F` | 0.75 | Keep a compressed page only if it shrinks to less than `F` of its size. |
| `SMASH_RECOMPRESS_BACKOFF=0` | on | Disable the back-off for page groups that keep getting decompressed and recompressed (ablation switch). |

## Tracking and reclaim

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_UFFD=0\|1` | auto | `userfaultfd` tracking is on automatically when the kernel supports it (Linux 6.8+, `UFFDIO_MOVE`). `0` turns it off; `1` also reports why it is unavailable. |
| `SMASH_READ_CHECK_TICKS=N` | 1 | Without `userfaultfd` or `smash-idled`, how many ticks a cold candidate page stays protected to catch reads before it is compressed (1–60). |
| `SMASH_DEFERRED_RECLAIM=0` | on | Release a page's memory as soon as it is compressed, instead of keeping the original until it is confirmed unwritten. Off is not recommended: the immediate path can deadlock with concurrent faults. |
| `SMASH_DEFERRED_RECLAIM_DELAY=N` | 2 | Ticks a compressed page's original is kept before its memory is released. A write in that window discards the compressed copy. |
| `SMASH_AGE_ORDER=N` | 3 | Placement policy. `3` is temperature-aware placement (TAP): never fill a free slot on a cold page. `0` turns placement policy off. `1`, `2` and `4` are experimental alternatives. |
| `SMASH_EMPTY_RESIDENT_MB=N` | RAM/32 (64 MB–1 GB) | How much memory in fully empty spans to keep resident for reuse before returning it to the kernel. |
| `SMASH_NUM_ARENAS=N` | from CPU count | Number of call-site arenas (a power of 2). |

`smash-idled`, built on Linux alongside the library, is a small root helper that gives smash processes exact read tracking from the kernel's idle-page bitmap. smash connects to it automatically. Run it as `sudo build/smash-idled`; it serves only the process that launched it (or the shell behind `sudo`) and that process's descendants. See [`tools/smash_idled.cpp`](../tools/smash_idled.cpp) for its options and safety rules.

## Compressor thread

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_DEFER_PHASES_MS=N` | 0 | Don't compress or protect pages for the first N ms. For programs that set up IPC buffers in heap memory at startup (Firefox: 30000). |
| `SMASH_NO_MONITOR=1` | unset | Never write-protect pages for access tracking. Trades accuracy for compatibility with code that checks page protection synchronously. |
| `SMASH_CPU_PRESSURE_CAP=0` | on | Let the compressor scale up its workers even when the CPU is saturated. |
| `SMASH_NO_COMPRESSOR=1` | unset | Run as a plain allocator with no compression (diagnostic). |
| `SMASH_EAGER_ZERO=1` | unset | Zero memory on `malloc` instead of in the background after `free`. For programs that wrongly assume `malloc` returns zeroed memory. |

## Calibration

At startup smash measures compression speed on this machine to feed its cost model.

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_CALIBRATE=always\|never` | auto | Force or skip startup calibration. |
| `SMASH_CALIBRATION_FILE=path` | unset | Save calibration results and reuse them on later runs (faster startup). |
| `SMASH_FAST_COMP_MBS_HI/LO`, `SMASH_FAST_DECOMP_MBS_HI/LO` | calibrated | Override the measured compression/decompression speed (MB/s) for highly and poorly compressible pages. |

## Diagnostics

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_BANNER=1` | unset | Print a line when smash loads, to confirm the preload took effect (useful for multi-process programs). |
| `SMASH_STATS=1` | unset | Print a stats line at normal process exit. Inherited across `fork()`, so each child prints its own. |
| `SMASH_DEBUG=1` | unset | Print a stats line periodically while running. |
| `SMASH_PRESSURE_LOG=path` | unset | Write a per-tick log of compressed pages, compressed bytes, RSS and compressor CPU. |
| `SIGUSR1` / `SIGUSR2` | — | Signals, not variables: sending either to a smash process prints stats on demand. |

## macOS

| Variable | Default | Effect |
|----------|---------|--------|
| `SMASH_USE_MACH_EXCEPTIONS=1` | unset | Catch protection faults through Mach exception ports before they become signals. Needed for Objective-C runtimes that swallow `SIGSEGV`. |
| `SMASH_MACH_TRACE=1` | unset | Trace each Mach exception to stderr (with `SMASH_USE_MACH_EXCEPTIONS=1`). |

## Experimental: compression tiers

smash compresses every page with **zstd level 1**. The cost model decides only *whether* a page is worth compressing. Other options exist but are off by default:

- **A zstd-9 "deep" tier**, built with `-DSMASH_ENABLE_DEEP_TIER`. On 4 KB pages zstd-9 produces blobs about the same size as zstd-1, because its larger match window needs more input, so it costs CPU with no memory gain. `SMASH_VERY_COLD_TICKS` (default 60) sets when it applies. A UCB bandit can pick between tiers per page group: `SMASH_UCB=1`, with `SMASH_UCB_VARIANT`, `SMASH_UCB_MIN_PULLS`, `SMASH_UCB_WARMSTART` and `SMASH_UCB_FORCE_DEEP_EVERY`. With the deep tier, `SMASH_DEEP_COMP_MBS_HI/LO` and `SMASH_DEEP_DECOMP_MBS_HI/LO` override its calibration.
- **LZ4 as the compressor**, built with `-DSMASH_USE_LZ4` (faster, lower ratio).
- **Dictionary compression** (zstd + trained dictionary), enabled by setting `kDictTrainSamples` above 0.

A page keeps the algorithm it was first compressed with until an access decompresses it.

## Compile-time constants

From [`include/smash/config.h`](../include/smash/config.h):

| Constant | Default | Description |
|----------|---------|-------------|
| `kCompressIntervalMs` | 1000 | Compressor tick interval (ms) |
| `kColdTicksDefault` | 10 | Default cold threshold in ticks |
| `kMinCompressRatio` | 0.75 | Keep a compressed page only below this ratio |
| `kPrefetchWindow` | 2 | Neighboring pages restored in each direction on a fault |
| `kDictTrainSamples` | 0 | Pages sampled before training a dictionary (0 = off) |
| `kNumClasses` | 36 | Small-object size classes (16 B–16 KB) |
| `kMaxArenas` | 128 | Upper bound on call-site arenas |
| `kMaxCompressorWorkers` | 8 | Cap on compressor worker threads |
| `kCompressStoreShards` | 8 | Lock shards in the compressed-page store |
