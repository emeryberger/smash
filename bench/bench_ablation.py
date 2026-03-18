#!/usr/bin/env python3
"""Ablation study runner for Smash.

Rebuilds libsmash with different preprocessor defines and benchmarks each
configuration to measure the marginal contribution of individual features.
See ABLATION.md for the full study design.
"""

import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from collections import OrderedDict
from pathlib import Path

# All CMake variables that must be cleared between configs (via -U)
ALL_ABLATION_VARS = [
    "SMASH_NUM_ARENAS",
    "SMASH_COLD_TICKS",
    "SMASH_VERY_COLD_TICKS",
    "SMASH_DICT_TRAIN_SAMPLES",
    "SMASH_PREFETCH_WINDOW",
    "SMASH_COMPRESSOR_WORKERS",
    "SMASH_COMPRESS_STORE_SHARDS",
    "SMASH_LARGE_ALLOC_VM_THRESHOLD",
    "SMASH_ABLATION_NO_ZERO_EAGER",
    "SMASH_ABLATION_NO_ZERO_DEFERRED",
    "SMASH_ABLATION_NO_SKIP_STATS",
    "SMASH_ABLATION_NO_CHUNK_BITMAP",
    "SMASH_ABLATION_NO_CALLSITE_ARENA",
]

# Configs in priority execution order (see ABLATION.md)
CONFIGS = OrderedDict([
    ("B1", {
        "name": "full",
        "cmake_flags": {},
        "use_smash": True,
    }),
    ("B0", {
        "name": "system-malloc",
        "cmake_flags": {},
        "use_smash": False,
    }),
    ("T1a", {
        "name": "no-arenas",
        "cmake_flags": {"SMASH_NUM_ARENAS": "1"},
        "use_smash": True,
    }),
    ("T1k", {
        "name": "no-callsite-arena",
        "cmake_flags": {"SMASH_ABLATION_NO_CALLSITE_ARENA": "ON"},
        "use_smash": True,
    }),
    ("T1b", {
        "name": "no-zero-eager",
        "cmake_flags": {"SMASH_ABLATION_NO_ZERO_EAGER": "ON"},
        "use_smash": True,
    }),
    ("T2a", {
        "name": "no-zero-deferred",
        "cmake_flags": {"SMASH_ABLATION_NO_ZERO_DEFERRED": "ON"},
        "use_smash": True,
    }),
    ("C1", {
        "name": "no-zero-both",
        "cmake_flags": {
            "SMASH_ABLATION_NO_ZERO_EAGER": "ON",
            "SMASH_ABLATION_NO_ZERO_DEFERRED": "ON",
        },
        "use_smash": True,
    }),
    ("T1c", {
        "name": "no-adaptive",
        "cmake_flags": {"SMASH_VERY_COLD_TICKS": "9999"},
        "use_smash": True,
    }),
    ("T1d", {
        "name": "no-dict",
        "cmake_flags": {"SMASH_DICT_TRAIN_SAMPLES": "0"},
        "use_smash": True,
    }),
    ("C2", {
        "name": "no-arenas-no-dict",
        "cmake_flags": {
            "SMASH_NUM_ARENAS": "1",
            "SMASH_DICT_TRAIN_SAMPLES": "0",
        },
        "use_smash": True,
    }),
    ("T1e", {
        "name": "no-prefetch",
        "cmake_flags": {"SMASH_PREFETCH_WINDOW": "0"},
        "use_smash": True,
    }),
    ("T2b", {
        "name": "no-skip-stats",
        "cmake_flags": {"SMASH_ABLATION_NO_SKIP_STATS": "ON"},
        "use_smash": True,
    }),
    ("T1f", {
        "name": "single-worker",
        "cmake_flags": {"SMASH_COMPRESSOR_WORKERS": "1"},
        "use_smash": True,
    }),
    ("T1g", {
        "name": "single-shard",
        "cmake_flags": {"SMASH_COMPRESS_STORE_SHARDS": "1"},
        "use_smash": True,
    }),
    ("T2c", {
        "name": "no-chunk-bitmap",
        "cmake_flags": {"SMASH_ABLATION_NO_CHUNK_BITMAP": "ON"},
        "use_smash": True,
    }),
    ("T1h", {
        "name": "no-large-vm",
        "cmake_flags": {"SMASH_LARGE_ALLOC_VM_THRESHOLD": "1073741824"},
        "use_smash": True,
    }),
    ("B2", {
        "name": "no-compress",
        "cmake_flags": {"SMASH_COLD_TICKS": "9999"},
        "use_smash": True,
    }),
    ("T1i", {
        "name": "cold-ticks-1",
        "cmake_flags": {"SMASH_COLD_TICKS": "1"},
        "use_smash": True,
    }),
    ("T1j", {
        "name": "cold-ticks-5",
        "cmake_flags": {"SMASH_COLD_TICKS": "5"},
        "use_smash": True,
    }),
])

BENCHMARKS = {
    "json": "bench_json",
    "kv_store": "bench_kv_store",
    "sqlite": "bench_sqlite",
}

# Metrics to display per benchmark
PRIMARY_METRICS = [
    "rss_reduction_pct",
    "cool_reduction_pct",
    "peak_rss_mb",
    "steady_rss_mb",
    "min_rss_mb",
    "cold_p50_us",
    "cold_p99_us",
]

BENCH_EXTRA_METRICS = {
    "json": ["accesses_per_sec", "parse_throughput_mbs"],
    "kv_store": ["ops_per_sec", "hot_p50_us", "hot_p99_us"],
    "sqlite": ["ops_per_sec", "hot_p50_us", "hot_p99_us"],
}

RANKING_METRIC = "rss_reduction_pct"


def rebuild(build_dir, cmake_flags, source_dir):
    """Reconfigure CMake (clearing stale ablation vars) and rebuild."""
    cmd = ["cmake", str(source_dir), "-DSMASH_BUILD_BENCH=ON"]
    # Clear all ablation vars from cache first
    for var in ALL_ABLATION_VARS:
        cmd.append(f"-U{var}")
    # Set this config's flags
    for key, val in cmake_flags.items():
        cmd.append(f"-D{key}={val}")

    print(f"  cmake: {' '.join(cmd[3:])}")
    r = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  CMAKE FAILED:\n{r.stderr[-500:]}")
        return False

    r = subprocess.run(
        ["make", "-j"],
        cwd=build_dir, capture_output=True, text=True,
    )
    if r.returncode != 0:
        print(f"  MAKE FAILED:\n{r.stderr[-500:]}")
        return False
    return True


def parse_metrics(stdout):
    """Parse METRIC lines from benchmark stdout."""
    metrics = {}
    for line in stdout.splitlines():
        m = re.match(r"^METRIC (\S+) (\S+)$", line)
        if m:
            try:
                metrics[m.group(1)] = float(m.group(2))
            except ValueError:
                pass
    return metrics


def run_benchmark(exe, args, smash_lib=None):
    """Run a benchmark executable, optionally with smash interposition."""
    env = os.environ.copy()
    if smash_lib:
        if platform.system() == "Darwin":
            env["DYLD_INSERT_LIBRARIES"] = str(smash_lib)
        else:
            env["LD_PRELOAD"] = str(smash_lib)

    r = subprocess.run(
        [str(exe)] + args,
        capture_output=True, text=True, env=env,
        timeout=600,
    )
    return parse_metrics(r.stdout)


def median_metrics(runs):
    """Compute median of each metric across runs."""
    if not runs:
        return {}
    all_keys = set()
    for r in runs:
        all_keys.update(r.keys())
    result = {}
    for k in all_keys:
        vals = [r[k] for r in runs if k in r]
        if vals:
            result[k] = statistics.median(vals)
    return result


def fmt_val(val, metric_name):
    """Format a metric value for display."""
    if val is None:
        return "N/A"
    if "pct" in metric_name:
        return f"{val:.1f}%"
    if "rss" in metric_name or "mb" in metric_name.lower():
        return f"{val:.1f}"
    if "per_sec" in metric_name:
        if val >= 1e6:
            return f"{val/1e6:.1f}M"
        return f"{val:.0f}"
    if "us" in metric_name:
        return f"{val:.1f}"
    if "sec" in metric_name:
        return f"{val:.2f}"
    return f"{val:.1f}"


def fmt_delta(val, baseline_val, metric_name):
    """Format value with delta from baseline."""
    if val is None or baseline_val is None:
        return fmt_val(val, metric_name)
    delta = val - baseline_val
    sign = "+" if delta >= 0 else ""
    if "pct" in metric_name or "us" in metric_name:
        return f"{fmt_val(val, metric_name)} ({sign}{delta:.1f})"
    if "per_sec" in metric_name:
        if baseline_val != 0:
            pct = (delta / baseline_val) * 100
            return f"{fmt_val(val, metric_name)} ({sign}{pct:.1f}%)"
    if "rss" in metric_name or "mb" in metric_name.lower():
        return f"{fmt_val(val, metric_name)} ({sign}{delta:.1f})"
    return f"{fmt_val(val, metric_name)} ({sign}{delta:.1f})"


def print_bench_table(bench_name, results, config_ids):
    """Print a per-benchmark comparison table."""
    metrics = PRIMARY_METRICS + BENCH_EXTRA_METRICS.get(bench_name, [])

    # Header
    col_w = 14
    name_w = 24
    print(f"\n{'=' * 78}")
    print(f"  {bench_name} ablation results (median of {len(next(iter(results.values()))['runs'])} runs)")
    print(f"{'=' * 78}")
    header = f"  {'Config':<{name_w}}"
    for m in metrics:
        label = m.replace("_", " ").replace("pct", "%")
        header += f"{label:>{col_w}}"
    print(header)
    print(f"  {'-' * (name_w + col_w * len(metrics))}")

    baseline = results.get("B1", {}).get("median", {})

    for cid in config_ids:
        if cid not in results:
            continue
        cfg = CONFIGS[cid]
        med = results[cid]["median"]
        label = f"{cfg['name']} ({cid})"
        row = f"  {label:<{name_w}}"
        for m in metrics:
            val = med.get(m)
            if cid == "B1":
                row += f"{fmt_val(val, m):>{col_w}}"
            else:
                row += f"{fmt_delta(val, baseline.get(m), m):>{col_w}}"
        print(row)


def print_ranking(all_results, config_ids, bench_names):
    """Print cross-benchmark feature ranking."""
    print(f"\n{'=' * 78}")
    print(f"  Feature Impact Ranking (by avg {RANKING_METRIC} delta)")
    print(f"{'=' * 78}")

    col_w = 14
    name_w = 24
    header = f"  {'Rank':<6}{'Config':<{name_w}}"
    for b in bench_names:
        header += f"{b:>{col_w}}"
    header += f"{'Average':>{col_w}}"
    print(header)
    print(f"  {'-' * (6 + name_w + col_w * (len(bench_names) + 1))}")

    # Compute deltas
    ranking = []
    for cid in config_ids:
        if cid in ("B0", "B1"):
            continue
        deltas = []
        per_bench = {}
        for b in bench_names:
            if b not in all_results or cid not in all_results[b]:
                per_bench[b] = None
                continue
            baseline = all_results[b].get("B1", {}).get("median", {}).get(RANKING_METRIC)
            val = all_results[b][cid]["median"].get(RANKING_METRIC)
            if baseline is not None and val is not None:
                d = val - baseline
                deltas.append(d)
                per_bench[b] = d
            else:
                per_bench[b] = None
        avg = statistics.mean(deltas) if deltas else None
        ranking.append((cid, per_bench, avg))

    # Sort by average delta (most negative = biggest impact)
    ranking.sort(key=lambda x: x[2] if x[2] is not None else 0)

    for rank, (cid, per_bench, avg) in enumerate(ranking, 1):
        label = f"{CONFIGS[cid]['name']} ({cid})"
        row = f"  {rank:<6}{label:<{name_w}}"
        for b in bench_names:
            d = per_bench.get(b)
            if d is not None:
                row += f"{d:>+{col_w}.1f}"
            else:
                row += f"{'N/A':>{col_w}}"
        if avg is not None:
            row += f"{avg:>+{col_w}.1f}"
        else:
            row += f"{'N/A':>{col_w}}"
        print(row)


def write_report(output_dir, all_results, config_ids, bench_names):
    """Write markdown report to output_dir/ablation_report.md."""
    lines = ["# Ablation Study Results\n"]
    baseline_tag = "B1 (full)"

    for bench in bench_names:
        if bench not in all_results:
            continue
        results = all_results[bench]
        metrics = PRIMARY_METRICS + BENCH_EXTRA_METRICS.get(bench, [])
        baseline = results.get("B1", {}).get("median", {})

        lines.append(f"\n## {bench}\n")
        # Table header
        cols = ["Config"] + [m.replace("_", "\\_") for m in metrics]
        lines.append("| " + " | ".join(cols) + " |")
        lines.append("|" + "|".join(["---"] * len(cols)) + "|")

        for cid in config_ids:
            if cid not in results:
                continue
            med = results[cid]["median"]
            label = f"{CONFIGS[cid]['name']} ({cid})"
            vals = [label]
            for m in metrics:
                v = med.get(m)
                if cid == "B1":
                    vals.append(fmt_val(v, m))
                else:
                    vals.append(fmt_delta(v, baseline.get(m), m))
            lines.append("| " + " | ".join(vals) + " |")

    report_path = output_dir / "ablation_report.md"
    report_path.write_text("\n".join(lines) + "\n")
    print(f"\nReport written to {report_path}")


def main():
    parser = argparse.ArgumentParser(description="Smash ablation study runner")
    parser.add_argument("--build-dir", default="build",
                        help="Build directory (default: ./build)")
    parser.add_argument("--configs", default=None,
                        help="Comma-separated config IDs (default: all)")
    parser.add_argument("--benchmarks", default="json,kv_store,sqlite",
                        help="Comma-separated benchmark names")
    parser.add_argument("--runs", type=int, default=3,
                        help="Runs per config (default: 3)")
    parser.add_argument("--quick", action="store_true",
                        help="Pass --quick to benchmarks")
    parser.add_argument("--output", default="ablation_results",
                        help="Output directory")
    parser.add_argument("--resume", action="store_true",
                        help="Skip configs already in results file")
    args = parser.parse_args()

    source_dir = Path(__file__).resolve().parent.parent
    build_dir = Path(args.build_dir).resolve()
    output_dir = Path(args.output).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    results_path = output_dir / "ablation_results.json"

    bench_names = [b.strip() for b in args.benchmarks.split(",")]
    config_ids = (
        [c.strip() for c in args.configs.split(",")]
        if args.configs else list(CONFIGS.keys())
    )

    smash_lib = build_dir / ("libsmash.dylib" if platform.system() == "Darwin"
                             else "libsmash.so")
    bench_dir = build_dir / "bench"

    bench_args = ["--quick"] if args.quick else []

    # Load existing results for resume
    existing = {}
    if args.resume and results_path.exists():
        existing = json.loads(results_path.read_text())
        print(f"Loaded {sum(len(v) for v in existing.values())} existing results")

    # all_results[bench_name][config_id] = {"runs": [...], "median": {...}}
    all_results = {}
    for b in bench_names:
        all_results[b] = existing.get(b, {})

    # B0 and B1 share a build (default config); group configs by cmake_flags
    # to avoid redundant rebuilds
    last_flags = None
    total = len(config_ids)
    t0 = time.time()

    for idx, cid in enumerate(config_ids, 1):
        cfg = CONFIGS[cid]

        # Check resume
        if args.resume:
            already = all(cid in all_results[b] for b in bench_names)
            if already:
                print(f"[{idx}/{total}] {cfg['name']} ({cid}) — skipped (resume)")
                continue

        elapsed = time.time() - t0
        print(f"\n[{idx}/{total}] {cfg['name']} ({cid})  [{elapsed:.0f}s elapsed]")

        # Rebuild only if flags changed
        # B0 doesn't need smash, but B1 (default flags) does — both use empty flags
        # so they share a build. Only rebuild when cmake_flags actually differ.
        flags = cfg["cmake_flags"]
        need_rebuild = (flags != last_flags)

        if need_rebuild and cfg["use_smash"]:
            print(f"  Rebuilding...")
            if not rebuild(build_dir, flags, source_dir):
                print(f"  SKIPPING {cid}: build failed")
                continue
            last_flags = flags
        elif need_rebuild and not cfg["use_smash"]:
            # B0 doesn't use smash lib, but we need a default build for the
            # benchmark executables. If last build was non-default, rebuild.
            if last_flags is not None and last_flags != {}:
                print(f"  Rebuilding (default for baseline benchmarks)...")
                if not rebuild(build_dir, {}, source_dir):
                    print(f"  SKIPPING {cid}: build failed")
                    continue
                last_flags = {}

        # Run benchmarks
        for bench in bench_names:
            exe = bench_dir / BENCHMARKS[bench]
            if not exe.exists():
                print(f"  {bench}: executable not found at {exe}")
                continue

            runs = []
            for run_num in range(1, args.runs + 1):
                lib = smash_lib if cfg["use_smash"] else None
                try:
                    metrics = run_benchmark(exe, bench_args, smash_lib=lib)
                except subprocess.TimeoutExpired:
                    print(f"  {bench} run {run_num}: TIMEOUT")
                    continue
                except Exception as e:
                    print(f"  {bench} run {run_num}: ERROR {e}")
                    continue

                rss = metrics.get("rss_reduction_pct", "N/A")
                print(f"  {bench} run {run_num}: rss_reduction={rss}")
                runs.append(metrics)

            if runs:
                med = median_metrics(runs)
                all_results[bench][cid] = {
                    "runs": runs,
                    "median": med,
                }

        # Save after each config (incremental)
        results_path.write_text(json.dumps(all_results, indent=2))

    # Print tables
    for bench in bench_names:
        if bench in all_results and all_results[bench]:
            print_bench_table(bench, all_results[bench], config_ids)

    print_ranking(all_results, config_ids, bench_names)

    # Write markdown report
    write_report(output_dir, all_results, config_ids, bench_names)

    elapsed = time.time() - t0
    print(f"\nDone in {elapsed:.0f}s. Results: {results_path}")


if __name__ == "__main__":
    main()
