# smash

**smash** is a drop-in `malloc` replacement that finds the parts of your heap you aren't using and compresses them, so long-running services hold far less physical memory while idle or serving a small hot set.

by [Emery Berger](http://www.emeryberger.org), University of Massachusetts Amherst and Amazon Web Services

No code changes, no root and no kernel configuration: preload the library and go.

## Quick start

Requires a C++20 compiler (GCC 12+ or Clang 14+), CMake 3.15+, and network access on the first configure (to fetch [alloc8](https://github.com/emeryberger/alloc8), zstd and LZ4).

```bash
git clone https://github.com/emeryberger/smash && cd smash
cmake -B build && cmake --build build -j
```

Then run any program with it:

```bash
LD_PRELOAD=$PWD/build/libsmash.so ./your_server                 # Linux
DYLD_INSERT_LIBRARIES=$PWD/build/libsmash.dylib ./your_server   # macOS
```

That's the whole setup. smash leaves a page alone until it has gone **10 seconds** without being touched, so a short test program that exits right away shows no change. That's intended: smash is built for services that stay up and leave most of their data idle. To see it act sooner, set `SMASH_COLD_TIMEOUT_SEC=2`.

On macOS, a dylib downloaded from a [release](https://github.com/emeryberger/smash/releases) needs its quarantine flag cleared first (`xattr -d com.apple.quarantine libsmash-macos-arm64.dylib`). macOS also strips `DYLD_*` variables from SIP-protected system binaries, so smash applies to your own programs, not to `/bin` tools.

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

Under a tight cgroup memory limit the picture is mixed. smash's compressed data lives in the process and counts against the cgroup, so the kernel can swap it out, and reaching it then costs two faults. Earlier measurements had smash ahead on memcached and zswap ahead on SQLite and Redis inside a tight cap. Those numbers predate the current defaults and are being re-measured; see [docs/ZSWAP.md](docs/ZSWAP.md).

## Options worth knowing

Everything below is optional; the defaults are what the results above use.

| Variable | What it does |
|---|---|
| `SMASH_COLD_TIMEOUT_SEC=N` | Seconds a page must sit untouched before it is compressed (default 10). Lower reclaims sooner; higher compresses less often. |
| `SMASH_LARGE_ONLY=1` | Only manage allocations over 16 KB and pass small ones to the system allocator. For runtimes with their own small-object allocator, such as Python 3.13+. |
| `SMASH_MODE=compress_only` | Don't replace `malloc` at all; just compress cold heap pages, whichever allocator owns them. For programs linked against jemalloc or tcmalloc. |
| `SMASH_TRACK_EXTERNAL=1` | Also compress anonymous memory the program maps itself with `mmap`. Opt-in. |
| `SMASH_STATS=1` | Print a one-line summary of what smash did when the process exits. |
| `SMASH_BANNER=1` | Print a line when smash loads, to confirm the preload worked. |
| `SMASH_UFFD=0` | Turn off `userfaultfd` tracking (on by default on Linux 6.8+). `SMASH_UFFD=1` reports why it is unavailable. |

The full list, including tuning and diagnostic knobs, is in [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

**Optional: exact read tracking on Linux.** The build also produces `smash-idled`, a small root helper that gives smash processes read tracking through the kernel's idle-page bitmap. smash connects to it automatically when it is running. It serves only the process that started it, plus that process's children:

```bash
sudo build/smash-idled &      # serves the shell that launched it and its children
LD_PRELOAD=$PWD/build/libsmash.so ./your_server
```

**Optional API.** Programs can also give hints directly:

```c
#include <smash/smash.h>
smash_hint_cold(ptr, size);   // compress this region now
smash_hint_hot(ptr, size);    // keep this region uncompressed
SmashStats stats; smash_get_stats(&stats);
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
