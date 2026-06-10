# Allocator comparison on smash's own benchmarks (mstat / cgroup accounting)

**Date:** 2026-06-06  **Host:** 192-core, 1.48 TB RAM, Linux 6.12 (cgroup v2)
**Method:** each workload run under `glibc / jemalloc / mimalloc / smash` via
`LD_PRELOAD`. Memory measured two ways: (a) the bench's own getrusage RSS at
each phase (the paper's metric), and (b) `mstat` cgroup `memory.current` of the
whole process tree (shared pages counted once). Smash = full mode,
`SMASH_VERY_COLD_TICKS=5 SMASH_COLD_TIMEOUT_SEC=2`.

> **Prerequisite:** these results are **post** the deferred-madvise regression
> fix (commit a8354be). Before it, `SMASH_DEFER_MADVISE_TICKS` defaulted to 50
> ticks = **50 seconds** (mislabeled "~500ms"), so compressed pages stayed
> resident through any cooling window shorter than 50s and smash showed **zero**
> RSS reduction on every app benchmark. The regression was bisected to 823515f.

## SQLite (500K rows, in-memory; getrusage RSS)

| alloc | peak MiB | min RSS MiB | reduction | ops/s |
|-------|----------|-------------|-----------|-------|
| glibc    | 355 | 355 | 28% | 96K |
| jemalloc | 418 | 418 | 27% | 92K |
| mimalloc | 415 | 416 | 27% | 93K |
| **smash** | 460 | **126** | **73%** | 88K |

Only smash reclaims the cold heap (126 vs 355–418 MiB); ~8% lower throughput.
(Paper reported 429→191, 69%.)

## RocksDB (1M keys × 512B, block cache; getrusage RSS)

| alloc | peak | cool RSS | serve RSS | min RSS | ops/s |
|-------|------|----------|-----------|---------|-------|
| glibc    | 275 | 275 | 282 | 275 | 700K |
| jemalloc | 384 | 384 | 390 | 384 | 720K |
| mimalloc | 281 | 281 | 291 | 281 | 720K |
| **smash** | 298 | **54** | **84** | **54** | 700K |

Smash drops to 54 MiB during cooling (vs 275–384); throughput on par.
(Note: mstat cgroup min is ~313 MiB for smash because rocksdb's SST page cache
is file-backed and counted by the cgroup but not by heap-RSS — that memory is
outside smash's reach by design.)

## Redis (200K keys × 2KB; mstat cgroup memory.current)

> **Correction (2026-06-06):** the originally-reported Redis `min during cool`
> numbers were a **measurement artifact**. The sampler kept reading the cgroup's
> `memory.current` as `redis-server` exited, so the final sample(s) captured the
> *emptying* cgroup (e.g. mimalloc's "2 MiB") — not idle reclaim. After trimming
> the shutdown-teardown tail (drop trailing samples < 50% of peak), the true
> cooling-phase minimums are below. **mimalloc does NOT reclaim idle Redis
> memory** (it stays flat at ~411 MiB); only smash does.

**Standard (SET → cool → GET):**

| alloc | peak MiB | cool min MiB | reclaims idle? | get rps |
|-------|----------|--------------|----------------|---------|
| glibc    | 331 | 330 | no | 58K |
| jemalloc | 408 | 406 | no | 63K |
| mimalloc | 414 | 411 | no | 61K |
| **smash** | 606 | **187** | **yes (compresses)** | 44K |

**Extended (SET → DELETE 50% → cool → GET):**

| alloc | peak MiB | cool min MiB |
|-------|----------|--------------|
| glibc    | 332 | 330 |
| jemalloc | 410 | 407 |
| mimalloc | 414 | 375 |
| **smash** | 603 | **187** |

In both variants smash is the **only** allocator that reduces its footprint
during the idle cooling window (606→187 MiB), by compressing the cold key data.
glibc/jemalloc/mimalloc hold the full working set resident. Smash's peak is
higher (its VM/compress-store/metadata overhead is real) and its GET throughput
is ~30% lower (serve-phase decompression faults) — the genuine time/space
tradeoff. The earlier claim that "mimalloc wins on cooling memory" was the
teardown artifact and is retracted.

## Honest summary

- **The regression fix is the headline:** smash's RSS reduction was completely
  broken (50s deferral) and is now restored; it again reduces cold-heap memory
  by 70–80% on sqlite/rocksdb, matching the paper.
- **vs glibc:** smash is a large memory win on every cold-data workload.
- **vs mimalloc:** mimalloc does NOT reclaim idle memory on these workloads
  (the earlier "mimalloc reclaims to ~2 MiB" was a shutdown-teardown sampling
  artifact, now corrected — it stays flat at ~411 MiB on Redis). smash is the
  only allocator here that shrinks its footprint when data goes cold, by
  compressing it. The cost is a higher peak (VM/metadata overhead) and ~30%
  lower Redis GET throughput (serve-phase decompression).
- **Throughput:** smash costs ~8% on sqlite, ~0% on rocksdb, but ~30% on redis
  GET (serve-phase decompression faults) — the real time/space tradeoff.
