# smash

**smash** is a drop-in `malloc` replacement that finds the parts of your heap you aren't using and compresses them, so long-running services hold far less physical memory while idle or serving a small hot set.

by [Emery Berger](http://www.emeryberger.org), University of Massachusetts Amherst and Amazon Web Services

No code changes, no root and no kernel configuration: preload the library and go.

## Quick start

Requires a C++20 compiler (GCC 12+ or Clang 14+), CMake 3.15+, and network access on the first configure (to fetch [alloc8](https://github.com/emeryberger/alloc8), zstd and LZ4).

```bash
git clone https://github.com/emeryberger/smash && cd smash
cmake -B build && cmake --build build -j
build/bin/smash run --report -- ./your_server
```

`smash run` preloads the library (`LD_PRELOAD` on Linux, `DYLD_INSERT_LIBRARIES` on macOS) and starts your program. `--report` prints a summary when the program exits: peak memory and how far it dropped, how much was compressed and how well, and how long faults on compressed pages took. `cmake --install build --component smash --prefix ~/.local` installs `bin/smash` and `lib/libsmash.*`; `smash doctor` checks the setup. With a library downloaded from a [release](https://github.com/emeryberger/smash/releases), preload it directly (`LD_PRELOAD=/path/to/libsmash.so ./your_server`) or point the launcher at it with `SMASH_LIB`.

smash leaves a page alone until it has gone **10 seconds** without being touched, so a short test program that exits right away shows no change; the report then says why. That's intended: smash is built for services that stay up and leave most of their data idle. To see it act sooner, set `SMASH_COLD_TIMEOUT_SEC=2`.

On macOS, a downloaded dylib needs its quarantine flag cleared first (`xattr -d com.apple.quarantine libsmash-macos-arm64.dylib`). macOS also strips `DYLD_*` variables from SIP-protected system binaries, so smash applies to your own programs, not to `/bin` tools or `/usr/bin/python3`; `smash run` warns when that happens.

## Will smash help me?

Find out without risk before running it for real:

- **`smash-estimate <pid>`** (Linux, built with smash): attaches to a running process, no preload and
  no restart. It watches which anonymous pages stay idle for a window (default 10 s), compresses a
  sample of them the way smash would, and prints the projected RSS reduction with a confidence
  interval, per mapping, with notes on which memory smash manages. It never writes the target's
  memory. Reading page contents needs ptrace access (root, or launch the program through it:
  `smash-estimate -- ./your_server`).
- **`SMASH_MODE=observe`**: run with smash preloaded in dry-run mode. Cold detection and
  compression run as usual, but every compressed result is thrown away: no page is ever
  unmapped, protected against reads, or changed. It reports projected savings, the compression
  ratio histogram and an estimated refault rate on stderr (and as JSON with
  `SMASH_OBSERVE_REPORT=path`). It still uses smash's allocator, so add `SMASH_LARGE_ONLY=1` for the
  most conservative run.

Both overestimated the measured reduction on our SQLite benchmark (by 6.6 and 10.8 points); see
[agents/estimator.md](agents/estimator.md) for what they do and don't capture.

## Install

Every install method gives the same layout: `lib/libsmash.so` (`.dylib` on macOS), a `bin/smash` launcher that preloads it (`smash run -- ./your_server`), and headers in `include/smash/`. The Linux library is built on glibc 2.28, so it runs on Debian 11+, Ubuntu 20.04+, RHEL/Alma/Rocky 8+, Amazon Linux 2023 and Fedora. musl (Alpine) is not supported.

**Debian / Ubuntu, RHEL / Fedora / Amazon Linux.** Each [release](https://github.com/emeryberger/smash/releases) attaches `.deb` and `.rpm` packages for x86-64 and aarch64 (published from the first release cut after packaging landed). They install `/usr/lib/libsmash.so` and `/usr/bin/smash`:

```bash
tag=vX.Y.Z-alpha    # a release tag from the releases page (all are pre-releases so far,
                    # which GitHub's releases/latest/download/ URLs skip)
arch=$(uname -m)    # x86_64 or aarch64
curl -fLO https://github.com/emeryberger/smash/releases/download/$tag/smash-linux-$arch.deb
sudo apt install ./smash-linux-$arch.deb         # or: sudo dnf install ./smash-linux-$arch.rpm
smash run -- ./your_server                       # = LD_PRELOAD=/usr/lib/libsmash.so ./your_server
```

`smash-linux-$arch.tar.gz` holds the same files for unpacking anywhere without root.

**Docker.** `ghcr.io/emeryberger/smash` is a `FROM scratch` image that holds only `/smash/lib/libsmash.so` and `/smash/bin/smash`. Copy it into your own glibc-based image:

```dockerfile
FROM python:3.12-slim
COPY --from=ghcr.io/emeryberger/smash:latest /smash /opt/smash
ENV LD_PRELOAD=/opt/smash/lib/libsmash.so
```

Each release pushes `:<tag>` and moves `:latest`. See [packaging/docker/example/Dockerfile](packaging/docker/example/Dockerfile) for a full example (pin a release tag in place of `latest`).

**Homebrew (macOS).** The formula is [packaging/homebrew/smash.rb](packaging/homebrew/smash.rb). Homebrew only installs formulae from a tap, so put it in a local one. `--HEAD` builds from `master`; once a release has filled in the formula's tarball checksum, drop `--HEAD` to build that release:

```bash
brew tap-new --no-git $USER/local
curl -fsSL https://raw.githubusercontent.com/emeryberger/smash/master/packaging/homebrew/smash.rb \
  -o "$(brew --repo $USER/local)/Formula/smash.rb"
brew install --HEAD $USER/local/smash
```

On Apple Silicon the formula builds a fat arm64 + arm64e dylib, so it can also be loaded into arm64e processes.

**From source** (any platform; see Quick start for requirements):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSMASH_BUILD_TESTS=OFF
cmake --build build --target smash -j
sudo cmake --install build --prefix /usr/local
```

## What it does for real servers

Each workload fills its data, sits idle, then serves queries that touch only the newest 5% of the data. Out-of-the-box smash compared with glibc `malloc` and jemalloc, median of 3 runs:

| Workload | Peak RSS (glibc / jemalloc / smash) | Idle RSS (glibc → smash) | RSS while serving (glibc → smash) | Throughput vs glibc |
|---|---|---|---|---|
| SQLite (in-memory) | 487 / 569 / **405 MB** | 356 → **114 MB** (−68%) | 487 → **185 MB** (−62%) | 0.92× |
| RocksDB (block cache) | 279 / 411 / 298 MB | 274 → **42 MB** (−85%) | 280 → **61 MB** (−78%) | 0.98× |
| memcached | 290 / 298 / 314 MB | 290 → **31 MB** (−89%) | 290 → **50 MB** (−83%) | 1.03× |
| Redis | 264 / 275 / 288 MB | 264 → **116 MB** (−56%) | 264 → **116 MB** (−56%) | 0.94× |
| Redis, half the keys deleted | 264 / 274 / 293 MB | 264 → **92 MB** (−65%) | 264 → **92 MB** (−65%) | 0.99× |

How to read it:
- **Idle and serving memory drop by 56% to almost 90%.** The cold data is compressed, and it stays compressed while the hot 5% is served. SQLite's serving memory grows because its workload keeps inserting rows; glibc grows by more over the same phase.
- **Peak memory is roughly a wash.** During a fast fill everything is hot, so there is little to compress yet. On SQLite smash's peak is 17% *below* glibc's, because its size classes fit SQLite's ~4.4 KB pages more tightly; on the others it is 7–11% above glibc's (allocator overhead during the fill, plus a capped amount of compression in flight). jemalloc's peak is higher than both on SQLite and RocksDB. If you size machines by peak, allow about 10% headroom over glibc.
- **Throughput stays within 8% of glibc.** The first touch of a compressed page costs a decompression (on the order of 100 µs); after that the page is ordinary memory.
- **Redis gains the least.** `redis-benchmark` fills values with random text, which compresses to only about 40% of its size. Real data usually compresses better.

Measured on an AWS c7a.2xlarge (AMD EPYC 9R14, 8 vCPUs, 16 GB, Ubuntu 22.04, Linux 6.8) with `bench/run_paper_experiments.py`, which runs `SMASH_COLD_TIMEOUT_SEC=1` so the runs stay short. With the default 10 s, expect the same idle numbers about 10 seconds later.

## Why it works out of the box

The defaults are the tuned configuration. Each one targets a way a compressing allocator usually goes wrong:

- **Only cold data is compressed.** smash tracks every page and compresses one only after it has gone untouched for the cold timeout. A page that stays in use is never compressed, so the hot path pays nothing.
- **Tracking is cheap.** On Linux 6.8+ smash uses `userfaultfd` with no setup. Writes are tracked with write-protection, and a compressed page that gets touched comes back through a lightweight fault instead of a signal. On older kernels and macOS it falls back to `mprotect`-based tracking. The optional `smash-idled` helper (below) gives exact read tracking from the kernel's idle-page bitmap.
- **A page is released only after it proves it is cold.** smash first compresses a copy and keeps the original page for about a second. If the program writes to it in that window, the copy is discarded and nothing is lost. Redis, for example, updates an object's LRU field on every read.
- **New objects stay off cold pages.** smash's temperature-aware placement (TAP) never puts a new object into a free slot on a cold page; it uses a warm or empty page instead. Cold pages stay cold and stay compressed, instead of being woken up by one new object.
- **Similar data lands together.** Allocations from the same call site share an arena, so pages hold similar objects. That compresses better and makes each page's compressibility predictable.
- **Incompressible pages are skipped.** A return-on-investment check compares each page group's observed compression ratio and CPU cost against how long the page has been idle. Data that won't shrink is left alone. Compression uses zstd level 1.
- **It stays out of the way.** The compressor scales its worker threads with the amount of cold data and backs off when the CPU is busy. It also budgets the number of memory mappings it creates, so it never runs into the kernel's `vm.max_map_count` limit.

## When to use it, and when not

**Good fits:** long-running services with a large heap where most data sits idle at any moment, such as caches, in-memory databases, key-value stores, and services with big lookup tables or session state.

**Poor fits:**
- **Short-lived processes** (compilers, batch jobs) finish before anything goes cold.
- **Memory that is constantly in use**, where nothing is ever cold, gains nothing.
- **Workloads that must not exceed a memory cap at their peak.** smash lowers steady-state memory; its peak can be up to about 10% above glibc's.

### smash and zswap

Linux's zswap also compresses memory, but only *reactively*: it compresses pages the kernel has decided to evict, which happens under memory pressure. On a server with spare RAM, zswap does nothing, and idle heap data stays resident. smash compresses idle data *proactively*, with no pressure, no swap device and no system configuration.

Under a tight cgroup memory limit the picture is mixed. smash's compressed data lives in the process and counts against the cgroup. zswap cannot compress it again, so when the kernel reclaims it, it goes to the disk swap device and reaching it costs a disk read plus a decompress. Inside a container smash now detects the cgroup limit and keeps its compressed store out of reclaim (`SMASH_CGROUP`, on by default when a limit is present; see [agents/containers.md](agents/containers.md)).

Measured on the EPYC 9R14 host (24-core slice of a shared machine, zswap zstd, five interleaved runs; `bench/run_cgroup_compare.py`), inside the same cap as glibc+zswap smash used 25% less memory on average on SQLite (cap 250M) with the same throughput, but its cold-read p99 was 14% higher (2.6 vs 2.3 ms). On Redis (cap 160M) it used 27% less memory, but GET throughput was 19% lower. Earlier per-workload numbers are in [docs/ZSWAP.md](docs/ZSWAP.md).

## Deploying safely, and backing out

smash can be switched off inside a running process, by hand or automatically, without restarting it.

- **Kill switch.** `smash_disable()` (C API), a signal you choose (`SMASH_DISABLE_SIGNAL=SIGUSR1`), or a control file (`SMASH_CONTROL_FILE=/run/myapp/smash`, write `disable`) stops compression and decompresses every compressed page back in place. It also removes every page protection smash applied. The process keeps running with smash as a plain allocator, and smash causes no further page faults. `smash_enable()`, `SMASH_ENABLE_SIGNAL`, or writing `enable` turns compression back on. `SMASH_START_DISABLED=1` starts a process with compression off, so it can be turned on later. No signal is taken unless you name one.
- **Watchdog (fail-open).** With `SMASH_WATCHDOG_FAULT_RATE=N` (decompression faults per second, averaged over `SMASH_WATCHDOG_WINDOW` seconds) or `SMASH_WATCHDOG_P99_US=N` (fault-service p99), smash disables itself when the limit is exceeded and prints one line to stderr saying why. With `SMASH_WATCHDOG_DECOMP_FAIL=N` it also trips after N decompression failures (off by default).
- **Refault budget.** `SMASH_MAX_FAULT_RATE=N` adapts instead of tripping. While refaults exceed N per second, smash waits longer before compressing a page: the cold threshold doubles each second the budget is exceeded and relaxes once refaults are under half of it.
- **Status.** `smash_safety_status()` reports whether smash is disabled, how many pages are compressed, fault counts and rates, the current cold threshold, and the reason for the last disable. The stats line (`SMASH_STATS=1`) carries the same fields.

Details, and what is and isn't covered, are in [agents/safety.md](agents/safety.md).

**Formal models.** `model/` holds TLA+ models and Lean 4 proofs of smash's page state machine and of specific races that caused real bugs. The Lean proofs are machine-checked and use no `sorry`. `SmashCoreConc.lean` proves that, in the model, a page is never marked ACTIVE while still protected, and that a compressed page is always inaccessible, has its compressed copy, and holds no physical memory. This holds for any number of pages and threads and every interleaving. (The code has one known race outside this abstraction: a batched Phase 3 `mprotect` can leave a just-restored ACTIVE page read-only until its next write fault. The kill switch repairs it.) Other proofs cover the decompress-on-fault restore (no reader sees a readable page before its data is in place), the fork-pause handshake (a fork never lands mid-tick), the deferred-reclaim snapshot, the external-mapping munmap race, the free-pool race, and creating worker threads early. Each bug model also has its pre-fix variant, which TLC shows violates the property. What is *not* proven: that the C++ code matches the models (they are hand-written abstractions, with no refinement proof); sequentially consistent memory is assumed; the userfaultfd path, the compression codecs, the kill switch, and liveness under the `vm.max_map_count` limit are not modeled; and the TLC runs of the larger models are bounded. See [model/README.md](model/README.md).

## Options worth knowing

Everything below is optional; the defaults are what the results above use.

| Variable (`smash run` flag) | What it does |
|---|---|
| `SMASH_PROFILE=latency\|balanced\|memory` (`--profile`) | One knob for the tuning variables. `memory` compresses sooner (2 s) and returns memory more eagerly; `latency` waits 30 s and runs the compressor at idle priority. `balanced` is the default. |
| `SMASH_COLD_TIMEOUT_SEC=N` | Seconds a page must sit untouched before it is compressed (default 10). Overrides the profile. |
| `SMASH_LARGE_ONLY=1` (`--large-only`) | Only manage allocations over 16 KB and pass small ones to the system allocator. For runtimes with their own small-object allocator, such as Python 3.13+. |
| `SMASH_REPORT=1\|path` (`--report`) | Print a summary at exit. |
| `SMASH_METRICS_FILE=path` (`--metrics`) | Keep a Prometheus-format metrics file up to date (every 10 s), for node_exporter's textfile collector. No network port is opened. |
| `SMASH_MODE=observe` | Dry run: detect and compress cold pages but discard the result, reporting projected savings instead (`SMASH_OBSERVE_REPORT=path` for JSON, `SMASH_OBSERVE_INTERVAL=N` seconds between reports). See "Will smash help me?" above. |
| `SMASH_MODE=compress_only` | Don't replace `malloc` at all; just compress cold heap pages, whichever allocator owns them. For programs linked against jemalloc or tcmalloc. |
| `SMASH_DISABLE_SIGNAL=SIG` / `SMASH_CONTROL_FILE=path` | Kill switch: a signal or a file (`disable` / `enable`) that turns compression off and restores every page, or back on. See above. |
| `SMASH_WATCHDOG_FAULT_RATE=N` / `SMASH_MAX_FAULT_RATE=N` | Disable compression when refaults exceed N/s, or back off the cold threshold to stay under N/s. |

Every other variable (tuning, compatibility and diagnostics) is in [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

**Optional: exact read tracking on Linux.** The build also produces `smash-idled`, a small root helper that gives smash processes read tracking through the kernel's idle-page bitmap. smash connects to it automatically when it is running. It serves only the process that started it, plus that process's children:

```bash
sudo build/smash-idled &      # serves the shell that launched it and its children
LD_PRELOAD=$PWD/build/libsmash.so ./your_server
```

**Optional API.** Programs can also give hints directly:

```c
#include <smash/smash.h>
smash_hint_cold(ptr, size);   // done with it: may compress at the next tick (~1 s)
smash_hint_hot(ptr, size);    // about to use it: decompress now, restart its cold wait
struct SmashStats s; smash_get_stats(&s);   // s.compressed_bytes, s.stored_bytes, ...
smash_disable();              // stop compressing, restore every page
smash_enable();               // resume
struct SmashSafetyStatus st; smash_safety_status(&st);
```

## How it works

1. **Allocation.** smash interposes on `malloc`/`free` through [alloc8](https://github.com/emeryberger/alloc8). Objects up to 16 KB live in size-classed slabs; larger ones get their own pages. All of it comes from one large virtual-memory reservation, so smash can track every page.
2. **Tracking.** A background thread runs once a second and records which pages were written or read since the last pass.
3. **Compression.** Pages that stay cold past the timeout, and that are worth it, are compressed into a separate store. Their physical memory is then returned to the kernel (`MADV_DONTNEED` on Linux, `MADV_FREE_REUSABLE` on macOS).
4. **Decompression.** When the program touches a compressed page, a fault handler restores it before the access completes. The program never sees the difference.

## More

- [docs/CONFIGURATION.md](docs/CONFIGURATION.md): every environment variable and build-time constant.
- [docs/DEVELOPING.md](docs/DEVELOPING.md): building tests and benchmarks, competing allocators, reproducing the paper results, and architecture notes.
- [docs/ZSWAP.md](docs/ZSWAP.md): the earlier smash-vs-zswap measurements.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE).
