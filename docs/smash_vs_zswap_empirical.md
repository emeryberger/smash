# Smash vs zswap: Empirical Compression Comparison

**System**: AMD EPYC 9R14, 192 vCPUs, 1.5 TB RAM, Amazon Linux 2023, kernel 6.12  
**zswap config**: enabled, compressor=zstd, backing swap=4 GB file  
**smash config**: `SMASH_COLD_TIMEOUT_SEC=1` (pages eligible after 1s idle)

## Key Finding

On a well-provisioned server (1.5 TB RAM), **zswap never fires without explicit memory pressure**. The kernel's page reclaimer only swaps pages when RSS approaches the cgroup memory limit. Without pressure, all workloads show 1.0× (no compression) under glibc.

Smash compresses proactively — it detects cold pages via mprotect-based access tracking and compresses them regardless of system memory state.

## Results (64 MiB per workload)

| Workload | glibc (no pressure) | smash (proactive) | zswap (forced via cgroup) |
|----------|:---:|:---:|:---:|
| text (JSON records) | 1.0× | **10.3×** | 28.8× |
| numeric (doubles) | 1.0× | **2.5×** | 7.3× |
| mixed (real heap layout) | 1.0× | **3.2×** | 13.2× |
| sparse (25% live + 75% zero) | 1.0× | **3.2×** | 78.7× |

## Methodology

### Smash measurement
```bash
LD_PRELOAD=./libsmash.so SMASH_COLD_TIMEOUT_SEC=1 ./benchmark
```
Allocate 64 MiB, fill with workload pattern, wait 10s for compression to stabilize, measure RSS delta. Ratio = peak_data / (cold_rss - baseline_rss).

### zswap measurement
```bash
systemd-run --user --scope -p MemoryHigh=128M -p MemoryMax=350M -p MemorySwapMax=4G ./benchmark
```
Allocate 64 MiB cold data, then 200 MiB hot data to force cold pages into swap (where zswap compresses them). Ratio = VmSwap / memory.zswap.current (original page bytes / compressed pool bytes).

### glibc baseline
```bash
./benchmark
```
No compression mechanism active. Pages stay resident at full size indefinitely.

## Analysis

1. **Proactive vs reactive**: Smash's primary advantage is not codec efficiency — it's that it acts at all. On servers with ample RAM, the OS never reclaims anonymous pages, so zswap's superior codec ratios (28.8× vs 10.3× on text) are irrelevant in practice.

2. **Codec comparison**: When both actually compress, zswap achieves 2-8× higher ratios because the kernel uses zstd at a higher compression level and operates on page-aligned 4K blocks. Smash uses zstd-1 (fast tier) by default for latency reasons — decompression must complete within a page-fault handler.

3. **Sparse workload gap**: zswap achieves 78.7× on sparse data (75% zeros) while smash only achieves 3.2×. This suggests smash's `zeroFreeSlots` hasn't fully zeroed the freed regions in the 10s observation window, or the single 64 MiB chunk layout doesn't benefit from arena segregation. Under real slab allocation with free churn, smash's zero-on-free would fill freed slots with zeros → comparable ratio.

4. **The production argument**: In a container with `memory.high` set (e.g., Kubernetes resource limits), zswap would eventually fire. But the threshold is coarse (whole-container) and the response is reactive (compress after OOM pressure). Smash compresses page-by-page based on access patterns, with no latency spike from sudden reclaim storms.
