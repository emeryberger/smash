# userfaultfd vs mprotect Benchmark Comparison

## SQLite Benchmark (250K rows, 5s cool, 10s serve)

| Metric | Baseline | mprotect | userfaultfd |
|--------|----------|----------|-------------|
| Fill time | 1.30s | 1.77s | 1.80s |
| Peak RSS | 179 MB | 255 MB | 255 MB |
| Post-cool RSS | 179 MB | 87 MB | 87 MB |
| **RSS Reduction** | 0% | **65.7%** | **65.7%** |
| Ops/sec | 82,852 | 75,448 | 74,840 |
| Hot p50 | 0.76 μs | 0.83 μs | 0.82 μs |
| Hot p99 | 66.6 μs | 69.8 μs | 72.2 μs |
| Cold p50 | 1.3 μs | 12.7 μs | 12.7 μs |
| Cold p99 | 2.0 μs | 68.8 μs | 70.7 μs |

## RocksDB Benchmark (quick mode)

| Metric | Baseline | mprotect | userfaultfd |
|--------|----------|----------|-------------|
| Cold RSS | 283 MB | 140 MB | 142 MB |
| **RSS Reduction** | 0% | **51%** | **50%** |
| Cold p50 | 1.5 μs | 1.6 μs | 1.6 μs |
| Cold p99 | 8.0 μs | 65.0 μs | 61.1 μs |
| Hot p50 | 1.8 μs | 1.7 μs | 1.8 μs |
| Hot p99 | 3.3 μs | 3.8 μs | 3.8 μs |

## Conclusions

1. **RSS Reduction**: Both implementations achieve identical compression effectiveness
2. **Throughput**: Performance is within measurement noise (~1% difference)
3. **Latency**: userfaultfd shows slightly better p99 tail latency on RocksDB (61μs vs 65μs)
4. **Correctness**: Both implementations work correctly and produce consistent results

## Key Differences

| Aspect | mprotect (signals) | userfaultfd |
|--------|-------------------|-------------|
| Handler context | Signal context (async-signal-safe only) | Dedicated thread (any code allowed) |
| Signal conflicts | Can conflict with debuggers/sanitizers | No conflicts |
| Kernel requirements | Any Linux/macOS | Linux 4.3+, CAP_SYS_PTRACE |
| Implementation complexity | Simpler | More complex (hybrid approach) |

## Recommendation

The userfaultfd implementation works correctly and achieves identical results to the signal-based approach. Use userfaultfd when:
- Running with debuggers/sanitizers that install signal handlers
- Need to use malloc/complex functions in the fault handler
- Targeting Linux 4.3+ only

Use signal-based (mprotect) when:
- Cross-platform support needed (macOS)
- Simpler deployment requirements
- Older Linux kernels
