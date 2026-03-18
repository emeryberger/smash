#!/usr/bin/env python3
"""Memcached ablation runner.

Rebuilds libsmash with different ablation flags, then runs
bench_memcached.sh for each config to measure RSS impact.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ALL_VARS = [
    "SMASH_NUM_ARENAS", "SMASH_COLD_TICKS", "SMASH_VERY_COLD_TICKS",
    "SMASH_DICT_TRAIN_SAMPLES", "SMASH_PREFETCH_WINDOW", "SMASH_COMPRESSOR_WORKERS",
    "SMASH_COMPRESS_STORE_SHARDS", "SMASH_LARGE_ALLOC_VM_THRESHOLD",
    "SMASH_ABLATION_NO_ZERO_EAGER", "SMASH_ABLATION_NO_ZERO_DEFERRED",
    "SMASH_ABLATION_NO_SKIP_STATS", "SMASH_ABLATION_NO_CHUNK_BITMAP",
    "SMASH_ABLATION_NO_CALLSITE_ARENA",
]

CONFIGS = [
    ("B1",  "full",           {}),
    ("T1a", "no-arenas",      {"SMASH_NUM_ARENAS": "1"}),
    ("T1k", "no-callsite-arena", {"SMASH_ABLATION_NO_CALLSITE_ARENA": "ON"}),
    ("T1b", "no-zero-eager",  {"SMASH_ABLATION_NO_ZERO_EAGER": "ON"}),
    ("T2a", "no-zero-defer",  {"SMASH_ABLATION_NO_ZERO_DEFERRED": "ON"}),
    ("T1c", "no-adaptive",    {"SMASH_VERY_COLD_TICKS": "9999"}),
    ("T1d", "no-dict",        {"SMASH_DICT_TRAIN_SAMPLES": "0"}),
    ("T1e", "no-prefetch",    {"SMASH_PREFETCH_WINDOW": "0"}),
    ("T1f", "single-worker",  {"SMASH_COMPRESSOR_WORKERS": "1"}),
]


def rebuild(build_dir, source_dir, flags):
    cmd = ["cmake", str(source_dir), "-DSMASH_BUILD_BENCH=ON"]
    for v in ALL_VARS:
        cmd.append(f"-U{v}")
    for k, val in flags.items():
        cmd.append(f"-D{k}={val}")
    r = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  cmake failed: {r.stderr[-300:]}")
        return False
    r = subprocess.run(["make", "-j"], cwd=build_dir, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  make failed: {r.stderr[-300:]}")
        return False
    return True


def run_memcached_bench(bench_script):
    """Run bench_memcached.sh, return (stdout, metrics_dict)."""
    r = subprocess.run(
        ["bash", str(bench_script)],
        capture_output=True, text=True, timeout=300,
    )
    stdout = r.stdout
    # Parse human-readable output for key metrics
    metrics = {}
    for line in stdout.splitlines():
        m = re.match(r"\s*(Fill|Post-cool|Serve|Cold re-access)\s+RSS:\s+([\d.]+)\s+MB", line)
        if m:
            key = m.group(1).lower().replace(" ", "_").replace("-", "") + "_rss"
            metrics[key] = float(m.group(2))
        m = re.match(r"\s*Serve:\s+([\d]+)\s+GET/s", line)
        if m:
            metrics["serve_ops"] = int(m.group(1))
    return stdout, metrics


def main():
    source_dir = Path(__file__).resolve().parent.parent
    build_dir = source_dir / "build"
    bench_script = build_dir / "bench" / "bench_memcached.sh"
    output_dir = source_dir / "ablation_results_full"
    output_dir.mkdir(exist_ok=True)

    if not bench_script.exists():
        print(f"ERROR: {bench_script} not found. Build with -DSMASH_BUILD_BENCH=ON")
        sys.exit(1)

    t0 = time.time()
    results = {}
    last_flags = None

    for cid, name, flags in CONFIGS:
        elapsed = time.time() - t0
        print(f"\n[{cid}] {name}  [{elapsed:.0f}s elapsed]")

        if flags != last_flags:
            print("  Rebuilding...")
            if not rebuild(build_dir, source_dir, flags):
                print("  BUILD FAILED — skipping")
                continue
            last_flags = flags

        stdout, metrics = run_memcached_bench(bench_script)
        results[cid] = {"name": name, "flags": flags, "metrics": metrics}

        # Print the script's own summary table
        in_table = False
        for line in stdout.splitlines():
            if "------" in line and "Metric" not in line:
                in_table = True
                continue
            if in_table:
                if line.strip() == "" or "======" in line:
                    in_table = False
                    continue
                print(f"  {line.rstrip()}")

    # Summary
    print(f"\n{'=' * 75}")
    print(f"  Memcached Ablation Summary")
    print(f"{'=' * 75}")

    b1_fill = results.get("B1", {}).get("metrics", {}).get("fill_rss")
    b1_cool = results.get("B1", {}).get("metrics", {}).get("postcool_rss")
    b1_serve = results.get("B1", {}).get("metrics", {}).get("serve_rss")

    header = f"  {'Config':<20} {'Fill RSS':>10} {'Cool RSS':>10} {'Serve RSS':>10} {'Serve ops':>12}"
    print(header)
    print(f"  {'-' * 62}")

    for cid, name, _ in CONFIGS:
        if cid not in results:
            continue
        m = results[cid]["metrics"]
        fill = m.get("fill_rss", "?")
        cool = m.get("postcool_rss", "?")
        serve = m.get("serve_rss", "?")
        ops = m.get("serve_ops", "?")

        delta_cool = ""
        if isinstance(cool, (int, float)) and isinstance(b1_cool, (int, float)):
            d = cool - b1_cool
            delta_cool = f" ({d:+.0f})"

        fill_s = f"{fill:.0f} MB" if isinstance(fill, (int, float)) else str(fill)
        cool_s = f"{cool:.0f} MB" if isinstance(cool, (int, float)) else str(cool)
        serve_s = f"{serve:.0f} MB" if isinstance(serve, (int, float)) else str(serve)
        ops_s = f"{ops:,}" if isinstance(ops, int) else str(ops)

        print(f"  {name:<20} {fill_s:>10} {cool_s:>10}{delta_cool:>6} {serve_s:>10} {ops_s:>12}")

    elapsed = time.time() - t0
    print(f"\nDone in {elapsed:.0f}s")

    out_path = output_dir / "memcached_ablation.json"
    out_path.write_text(json.dumps(results, indent=2))
    print(f"Results: {out_path}")


if __name__ == "__main__":
    main()
