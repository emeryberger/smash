# smash environment variables: inventory and classification

Every `SMASH_*` variable that libsmash reads (`grep -rn 'getenv\|envOrProfile' src include`; 2026-11: 112 distinct names, including `SMASH_PROFILE`, `SMASH_REPORT`, `SMASH_METRICS_FILE` and `SMASH_METRICS_INTERVAL_SEC` added with the profile and report work). Each is in one of three classes:

- **User-facing**: the README shows these. They are what an operator needs for a first run and for running smash in production.
- **Expert**: tuning and compatibility knobs with a real use case. They are documented in [docs/CONFIGURATION.md](../docs/CONFIGURATION.md). Normally a profile sets them, or they are left alone.
- **Debug / internal**: diagnostics, ablation switches and experiments. They exist for development, and there is no promise they will stay or keep their meaning.

All variables are read once, at startup or at first use, and never on the fault path (see the `warmupEnvStatics` / `cfg_` pattern in `compressor_thread.h`). `SMASH_PROFILE_FILE*` (bucket-statistics persistence) has nothing to do with `SMASH_PROFILE`.

## SMASH_PROFILE

`SMASH_PROFILE=latency|balanced|memory` supplies defaults for a few expert variables. Two rules apply:

- **Explicit wins.** A variable that is set in the environment always overrides the profile, because readers use `envOrProfile()` (config.h).
- **`balanced` is today's behaviour.** `balanced`, an unset `SMASH_PROFILE` and any unknown value (which prints a warning at start) all supply nothing, so the built-in defaults apply unchanged.

| Variable | latency | balanced (= built-in) | memory |
|---|---|---|---|
| `SMASH_COLD_TIMEOUT_SEC` | 30 | 10 | 2 |
| `SMASH_COMPRESSOR_IDLE` | 1 (Linux) | (off) | (off) |
| `SMASH_PROMPT_TRIM_MB` | (off) | (off) | 8 |
| `SMASH_EMPTY_RESIDENT_MB` | (built-in) | RAM/32, clamped to 64–1024 | 64 |

**A profile's cold timeout behaves like the built-in default.** An explicit `SMASH_COLD_TIMEOUT_SEC` (or `SMASH_COLD_TICKS`) also sets `ROIConfig::cold_ticks_overridden`, and `CompressionROI::shouldCompress` then skips the ROI gate entirely. A profile only moves `cold_ticks_floor` and leaves the gate on, as the built-in 10 s default does. This is the only behavioural difference between `SMASH_PROFILE=memory` and setting its variables by hand.

### Why these values

- **Cold timeout.** config.h (`kColdTicksDefault`) explains the 10 s default: shorter floors compress the read-only working set, because the mprotect path is blind to reads. *memory* uses 2 s, the value the README gives for "see it act sooner". The published results (README, `run_paper_experiments.py`) ran with 1 s and kept throughput at 0.92–1.03× of glibc, so 2 s is inside that measured envelope. *latency* uses 30 s: fewer pages that are still being read get compressed, so fewer decompression faults land on the application's threads.
- **Compressor SCHED_IDLE (latency).** The `maybeIdleSched()` comment records that on Redis the main thread's remaining tail latency was timer preemption by the compressor. *latency* lets the compressor use only otherwise-idle CPU, at the cost of compressing later on a saturated machine.
- **Prompt trim (memory).** The `promptTrimBytes()` comment records that 1 and 8 MB cut SQLite's close peak from about 620 to 520 MB, but cost memcached about 40% throughput in 1 run of 4. *memory* accepts that trade. The other profiles keep it off, as the built-in default does.
- **Resident empty spans (memory).** `Slab::residentEmptyMaxBytesInit()` keeps up to RAM/32 (capped at 1 GiB) of fully empty spans resident for reuse. On a large host that is up to 1 GiB of RSS holding no data. *memory* uses 64 MB, the lower clamp of the built-in formula (what any host with 2 GiB of RAM or less already gets). The comment there records the cost on a 260 MB free/re-allocate batch loop: 2.5 s with a 32 MiB cap versus 0.75 s with 512 MiB, and peak RSS unchanged.
- **The ROI threshold is not in any profile.** `AlgoProfile::computeROI` divides the benefit (`page_size · ratio_255 · cold_ticks / 255`) by the calibrated codec cost per page. With the calibrated zstd-1 speeds it clears even 4096 within a few ticks for any page that passes the 0.75 ratio gate, so the cold floor is what binds and moving `SMASH_ROI_THRESHOLD` has little effect. Measured with the calibration code (`tools/roi_probe.cpp`: `CompressEngine::init` + `ROIConfig::init`, then `computeROI` over cold ticks), 3 runs per machine. On cloudnew (EPYC 9R14, 24-core taskset slice) the fast tier calibrated at 3690–3757 / 1612–1638 MB/s compress (high/low compressibility). There a page that saves 25% reaches ROI ≥ 1024 at 2 ticks and ≥ 4096 at 8 ticks; a page that saves 50% reaches 1024 at 1 tick and 4096 at 4. On an Apple M-series laptop (5868–10349 / 1289–2587 MB/s) a page that saves 25% reaches 1024 at 1–2 ticks and 4096 at 4–7. Every one of these is below the 10 s floor, so the floor is what binds and a profile ROI value would change little; it is left out.
- **Left alone on purpose.** `SMASH_NO_MONITOR` stays off in every profile: without Phase 3 the compressor cannot tell written pages from idle ones, which means more decompression faults, the opposite of what *latency* wants. `SMASH_COOLING_CLOSE_TICKS` already derives from the cold timeout (`getColdTicks()/2`), so every profile moves it implicitly. `SMASH_SHADOW_CAP_DIV` keeps 8, because the comment there records that 1/16 left SQLite's cool phase half-compressed. `SMASH_DEFER_MADVISE`, the VMA guards and the other safety mechanisms are never touched by a profile.

Dev measurements of the profiles (cloudnew, not for results tables) are in [the last section](#dev-measurements-of-the-profiles).

## User-facing

| Variable | Meaning |
|---|---|
| `SMASH_PROFILE` | `latency` / `balanced` / `memory` (above). |
| `SMASH_LARGE_ONLY` | `1`: only allocations over 16 KiB go through smash. For runtimes with their own small-object allocator. |
| `SMASH_COLD_TIMEOUT_SEC` | Idle seconds before a page may be compressed (default 10). |
| `SMASH_REPORT` | `1` (stderr) or a path (`%p` becomes the pid; appended): a summary at normal exit. |
| `SMASH_METRICS_FILE` | Path (`%p` becomes the pid): Prometheus text metrics, rewritten atomically. |
| `SMASH_METRICS_INTERVAL_SEC` | Metrics rewrite interval in seconds (default 10, minimum 1). |
| `SMASH_MODE` | `compress_only`: don't replace malloc, just compress heap pages. |
| `SMASH_TRACK_EXTERNAL` | `1`: also compress anonymous memory the program mmaps itself. |
| `SMASH_STATS` | `1`: one stats line at exit (also gates the TBB interposition line). |
| `SMASH_STATS_SIGNAL` | `1`: install the SIGUSR2 stats handler (`smash stats <pid>`). Off by default because hosts such as MariaDB use SIGUSR2. |
| `SMASH_BANNER` | `1`: print a line when the library loads. |

## Expert (docs/CONFIGURATION.md)

| Variable | Area | Meaning |
|---|---|---|
| `SMASH_LARGE_ONLY_THRESHOLD` | mode | Small/large cutoff in large-only mode (≤ 16384). |
| `SMASH_COLD_TICKS` | when | Cold floor in ticks (used when `SMASH_COLD_TIMEOUT_SEC` is unset). |
| `SMASH_ROI_THRESHOLD` | when | ROI cutoff (default 1024; see above for why it rarely binds). |
| `SMASH_MIN_COMPRESS_RATIO` | when | Keep a blob only below this fraction of the page (0.75). |
| `SMASH_COOLING_CLOSE_TICKS` | placement | Cooling-page closing threshold (default cold/2, 0 off). |
| `SMASH_AGE_ORDER` | placement | Placement policy (3 = TAP default, 0 off; 1/2/4 experimental). |
| `SMASH_EMPTY_RESIDENT_MB` | reclaim | Resident empty-span budget (RAM/32, 64 MB–1 GB; profile-covered). |
| `SMASH_NUM_ARENAS` | layout | Call-site arena count (power of 2). |
| `SMASH_VM_GIB` | layout | Size of the virtual reservation (default 16 GiB). |
| `SMASH_UFFD` | tracking | `0` off, `1` on and report why it is unavailable (default auto). |
| `SMASH_READ_CHECK_TICKS` | tracking | Read-check window without uffd/idled (1–60, default 1). |
| `SMASH_IDLED_SOCK` | tracking | smash-idled socket path. |
| `SMASH_IDLE_READ_TRACK` | tracking | `0`: don't connect to smash-idled. |
| `SMASH_DEFERRED_RECLAIM` | reclaim | `0`: reclaim immediately (not recommended: deadlock risk). |
| `SMASH_DEFERRED_RECLAIM_DELAY` | reclaim | Ticks a shadow page keeps its frame (default 1). |
| `SMASH_SHADOW_CAP_DIV` | reclaim | Shadow-blob cap as 1/N of the committed heap (8; 0 off). |
| `SMASH_DEFER_MADVISE` / `SMASH_DEFER_MADVISE_TICKS` | reclaim | Deferred MADV_DONTNEED sweeper (default on, 50 ticks). Load-bearing for correctness; do not disable in production. |
| `SMASH_PROMPT_TRIM_MB` | reclaim | Trim the store between ticks once N MB died (off; profile-covered). |
| `SMASH_LARGE_CACHE_MB` | reclaim | LargeAlloc block-cache cap (0 disables). |
| `SMASH_DEFER_PHASES_MS` | compressor | No compression or protection for the first N ms. |
| `SMASH_NO_MONITOR` | compressor | `1`: no Phase 3 protection-based tracking. |
| `SMASH_CPU_PRESSURE_CAP` | compressor | `0`: don't cap workers when the CPU is saturated. |
| `SMASH_COMPRESSOR_IDLE` | compressor | `1`: compressor threads at SCHED_IDLE (Linux; profile-covered). |
| `SMASH_EXIT_DRAIN` | compressor | `1`: decompress everything at exit (old behaviour). |
| `SMASH_VMA_MARGIN` | compressor | Headroom kept below vm.max_map_count (default 8192). |
| `SMASH_EAGER_ZERO` | compat | Zero memory on malloc (for programs that assume zeroed malloc). |
| `SMASH_USE_MACH_EXCEPTIONS` | compat (macOS) | Catch faults through Mach exception ports. |
| `SMASH_MLOCK_STORE` | cgroup | Pin the compressed store (`1` mlock, `2` WILLNEED). |
| `SMASH_CALIBRATE` / `SMASH_CALIBRATION_FILE` | calibration | Force/skip startup calibration; cache it in a file. |
| `SMASH_FAST_{COMP,DECOMP}_MBS_{HI,LO}` | calibration | Override the calibrated fast-tier speeds. |
| `SMASH_PRESSURE_LOG` | diagnostics | Per-tick time/space log (`scripts/pressure_report.py`). |
| `SMASH_DEBUG` | diagnostics | Periodic stats lines while running. |
| `SMASH_BUCKET_STATS` | diagnostics | Per-(arena, size class) dump with the stats line. |
| `SMASH_PROFILE_FILE` / `SMASH_PROFILE_FILE_SAVE` / `SMASH_PROFILE_FILE_RW` | persistence | Persist per-bucket statistics across runs. |
| `SMASH_SPILL` / `SMASH_SPILL_DIR` / `SMASH_SPILL_MAX_MB` / `SMASH_SPILL_PAGEOUT` | spill | File-back very cold blobs (experimental). |

## Debug / internal / ablation

| Variable | Meaning |
|---|---|
| `SMASH_ADAPTIVE_COLD`, `SMASH_ADAPTIVE_COLD_REF_MB` | Scale the cold floor down as residency grows past a reference size (experiment). |
| `SMASH_AGE_DENSE_PCT`, `SMASH_AGE_SPARSE_PCT` | Parameters of the experimental `SMASH_AGE_ORDER` 4 / TAP sparse threshold. |
| `SMASH_ARENA_TRACE` | Trace call-site arena routing. |
| `SMASH_COMMIT_CHUNK_KIB` | Arena commit chunk size sweep (0 = exact). |
| `SMASH_COUNT_FREE`, `SMASH_TRACE_FOREIGN_FREE` | Count/trace frees (disables the inlined free fast path). |
| `SMASH_CPU_ARENA` | Route arenas by CPU instead of call site (opt-in experiment; tests pin 0). |
| `SMASH_DEBUG_RESOLVE` | Verify dlsym resolved the system malloc, not smash's. |
| `SMASH_DEEP_{COMP,DECOMP}_MBS_{HI,LO}`, `SMASH_VERY_COLD_TICKS` | Deep-tier (zstd-9) calibration and threshold; only with `-DSMASH_ENABLE_DEEP_TIER`. |
| `SMASH_UCB`, `SMASH_UCB_VARIANT`, `SMASH_UCB_MIN_PULLS`, `SMASH_UCB_WARMSTART`, `SMASH_UCB_FORCE_DEEP_EVERY` | Bandit tier selection (experiment). |
| `SMASH_DEFER_FREE` | `0`: blocking cross-thread free path (A/B). |
| `SMASH_DICT_TRAIN_SAMPLES` | Dictionary training sample count (dictionaries are net-negative; off). |
| `SMASH_DUMP_CRASH_BT`, `SMASH_SIGTRACE`, `SMASH_MACH_TRACE` | Fault-handler tracing and crash backtraces. |
| `SMASH_FIXAV` | Experimental snapshot ordering + verify (off). |
| `SMASH_LANDMINES`, `SMASH_ZERO_FREE` | Poison / zero freed ranges (memory-safety debugging). |
| `SMASH_MADV_FREE`, `SMASH_NO_DECOMMIT` | Reclaim primitive ablations. |
| `SMASH_MAGAZINE` | Opt-in lock-free magazine transfer cache. |
| `SMASH_MIGRATE_PAGE_PCT`, `SMASH_MIGRATE_PROBE_PCT`, `SMASH_RESTORE_MARK_TICKS` | `smash_migrate_hint` experiment. |
| `SMASH_NO_ATEXIT`, `SMASH_SHUTDOWN_TRACE` | Shutdown debugging. |
| `SMASH_NO_COMPRESSOR`, `SMASH_PASSTHROUGH` | Isolate allocator vs compressor vs merely-loaded. |
| `SMASH_NO_SYSCALL_WRAP`, `SMASH_TRACE_EXEC` | Linux syscall wrapper debugging. |
| `SMASH_NO_THREAD_HOOKS` | Disable thread lifecycle hooks. |
| `SMASH_P2_CHUNK`, `SMASH_PROTECT_CHUNK_PAGES`, `SMASH_PHASE3_SKIP_THRESHOLD` | Batched-mprotect / Phase 3 sweeps. |
| `SMASH_PROFILE_THRASH_BACKOFF`, `SMASH_RECOMPRESS_BACKOFF` | Thrash back-off ablations. |
| `SMASH_PROT_READ_BARRIER` | membarrier after PROT_READ arming (experiment). |
| `SMASH_READ_CHECK` | Toggle the read-check window itself. |
| `SMASH_SAFE_MEMCPY`, `SMASH_SNAPSHOT_VERIFY` | Compressor snapshot safety checks. |
| `SMASH_SOFTDIRTY`, `SMASH_SOFTDIRTY_ROI` | Soft-dirty tracking and the soft-dirty ROI discount. |
| `SMASH_TIME_BUDGET_PCT` | Time-budget bucket gating (experiment). |
| `SMASH_TIME_INIT` | Time heap initialisation. |
| `SMASH_TRACE_MPROTECT_FAIL` | Log failing mprotect calls. |
| `SMASH_UFFD_WP_SCAN` | `0`: uffd without async write-protect scanning. |

## Dev measurements of the profiles

Development numbers only; they belong in no results table. Measured on cloudnew (EPYC 9R14, Amazon Linux 2023, kernel 6.12, uffd tracking) inside a 24-core `taskset` slice, with other agents' jobs on the other cores. The workload was `bench/bench_sqlite --quick --cool 40 --serve 20` (250K rows; fill, 40 s idle, 20 s serving the newest 5%). The 40 s cool phase is longer than every profile's timeout. 5 runs per configuration, randomly interleaved (`shuf`); the table shows the median with the [min–max] range. The driver loops over `glibc` (no preload), `unset` (libsmash, no profile), `SMASH_PROFILE=memory` and `SMASH_PROFILE=latency`, and records each run's `METRIC` lines.

| config | peak RSS MB | RSS after cool MB | cool AUC MB·s | serve AUC MB·s | serve ops/s | cold p99 µs |
|---|---|---|---|---|---|---|
| glibc | 179.9 [179.9–180.0] | 180.1 | 7203 [7203–7204] | 5070 [5062–5082] | 99316 [98721–100068] | 1.9 [1.8–2.0] |
| unset (= balanced) | 198.5 [198.4–198.5] | 62.9 [62.8–63.0] | 4365 [4362–4370] | 3746 [3328–3753] | 94806 [83842–95060] | 17.7 [16.6–18.5] |
| memory | 198.5 [198.4–198.5] | 62.9 [62.8–63.0] | 3269 [3266–3271] | 3337 [3036–3569] | 89750 [83764–94766] | 16.7 [16.5–80.2] |
| latency | 198.4 [198.4–198.5] | 61.8 [61.7–63.0] | 7070 [7066–7073] | 3733 [3722–3753] | 95167 [94937–95280] | 18.2 [17.8–19.6] |

What this run shows:

- *memory* reaches the same floor (about 63 MB) earlier. Its RSS integral over the cool phase is 25% below the default's (3269 vs 4365 MB·s) and 9% below over the serve phase. Serve throughput was 5% lower (median 89750 vs 94806 ops/s) and varied more between runs.
- *latency* compresses nothing until 30 s, so its cool-phase integral is close to glibc's. After that it reaches the same floor, and serve throughput matched the default's (95167 vs 94806).
- This workload cannot show the tail-latency purpose of *latency*. Its serve phase reads only the hot 5%, which no configuration compresses, and its cold reads come after the cool phase in every configuration.

Not measured here: memcached/Redis serving under each profile (the prompt-trim throughput risk recorded in `promptTrimBytes()` came from memcached), and large-only mode.
