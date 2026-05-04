#!/usr/bin/env python3
"""Collect actual heatmap position data from benchmark runs.

This script runs benchmarks under Smash and extracts the measured
compressibility and hot fraction from the SIGUSR2 stats dump.

Usage:
    cd build
    python3 ../bench/collect_heatmap_positions.py [--output positions.json]

The output can be used to overlay real app positions on the heatmap figure.
"""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time

# Benchmarks to run: (name, command, sample_time_sec)
BENCHMARKS = [
    ('SQLite', './bench/bench_sqlite --quick', 15),
    ('RocksDB', './bench/bench_rocksdb_builtin --quick', 12),
]


def run_benchmark(name, cmd, sample_at, timeout=60):
    """Run benchmark under Smash and extract heatmap stats."""
    env = os.environ.copy()
    env['LD_PRELOAD'] = './libsmash.so'

    print(f"  Starting {name}...", file=sys.stderr)

    proc = subprocess.Popen(
        cmd.split(),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    # Wait for cooling phase
    time.sleep(sample_at)

    # Send SIGUSR2 to dump stats
    try:
        proc.send_signal(signal.SIGUSR2)
    except ProcessLookupError:
        pass

    # Wait for completion
    try:
        stdout, stderr = proc.communicate(timeout=timeout - sample_at)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()

    output = stderr.decode() + stdout.decode()

    # Parse heatmap line
    hm_match = re.search(
        r'\[smash heatmap\].*?'
        r'pages_compressed=(\d+).*?'
        r'ratio=([0-9.]+)x.*?'
        r'compress_pct=([0-9.]+)%.*?'
        r'hot_pct=([0-9.]+)%.*?'
        r'accessed=(\d+).*?monitored=(\d+)',
        output
    )

    # Parse RSS reduction
    rss_match = re.search(r'Min serve RSS:.*?\(([0-9.]+)% reduction\)', output)
    if not rss_match:
        rss_match = re.search(r'reduction from peak\).*?([0-9.]+)%', output)

    if hm_match:
        return {
            'name': name,
            'pages_compressed': int(hm_match.group(1)),
            'compression_ratio': float(hm_match.group(2)),
            'compress_pct': float(hm_match.group(3)),
            'hot_pct': float(hm_match.group(4)),
            'pages_accessed': int(hm_match.group(5)),
            'pages_monitored': int(hm_match.group(6)),
            'rss_reduction_pct': float(rss_match.group(1)) if rss_match else None,
        }
    return None


def main():
    parser = argparse.ArgumentParser(description='Collect heatmap position data')
    parser.add_argument('--output', '-o', default='heatmap_positions.json',
                        help='Output JSON file')
    parser.add_argument('--benchmarks', '-b', nargs='+',
                        help='Run only specified benchmarks (by name)')
    args = parser.parse_args()

    # Filter benchmarks if specified
    benchmarks = BENCHMARKS
    if args.benchmarks:
        benchmarks = [(n, c, t) for n, c, t in BENCHMARKS
                      if n.lower() in [b.lower() for b in args.benchmarks]]

    results = []
    print(f"Collecting heatmap data from {len(benchmarks)} benchmarks...",
          file=sys.stderr)

    for name, cmd, sample_at in benchmarks:
        print(f"\n{name}:", file=sys.stderr)
        data = run_benchmark(name, cmd, sample_at)
        if data:
            print(f"  compress_pct = {data['compress_pct']:.1f}%", file=sys.stderr)
            print(f"  hot_pct = {data['hot_pct']:.1f}%", file=sys.stderr)
            print(f"  compression_ratio = {data['compression_ratio']:.1f}x", file=sys.stderr)
            if data['rss_reduction_pct']:
                print(f"  rss_reduction = {data['rss_reduction_pct']:.1f}%", file=sys.stderr)
            results.append(data)
        else:
            print(f"  (no heatmap data captured)", file=sys.stderr)

    # Save results
    output = {
        'description': 'Measured heatmap positions from benchmark runs',
        'note': 'compress_pct and hot_pct can be used to plot apps on the heatmap',
        'benchmarks': results,
    }

    with open(args.output, 'w') as f:
        json.dump(output, f, indent=2)
    print(f"\nSaved to {args.output}", file=sys.stderr)

    # Print summary table
    print("\n" + "=" * 70)
    print(f"{'Benchmark':<12} {'Compress%':>10} {'Hot%':>8} {'Ratio':>8} {'RSS Red':>10}")
    print("-" * 70)
    for r in results:
        rss = f"{r['rss_reduction_pct']:.1f}%" if r['rss_reduction_pct'] else "N/A"
        print(f"{r['name']:<12} {r['compress_pct']:>9.1f}% {r['hot_pct']:>7.1f}% "
              f"{r['compression_ratio']:>7.1f}x {rss:>10}")
    print("=" * 70)


if __name__ == '__main__':
    main()
