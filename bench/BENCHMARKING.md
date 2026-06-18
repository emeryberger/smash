# Benchmarking Methodology

## Principles

1. **Randomized interleaving**: Run configurations in shuffled order across
   trials to prevent first-run cold penalties (CPU frequency scaling, TCP
   buffer warmup, page cache effects). The first run of a session can show
   40%+ penalty — do not compare it to later runs.

2. **Minimum 5 trials**: Each configuration runs at least 5 times. Report
   the **median** (robust to outliers), not the mean.

3. **Discard first trial**: The very first measurement in a session (across
   ALL configs) is discarded as warmup. Alternatively, run one throwaway
   trial before beginning measurement.

4. **Fresh server per measurement**: Each trial spawns a fresh server process.
   Never reuse a server across configurations (residual state, RSS drift).

5. **Proper load generation**: Use production-grade tools, not ad-hoc socket
   loops. Single-threaded Python socket clients bottleneck at ~40K ops/s
   regardless of server performance.

## Tools

| Workload | Tool | Settings |
|----------|------|----------|
| Redis | `redis-benchmark` | `-c 50 -n 200000 -t get -q` |
| Memcached | `memtier_benchmark` | `--protocol=memcache_text --threads=4 --clients=10 --requests=50000` |
| SQLite | `bench_sqlite` (built-in) | 500K rows, hot=5%, cool=10s, serve=20s |
| Generic | `bench_alloc_compare_preload.c` | 180 MB mixed heap, 12s cool |

## Redis Configuration

```
redis-server-libc --port PORT --save '' --appendonly no \
  --hz 1 --activedefrag no --loglevel warning
```

- `--hz 1`: Minimize background tasks (default 10 interferes with RSS measurement)
- `--activedefrag no`: Disable defragmentation (allocates/frees during serve)
- `--save '' --appendonly no`: No persistence IO

## Memcached Configuration

```
memcached -p PORT -m 512 -t 4
```

- `-t 4`: 4 worker threads (matches memtier's 4 threads)
- `-m 512`: 512 MB slab limit (prevent eviction during test)

## Smash Configuration

```
LD_PRELOAD=libsmash.so SMASH_COLD_TIMEOUT_SEC=5 SMASH_NO_MONITOR=1
```

- `SMASH_COLD_TIMEOUT_SEC=5`: Pages cold after 5s without access
- `SMASH_NO_MONITOR=1`: Disable Phase 3 mprotect monitoring (use soft-dirty)

## Fill Methodology

Use `redis-cli --pipe` for Redis (bulk protocol, fast).
Use `memtier_benchmark --ratio=1:0` or socket loop for memcached.
Always verify fill with DBSIZE/stats after fill completes.

## Measurement Phases

1. **Fill**: Load N keys × V bytes
2. **Verify**: Check DBSIZE, spot-check values
3. **Cool**: Sleep COLD_TIMEOUT + defer_ticks + margin (typically 15s)
4. **Measure RSS**: Read /proc/PID/status VmRSS
5. **Benchmark (cold→warm)**: First GET pass triggers decompression
6. **Benchmark (warm)**: Second GET pass measures steady-state
7. **Verify integrity**: Spot-check values post-benchmark

## Reporting

Report per-configuration:
- Peak RSS (after fill)
- Cool RSS (after cooling)
- RSS reduction %
- Cold throughput (rps) — first pass after cooling
- Warm throughput (rps) — second pass, steady-state
- p50 latency
- p99 latency
- Data integrity (PASS/FAIL)

## Known Pitfalls

- **Run ordering**: First allocator tested in a session gets 20-40% cold penalty.
  ALWAYS randomize order AND run multiple trials.
- **Python socket client**: Maxes at ~40K rps (single-threaded, synchronous).
  Use redis-benchmark or memtier for throughput measurement.
- **redis-benchmark version mismatch**: The system redis-benchmark may not
  support `--csv`. Use `-q` for simple output, parse last line.
- **LD_PRELOAD + daemonize**: Redis `--daemonize yes` drops LD_PRELOAD env
  in the child. Use subprocess.Popen (no daemonize) instead.
- **Stale redis instances**: Always pkill before starting. Check port conflicts.
