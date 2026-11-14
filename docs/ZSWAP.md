# smash and zswap

> **Status:** the measurements below predate smash's current defaults (userfaultfd tracking, temperature-aware placement, the November 2026 compressor changes) and need to be re-run. Treat them as directional.

## The basic difference

zswap is the Linux kernel's compressed cache for swapped-out pages. It is *reactive*: it compresses only pages the kernel has already decided to evict, which happens under memory pressure, and the kernel prefers evicting file-backed pages over anonymous heap memory. On a server with spare RAM, zswap is effectively idle, and cold heap data stays resident and uncompressed.

smash is *proactive*: it compresses heap pages once they go cold, whether or not there is memory pressure. It needs no swap device, no root and no kernel configuration.

## Under a tight cgroup memory limit

The fair comparison gives both the same memory budget: the same cgroup `MemoryHigh` cap, with zswap available in both cases.

Measured on an AMD EPYC 9R14 (192 vCPUs, 1.5 TB RAM, kernel 6.12), zswap with zstd/zbud and 4 GB of swap:

| Workload (cap) | Config | Min RSS | Throughput | Cold p99 | zswap pages |
|---|---|---:|---:|---:|---:|
| **SQLite (200M)** | baseline, no cap | 355 MiB | 97k ops/s | 1.8 µs | 0 |
| | glibc + zswap | 101 MiB | 90k ops/s | 1853 µs | 0 |
| | smash + zswap | 130 MiB | 88k ops/s | 3676 µs | 0 |
| **Redis (200M)** | baseline, no cap | 244 MiB | 65k rps | 0.8 ms | 0 |
| | glibc + zswap | 154 MiB | 30k rps | 3.5 ms | 25k |
| | smash + zswap | 49 MiB | 14k rps | 10.5 ms | 71k |
| **memcached (150M)** | baseline, no cap | 232 MiB | 200k rps | 0.19 ms | 0 |
| | glibc + zswap | 144 MiB | 211k rps | 0.18 ms | 23k |
| | smash + zswap | **12 MiB** | 199k rps | 0.18 ms | 513 |

**memcached favored smash:** same throughput and latency, at 12 MiB instead of 144 MiB. smash had already compressed the cold slabs, so the cgroup barely had to swap.

**SQLite and Redis favored zswap.** smash keeps its compressed data in the process's own memory (the CompressStore), which counts against the cgroup, while zswap's pool does not. Under the cap, the kernel swapped CompressStore pages out (71k pages on Redis). Reaching compressed data then took two faults, a swap-in followed by a decompression, which made smash's cold latency worse than zswap's single fault. Pinning the CompressStore with `mlock()` prevents that but uses up the cgroup budget. `MADV_WILLNEED` is ignored under pressure.

| Scenario | Better fit | Why |
|----------|-----------|-----|
| Spare RAM, no memory pressure | smash | zswap does nothing without pressure |
| Tight cgroup, working set fits under the cap | smash | compresses cold data within the budget |
| Tight cgroup, working set exceeds the cap | zswap | the CompressStore competes with the working set |
| Latency-sensitive under pressure | zswap | one swap fault vs. a double fault |
| No root or kernel configuration | smash | `LD_PRELOAD` only |

Possible fixes for the cgroup case: keep compressed blobs in memory the kernel manages differently (e.g. `memfd_create` + `MADV_COLD`), or detect cgroup pressure and stop compressing.

## To verify in the re-measurement

The earlier README also argued that smash avoids several costs of reactive reclaim. These are plausible but were not all measured, and should be tested rather than assumed:

- **Cold-access latency:** userspace decompression from RAM (~100 µs) against a kernel swap-in (~2 ms p99 measured).
- **Direct-reclaim stalls:** `malloc` → `brk`/`mmap` under pressure runs direct reclaim on the calling thread. Memory smash has already released should avoid that.
- **Coldness signal:** smash tracks idle time per page, while the kernel's LRU uses a coarse accessed bit.
- **Pressure cliff:** zswap degraded sharply at the pressure boundary (Redis lost 69% of its throughput); smash's cost grows with the cold fraction.
- **Neighbors:** kswapd scanning and TLB-shootdown IPIs affect every process on the host; smash's compressor runs inside the one process.

## Reproducing

Requires Linux, cgroup v2, zswap enabled and swap active. See [smash_vs_zswap_empirical.md](smash_vs_zswap_empirical.md) for the methodology and [smash_vs_zswap_zram.md](smash_vs_zswap_zram.md) for the zram comparison.
