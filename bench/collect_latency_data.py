#!/usr/bin/env python3
"""Collect latency data from benchmarks for CDF plots.

Runs SQLite, RocksDB, and LevelDB benchmarks under Smash and collects
cold-access latency measurements. Outputs CSV files suitable for CDF plotting.

Usage:
    cd build
    python3 ../bench/collect_latency_data.py [--output-dir ./latency_data]

The benchmarks output METRIC lines with p50/p99 values, and can dump raw
latency samples to CSV when SMASH_LATENCY_DIR is set.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


def get_preload_var():
    """Return the appropriate library preload environment variable."""
    if sys.platform == "darwin":
        return "DYLD_INSERT_LIBRARIES"
    return "LD_PRELOAD"


def parse_metrics(text):
    """Parse METRIC lines from benchmark output."""
    metrics = {}
    for line in text.splitlines():
        m = re.match(r"^METRIC (\S+) (\S+)$", line)
        if m:
            try:
                metrics[m.group(1)] = float(m.group(2))
            except ValueError:
                pass
    return metrics


def run_benchmark(name, cmd, build_dir, latency_dir, quick=False):
    """Run a benchmark and collect latency data.

    Returns dict with metrics including cold_p50_us, cold_p99_us, etc.
    Also writes raw latency samples to CSV in latency_dir.
    """
    print(f"\n{'='*60}")
    print(f"Running {name}...")
    print(f"{'='*60}")

    smash_lib = build_dir / "libsmash.so"
    if not smash_lib.exists():
        smash_lib = build_dir / "libsmash.dylib"

    if not smash_lib.exists():
        print(f"  ERROR: libsmash not found in {build_dir}")
        return None

    env = os.environ.copy()
    env[get_preload_var()] = str(smash_lib)
    env["SMASH_LATENCY_DIR"] = str(latency_dir)

    full_cmd = cmd if not quick else cmd + ["--quick"]

    try:
        result = subprocess.run(
            full_cmd,
            env=env,
            capture_output=True,
            text=True,
            timeout=300  # 5 minute timeout
        )

        # Print stderr for visibility
        if result.stderr:
            for line in result.stderr.splitlines()[-20:]:
                print(f"  {line}")

        metrics = parse_metrics(result.stdout)

        if metrics:
            print(f"\n  Results:")
            if "cold_p50_us" in metrics:
                print(f"    Cold access p50: {metrics['cold_p50_us']:.2f} us")
            if "cold_p99_us" in metrics:
                print(f"    Cold access p99: {metrics['cold_p99_us']:.2f} us")
            if "hot_p50_us" in metrics:
                print(f"    Hot access p50: {metrics['hot_p50_us']:.2f} us")
            if "hot_p99_us" in metrics:
                print(f"    Hot access p99: {metrics['hot_p99_us']:.2f} us")
            if "rss_reduction_pct" in metrics:
                print(f"    RSS reduction: {metrics['rss_reduction_pct']:.1f}%")

        return metrics

    except subprocess.TimeoutExpired:
        print(f"  ERROR: {name} timed out")
        return None
    except Exception as e:
        print(f"  ERROR: {name} failed: {e}")
        return None


def run_sqlite(build_dir, latency_dir, quick):
    """Run SQLite benchmark."""
    exe = build_dir / "bench" / "bench_sqlite"
    if not exe.exists():
        print(f"  bench_sqlite not found at {exe}")
        return None
    return run_benchmark("SQLite", [str(exe)], build_dir, latency_dir, quick)


def run_rocksdb(build_dir, latency_dir, quick):
    """Run RocksDB benchmark."""
    exe = build_dir / "bench" / "bench_rocksdb_builtin"
    if not exe.exists():
        print(f"  bench_rocksdb_builtin not found at {exe}")
        return None
    return run_benchmark("RocksDB", [str(exe)], build_dir, latency_dir, quick)


def run_leveldb(build_dir, latency_dir, quick):
    """Run LevelDB benchmark."""
    exe = build_dir / "bench" / "bench_leveldb"
    if not exe.exists():
        print(f"  bench_leveldb not found at {exe}")
        return None
    return run_benchmark("LevelDB", [str(exe)], build_dir, latency_dir, quick)


def main():
    parser = argparse.ArgumentParser(description="Collect latency data for CDF plots")
    parser.add_argument("--output-dir", "-o", default="latency_data",
                        help="Output directory for latency CSV files")
    parser.add_argument("--quick", "-q", action="store_true",
                        help="Run quick versions of benchmarks")
    parser.add_argument("--benchmarks", "-b", nargs="+",
                        choices=["sqlite", "rocksdb", "leveldb", "all"],
                        default=["all"],
                        help="Which benchmarks to run")
    args = parser.parse_args()

    # Determine build directory (assume we're in build/)
    build_dir = Path.cwd()
    if not (build_dir / "libsmash.so").exists() and not (build_dir / "libsmash.dylib").exists():
        print("ERROR: Run this script from the build directory")
        print("       (where libsmash.so/dylib exists)")
        sys.exit(1)

    latency_dir = Path(args.output_dir)
    latency_dir.mkdir(parents=True, exist_ok=True)

    benchmarks = args.benchmarks
    if "all" in benchmarks:
        benchmarks = ["sqlite", "rocksdb", "leveldb"]

    results = {}

    print(f"Collecting latency data from {len(benchmarks)} benchmarks...")
    print(f"Output directory: {latency_dir.absolute()}")

    for bench in benchmarks:
        if bench == "sqlite":
            metrics = run_sqlite(build_dir, latency_dir, args.quick)
        elif bench == "rocksdb":
            metrics = run_rocksdb(build_dir, latency_dir, args.quick)
        elif bench == "leveldb":
            metrics = run_leveldb(build_dir, latency_dir, args.quick)
        else:
            continue

        if metrics:
            results[bench] = metrics

    # Save summary
    summary_path = latency_dir / "latency_summary.json"
    with open(summary_path, "w") as f:
        json.dump(results, f, indent=2)

    print(f"\n{'='*60}")
    print("Summary")
    print(f"{'='*60}")

    # Check what CSV files were created
    csv_files = list(latency_dir.glob("*.csv"))
    if csv_files:
        print(f"\nLatency CSV files created:")
        for f in sorted(csv_files):
            lines = sum(1 for _ in open(f)) - 1  # subtract header
            print(f"  {f.name}: {lines} samples")

    print(f"\nLatency percentiles:")
    print(f"{'Benchmark':<12} {'Cold p50':>10} {'Cold p99':>10} {'Hot p50':>10} {'Hot p99':>10}")
    print("-" * 54)
    for bench, m in results.items():
        cold_p50 = f"{m.get('cold_p50_us', 'N/A'):>10.2f}" if isinstance(m.get('cold_p50_us'), (int, float)) else "N/A".rjust(10)
        cold_p99 = f"{m.get('cold_p99_us', 'N/A'):>10.2f}" if isinstance(m.get('cold_p99_us'), (int, float)) else "N/A".rjust(10)
        hot_p50 = f"{m.get('hot_p50_us', 'N/A'):>10.2f}" if isinstance(m.get('hot_p50_us'), (int, float)) else "N/A".rjust(10)
        hot_p99 = f"{m.get('hot_p99_us', 'N/A'):>10.2f}" if isinstance(m.get('hot_p99_us'), (int, float)) else "N/A".rjust(10)
        print(f"{bench:<12} {cold_p50} {cold_p99} {hot_p50} {hot_p99}")

    print(f"\nResults saved to: {summary_path}")


if __name__ == "__main__":
    main()
