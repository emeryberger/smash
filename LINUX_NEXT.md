# Linux smash — handoff for the next session

You are a Claude Code instance running on a **Linux ARM64 host** (Ubuntu 24.04 in a Parallels VM on Apple Silicon). Repo at `~/git/smash` mirrors `https://github.com/emeryberger/smash`. This doc is your operating manual: skim it before doing anything, since multiple traps cost the previous sessions hours.

## What's been accomplished

Three merged PRs, one open:

- **PR #11 (merged)** — fixed aarch64 `LD_PRELOAD` malloc binding. `libsmash.so` was emitting `malloc@@GLIBC_2.2.5`, but aarch64 binaries link against `malloc@GLIBC_2.17`, so ld.so silently bound them to libc and smash never saw any traffic. Version script now substitutes the per-arch GLIBC base in CMake.

- **PR #12 (merged)** — `pthread_atfork` compressor restart so each fork()'d child gets its own coordinator + helper threads (postgres backends, daemonized redis, etc.). Plus the Zipfian + 60 s in-connection cool-tail workload in `bench/postgres_shim_build.py` that gets the per-backend MemoryContext to age out so the compressor can see it.

- **PR #13 (open, awaiting CI)** — a thrash-mitigation stack:
  1. Per-page + per-(arena, size_class) recompression tracking with exponential back-off on the cold-tick floor.
  2. Thread-aware CPU-pressure cap on the active worker count (counts our process's threads via `/proc/self/task` on Linux / `task_threads()` on macOS, subtracts known compressor workers, leaves enough cores for the application).
  3. 16× multiplier on the cold-tick floor when the box is genuinely contended (`app_threads >= nproc` AND `loadavg ≥ app_threads + 1`). Bypassed when the user explicitly sets `SMASH_COLD_TIMEOUT_SEC` / `SMASH_COLD_TICKS`.
  4. `bench/postgres_shim_build.py --mode=compare` driver: builds stock + shim postgres side-by-side, runs perf workloads under stock / stock+jemalloc / shim / shim+jemalloc / shim+smash, hard 4× perf-duration timeout fuse, pkill cleanup that catches `smashuser` backends (postgres calls `setsid()` per connection, so `killpg(postmaster_pgid)` misses them).

## State of the repo

Master green on CI (x86_64 ubuntu-latest + macos-latest). No aarch64 Linux CI, so any aarch64-specific issue surfaces only locally.

Useful env vars when running smash interactively (set all three):
- `SMASH_BANNER=1` — `[smash] loaded pid=N ppid=M platform=linux` per process.
- `SMASH_DEBUG=1` — `[smash debug] compressor start` + `[smash stats]` every ~5 s.
- `SMASH_STATS=1` — `[smash stats]` line on every normal `atexit`. Inherits across `fork()`.

PR #13 adds:
- `SMASH_RECOMPRESS_BACKOFF=0` — disable the per-page back-off (ablation).
- `SMASH_CPU_PRESSURE_CAP=0` — disable the worker cap (ablation).
- `SMASH_COLD_TIMEOUT_SEC=N` (or `SMASH_COLD_TICKS=N`) — explicit cold-tick override; also bypasses the floor multiplier.

Three valuable scripts in `bench/`:
- `postgres_shim_build.py` — downloads postgres-16.6 to `/tmp/smash-pg-shim/`, patches `aset.c` to a malloc-passthrough, builds. Default `--mode=shim-smash` runs the cool-tail workload (Zipfian + `pg_sleep(60)` in-connection) and reports per-backend compression.
- `postgres_shim_build.py --mode=compare --runs N --clients K --perf-duration S` — the comparison driver. Builds stock too, runs perf workloads, prints a side-by-side table.
- `run_quick_ci.py` — the regression gate. `bench_rss` ≥ 30 % RSS reduction, `bench_sqlite` ≥ 5 % cooling reduction. CI uses these.

## Headline numbers (aarch64 Ubuntu 24.04, 2-CPU Parallels VM, current `smash-recompress-backoff` branch)

**Cool-tail workload** (default `--mode=shim-smash`):
```
long-lived (samples>5):  committed=7147 pages (28 MB),
                         compressed=7100 pages (28 MB), ratio=99.3%
```

**Perf workload** (`--mode=compare --runs 5 --clients 2 --perf-duration 30`):
```
config           runs    tps_med    tps_min    tps_max    Δ vs stock
stock               5    11182.8     9742.0    11205.9         —
stock+jemalloc      5    11235.4    11097.2    11343.6      +0.5 %
shim                5    10029.6     9722.8    10105.2     -10.3 %
shim+jemalloc       5    10851.7    10699.6    10902.3      -3.0 %
shim+smash          5     8039.4     7506.7    (1 timeout)  -28.1 %
```

`shim+jemalloc` within ~3 % of stock validates the *Reconsidering Custom Memory Allocation* (Berger/Zorn/McKinley, OOPSLA 2002) thesis on this codebase: removing palloc's pooling and dropping in jemalloc nearly recovers the loss.

`shim+smash` still has occasional pgbench timeouts on this 2-CPU VM. **Pending: re-test on a bigger VM.** With more cores the CPU cap is less restrictive (compressor workers + app threads no longer compete for every core), the floor multiplier triggers less often, and the timeout rate should drop.

## Step 1 — confirm baseline state

Before any other work, sanity-check the build:

```sh
cd ~/git/smash
git fetch origin && git checkout master && git pull --ff-only

cd linux-build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DSMASH_BUILD_BENCH=ON \
         -DSMASH_BUILD_BENCH_DEPS=OFF \
         -DSMASH_BUILD_BENCH_ALLOCATORS=OFF
make -j$(nproc)
ctest --output-on-failure   # all 15 must pass

cd ..
python3 bench/run_quick_ci.py --build-dir linux-build
# must say:
#   [PASS] bench_rss t=10s reduction ≥ 30%
#   [PASS] bench_sqlite cooling reduction ≥ 5%
```

If `bench_rss` shows committed=1 instead of committed=16384, smash's malloc isn't being intercepted. That's a regression — see `memory/project_aarch64_ld_preload_glibc_versioning.md` for the diagnostic recipe (PR #11 fixed this once already).

## Step 2 — exercise the postgres validation

The cool-tail workload is the headline result for the paper:

```sh
unset PROFILE   # the postgres Makefile interprets $PROFILE as CFLAGS
python3 bench/postgres_shim_build.py linux-build/libsmash.so
# expect:
#   long-lived (samples>5): ratio ≥ 95%
```

The compare driver is for performance investigation:

```sh
python3 bench/postgres_shim_build.py linux-build/libsmash.so \
        --mode=compare --runs 5 --clients 2 --perf-duration 30
# stock / stock+jemalloc / shim / shim+jemalloc / shim+smash table.
# Adjust --clients to match nproc on the VM.
```

`/tmp/smash-pg-shim/` is the cached postgres source + install. `/tmp/smash-pg-shim-run-*/` are per-run pgdata dirs (auto-cleaned when stage_run() returns; if `/tmp` fills up, leftover runs accumulated from a kill — `rm -rf /tmp/smash-pg-{shim,stock}-run-*`).

## What's next

In priority order:

1. **Run the perf comparison on a larger-core VM** (the user has been ramping core count up). Goal: validate that the timeout rate drops to zero. If it doesn't, the next likely culprit is the first-compression flurry described in PR #13 — fresh pages with no thrash history can still get into the compress→fault loop on a 60-second window. Possible mitigations: anticipatory "freshly-allocated, give it more time" heuristic, or feed the existing `kColdArenaFeedback` decompress signal into the compression gate.

2. **Cross-check on more workloads.** The cool-tail design point (long-running backend that idles) should reproduce on Redis, memcached, RocksDB. The `bench/` directory has scripts for those (`bench_redis.sh` etc.) but they predate the comparison driver — port one to the same `--mode=compare` pattern if the perf-vs-jemalloc number is interesting elsewhere too.

3. **Document the result for the paper.** `paper/` has the LaTeX. The headline numbers from this work:
   - Cool-tail postgres: 99.3 % per-backend compression on long-lived backends.
   - shim+jemalloc within 3 % of stock postgres → *Reconsidering* paper validated.
   - shim+smash on perf workload: ~−28 % (median) on 2-CPU; pending bigger-VM data to claim a clean number.

## Things NOT to do

These are traps from previous sessions:

- **Don't trust `bench_rss` alone as a smoke test for LD_PRELOAD.** It uses its own in-process `SmashHeap` and bypasses libc malloc; the third stats line in its output (the one with `committed=1`) is the LD_PRELOAD'd singleton and the actual canary. Use a tiny `malloc(64MB)+memset+sleep` standalone program if you want to verify LD_PRELOAD interception.
- **Don't try LibreOffice / Firefox / Chromium** — they have bundled allocators (mozjemalloc, PartitionAlloc, internal arena) that bypass libc malloc entirely. They run cleanly under smash but won't show meaningful compression. See `memory/feedback_browser_allocators`.
- **Don't run snap or flatpak versions of anything.** Snaps strip `LD_PRELOAD` at the sandbox boundary.
- **Don't use `pkill -f` against substrings that appear in your own command line.** Match by `os.readlink('/proc/PID/exe')` or by an unambiguous role string (we use `smashuser` as the postgres role for exactly this reason).
- **Don't add new libc wrappers without `.symver` aliases** for any libc symbol introduced after GLIBC_2.2.5. CI failed four times in PR #10 from this. See `memory/project_glibc_symbol_versioning`.
- **Don't bypass clangd diagnostics.** The repo has `.clangd` + `CMAKE_EXPORT_COMPILE_COMMANDS=ON` so clangd resolves project paths properly. Trust real diagnostics.

## Useful invocations

```sh
# Watch a running postgres backend's smash stats live (in another terminal):
LATEST=$(ls -td /tmp/smash-pg-shim-run-*/ | head -1); tail -f $LATEST/postgres.log | grep "smash stats"

# Force-clean any leaked test backends:
pkill -9 -f smashuser; pkill -9 -f /tmp/smash-pg

# Inspect smash's view of one backend mid-run (useful when debugging hangs):
gdb -p <pid> -batch -ex 'thread apply all bt 30'

# Long perf run with detailed pgbench output (no timeout fuse):
unset PROFILE; SMASH_BANNER=1 SMASH_STATS=1 LD_PRELOAD=$PWD/linux-build/libsmash.so \
  /tmp/smash-pg-shim/install/bin/pgbench ...
```

## Memory

The smash project memory at `~/.claude/projects/-media-psf-Home-git-smash/memory/` has accumulated lessons across PR #10–13:

- `feedback_browser_allocators` — why Firefox/Chromium/LibreOffice don't show smash compression.
- `project_aarch64_ld_preload_glibc_versioning` — the GLIBC_2.2.5 vs GLIBC_2.17 trap and how to spot it.
- `project_glibc_symbol_versioning` — the `.symver` dance for any libc-versioned wrapper, four-times-bitten in PR #10.
- `feedback_use_clangd` — set up compile_commands.json and trust the diagnostics.
- `linux_buffered_io_fix` — `__read` bypass via fread/fgets/getcwd.

Read those before making changes.
