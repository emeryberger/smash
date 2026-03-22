#!/bin/bash
# Benchmark comparison: userfaultfd vs mprotect

set -e
cd "$(dirname "$0")"

RESULTS_DIR="paper_results/uffd_compare"
mkdir -p "$RESULTS_DIR"

echo "=== Fault Handler Comparison: userfaultfd vs mprotect ==="
echo ""

# Check if redis-server exists
REDIS_SERVER=$(which redis-server 2>/dev/null || echo "")
if [ -z "$REDIS_SERVER" ]; then
    echo "redis-server not found, using redis benchmark directly"
fi

# Function to run benchmark with a specific library
run_benchmark() {
    local lib=$1
    local name=$2
    local output_file="$RESULTS_DIR/${name}_results.txt"

    echo "--- Running benchmark with $name ---"
    echo "Library: $lib"

    # Use bench_sqlite as it's a reliable in-process benchmark
    echo "  SQLite benchmark..."
    LD_PRELOAD="$lib" ./bench/bench_sqlite --quick 2>&1 | tee "$output_file"

    echo ""
}

# Run baseline (no smash)
echo "=== Baseline (no Smash) ==="
./bench/bench_sqlite --quick 2>&1 | tee "$RESULTS_DIR/baseline_results.txt"
echo ""

# Run with mprotect version
echo "=== mprotect (signal-based) ==="
run_benchmark "./libsmash_mprotect.so" "mprotect"

# Run with userfaultfd version
echo "=== userfaultfd ==="
run_benchmark "./libsmash_uffd.so" "userfaultfd"

# Summary
echo ""
echo "=== Summary ==="
echo "Results saved to: $RESULTS_DIR/"

# Extract key metrics
echo ""
echo "--- Peak RSS Comparison ---"
echo "Baseline:"
grep -E "Peak RSS|RSS reduction" "$RESULTS_DIR/baseline_results.txt" || true
echo ""
echo "mprotect:"
grep -E "Peak RSS|RSS reduction" "$RESULTS_DIR/mprotect_results.txt" || true
echo ""
echo "userfaultfd:"
grep -E "Peak RSS|RSS reduction" "$RESULTS_DIR/userfaultfd_results.txt" || true

echo ""
echo "--- Cold Access Latency ---"
echo "Baseline:"
grep -E "p50|p99|latency" "$RESULTS_DIR/baseline_results.txt" || true
echo ""
echo "mprotect:"
grep -E "p50|p99|latency" "$RESULTS_DIR/mprotect_results.txt" || true
echo ""
echo "userfaultfd:"
grep -E "p50|p99|latency" "$RESULTS_DIR/userfaultfd_results.txt" || true
