#!/usr/bin/env python3
"""GAP Benchmark Suite benchmark for Smash.

Downloads and builds the GAP Benchmark Suite (gapbs), generates synthetic
graphs, and runs BFS/PageRank with LD_PRELOAD=libsmash.so.

Measurement strategy: gapbs loads the graph once, then runs N trials of the
algorithm. With a large graph, the adjacency structure dominates RSS. During
BFS/SSSP, only the frontier + neighbor pages are hot — the rest of the graph
is cold and compressible by Smash. With many trials (-n), Smash has time to
compress cold regions between iterations.

We sample RSS externally every 0.5s and compare peak/average RSS between
baseline and Smash runs.

Usage:
    cd build
    python3 ../bench/bench_gap.py [--quick] [--skip-build]

Prerequisites:
    - g++ with OpenMP support
    - Internet access (to clone gapbs)
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


# ── RSS helpers ────────��─────────────────────────��───────────────────────────

def get_rss_mb(pid):
    """Get RSS of a process in MiB via /proc (Linux) or ps (fallback)."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024.0
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        pass
    # Fallback to ps
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)], text=True
        )
        return float(out.strip()) / 1024.0
    except Exception:
        return 0.0


# ── GAP build ──────���──────────────────────────────────���──────────────────────

GAPBS_REPO = "https://github.com/sbeamer/gapbs.git"


def build_gapbs(install_dir):
    """Clone and build the GAP Benchmark Suite."""
    install_dir = Path(install_dir)

    if not (install_dir / "Makefile").exists():
        print(f"Cloning gapbs to {install_dir}...")
        subprocess.run(
            ["git", "clone", "--depth", "1", GAPBS_REPO, str(install_dir)],
            check=True,
        )

    print("Building gapbs...")
    subprocess.run(
        ["make", "-j", str(os.cpu_count() or 4)],
        cwd=str(install_dir), check=True,
    )

    # Verify key binaries exist
    for prog in ["bfs", "pr", "converter"]:
        if not (install_dir / prog).exists():
            print(f"ERROR: {prog} not found in {install_dir}")
            sys.exit(1)

    print(f"  Built: bfs, pr, converter")
    return install_dir


def generate_graph(gapbs_dir, scale, out_file):
    """Generate a synthetic RMAT graph using gapbs converter."""
    gapbs_dir = Path(gapbs_dir)
    out_file = Path(out_file)

    if out_file.exists():
        print(f"  Graph {out_file} already exists, reusing")
        return

    print(f"  Generating RMAT graph (scale={scale}) → {out_file}...")
    subprocess.run(
        [str(gapbs_dir / "converter"), "-g", str(scale), "-b", str(out_file)],
        check=True, timeout=600,
    )
    size_mb = out_file.stat().st_size / (1024 * 1024)
    print(f"    Graph file: {size_mb:.0f} MB")


# ── Benchmark driver ───────��─────────────────────────────────────────────────

def run_gap_benchmark(gapbs_dir, graph_file, algorithm, smash_lib=None,
                      num_trials=16):
    """Run a GAP algorithm and measure RSS throughout.

    gapbs loads the graph once into memory, then runs num_trials iterations.
    We sample RSS externally every 0.5s. With Smash interposed:
    - The graph adjacency arrays are malloc'd and large
    - During BFS, only frontier-adjacent pages are hot
    - Smash compresses the cold majority of the graph between iterations
    - We should see RSS drop during later trials
    """
    gapbs_dir = Path(gapbs_dir)
    graph_file = Path(graph_file)

    algo_bin = gapbs_dir / algorithm
    if not algo_bin.exists():
        print(f"  ERROR: {algo_bin} not found")
        return None

    env = os.environ.copy()
    if smash_lib:
        env["LD_PRELOAD"] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"

    config_name = "smash" if smash_lib else "baseline"
    print(f"  Running {algorithm} -n {num_trials} ({config_name})...")

    cmd = [str(algo_bin), "-f", str(graph_file), "-n", str(num_trials)]
    proc = subprocess.Popen(
        cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )

    # Sample RSS during execution at 0.5s intervals
    rss_timeline = []
    timestamps = []
    t_start = time.time()
    while proc.poll() is None:
        rss = get_rss_mb(proc.pid)
        if rss > 0:
            rss_timeline.append(rss)
            timestamps.append(time.time() - t_start)
        time.sleep(0.5)

    elapsed = time.time() - t_start
    stdout, stderr = proc.communicate(timeout=10)

    if proc.returncode != 0:
        print(f"    ERROR: {algorithm} exited with code {proc.returncode}")
        print(f"    stderr: {stderr[:500]}")
        return None

    # Parse timing from gapbs output
    trial_times = []
    for line in stdout.splitlines():
        if "Trial Time" in line:
            parts = line.split()
            for p in parts:
                try:
                    t = float(p.rstrip("s"))
                    if 0 < t < 10000:
                        trial_times.append(t)
                except ValueError:
                    pass

    avg_time = sum(trial_times) / len(trial_times) if trial_times else 0

    # Compute RSS metrics — filter out startup samples (before graph is loaded)
    if not rss_timeline:
        print(f"    WARNING: No RSS samples collected (process too short)")
        return None

    peak_rss = max(rss_timeline)
    # Filter: only consider samples above 50% of peak (graph loaded)
    loaded_samples = [r for r in rss_timeline if r > peak_rss * 0.5]
    if not loaded_samples:
        loaded_samples = rss_timeline

    min_rss = min(loaded_samples)
    avg_rss = sum(loaded_samples) / len(loaded_samples)
    # "Steady state" RSS = average of the last third of samples
    # (after Smash has had time to compress)
    n = len(rss_timeline)
    steady_samples = rss_timeline[2 * n // 3:]
    steady_rss = sum(steady_samples) / len(steady_samples) if steady_samples else avg_rss

    reduction = (1 - min_rss / peak_rss) * 100 if peak_rss > 0 else 0
    steady_reduction = (1 - steady_rss / peak_rss) * 100 if peak_rss > 0 else 0
    auc = sum(rss_timeline) * 0.5  # each sample is ~0.5s apart

    metrics = {
        "algorithm": algorithm,
        "num_trials": num_trials,
        "peak_rss_mb": peak_rss,
        "min_rss_mb": min_rss,
        "avg_rss_mb": avg_rss,
        "steady_rss_mb": steady_rss,
        "rss_reduction_pct": reduction,
        "steady_reduction_pct": steady_reduction,
        "avg_trial_time_sec": avg_time,
        "total_time_sec": elapsed,
        "num_rss_samples": n,
        "rss_timeline": rss_timeline,
        "auc_mb_sec": auc,
    }

    print(f"    peak={peak_rss:.0f} MB, min={min_rss:.0f} MB, "
          f"steady={steady_rss:.0f} MB ({steady_reduction:.1f}% reduction), "
          f"avg_trial={avg_time:.3f}s, {n} RSS samples over {elapsed:.1f}s")

    return metrics


# ── Main ────────��─────────────────────────────���──────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="GAP Benchmark Suite benchmark for Smash")
    parser.add_argument("--quick", action="store_true",
                        help="Smaller graph, fewer trials")
    parser.add_argument("--skip-build", action="store_true",
                        help="Use already-built gapbs")
    parser.add_argument("--gapbs-dir", default=os.path.expanduser("~/gapbs"),
                        help="gapbs install directory")
    parser.add_argument("--smash-lib", default=None,
                        help="Path to libsmash.so")
    parser.add_argument("--scale", type=int, default=None,
                        help="Graph scale (2^scale vertices). Default: 23 (full) or 20 (quick)")
    parser.add_argument("--trials", type=int, default=None,
                        help="Trials per run (default: 16 full, 8 quick)")
    parser.add_argument("--algorithms", default="bfs,pr",
                        help="Comma-separated list of algorithms to run")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--output-dir", default="paper_results")
    args = parser.parse_args()

    gapbs_dir = Path(args.gapbs_dir)

    # Build gapbs
    if not args.skip_build or not (gapbs_dir / "bfs").exists():
        build_gapbs(gapbs_dir)

    # Graph scale:
    #   scale=22 → ~4M vertices, ~64M edges → ~800 MB-1 GB RSS (quick)
    #   scale=24 → ~16M vertices, ~256M edges → ~3-4 GB RSS (full)
    # Need large enough graphs that each trial takes seconds, not milliseconds,
    # so Smash has time to compress cold pages between iterations.
    if args.scale:
        scale = args.scale
    elif args.quick:
        scale = 22
    else:
        scale = 24

    num_trials = args.trials or (8 if args.quick else 16)

    # Generate graph
    graph_dir = gapbs_dir / "graphs"
    graph_dir.mkdir(exist_ok=True)
    graph_file = graph_dir / f"rmat_s{scale}.sg"
    generate_graph(gapbs_dir, scale, graph_file)

    # Find libsmash.so
    build_dir = Path(".").resolve()
    if args.smash_lib:
        smash_lib = Path(args.smash_lib).resolve()
    else:
        smash_lib = build_dir / "libsmash.so"
        if not smash_lib.exists():
            print(f"WARNING: {smash_lib} not found, will only run baseline")
            smash_lib = None

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    algorithms = [a.strip() for a in args.algorithms.split(",")]
    all_results = {}

    for algo in algorithms:
        print(f"\n{'='*60}")
        print(f"  GAP {algo.upper()} (scale={scale}, trials={num_trials})")
        print(f"{'='*60}")

        configs = [
            ("baseline", None),
        ]  # type: list[tuple[str, str | None]]
        if smash_lib:
            configs.append(("smash", str(smash_lib)))

        algo_results = {}
        for config_name, lib in configs:
            run_results = []
            for run_idx in range(args.runs):
                if args.runs > 1:
                    print(f"\n  --- {config_name} run {run_idx+1}/{args.runs} ---")

                metrics = run_gap_benchmark(
                    gapbs_dir, graph_file, algo, smash_lib=lib,
                    num_trials=num_trials,
                )
                if metrics:
                    run_results.append(metrics)

            if run_results:
                # Median of key metrics
                median_metrics = {}
                for key in ["peak_rss_mb", "min_rss_mb", "avg_rss_mb",
                            "steady_rss_mb", "rss_reduction_pct",
                            "steady_reduction_pct", "auc_mb_sec",
                            "avg_trial_time_sec", "total_time_sec"]:
                    vals = sorted(r[key] for r in run_results if key in r)
                    if vals:
                        median_metrics[key] = vals[len(vals) // 2]

                algo_results[config_name] = {
                    "median": median_metrics,
                    "runs": [{k: v for k, v in r.items() if k != "rss_timeline"}
                             for r in run_results],
                }

        all_results[algo] = algo_results

    # Print summary
    print(f"\n{'='*60}")
    print(f"  GAP BENCHMARK RESULTS (scale={scale}, {num_trials} trials)")
    print(f"{'='*60}")

    for algo, algo_results in all_results.items():
        print(f"\n  {algo.upper()}:")
        for config_name, data in algo_results.items():
            m = data["median"]
            print(f"    {config_name:12s}: peak={m.get('peak_rss_mb',0):.0f} MB, "
                  f"steady={m.get('steady_rss_mb',0):.0f} MB "
                  f"({m.get('steady_reduction_pct',0):.1f}% red), "
                  f"min={m.get('min_rss_mb',0):.0f} MB, "
                  f"trial={m.get('avg_trial_time_sec',0):.3f}s")

        if "baseline" in algo_results and "smash" in algo_results:
            b = algo_results["baseline"]["median"]
            s = algo_results["smash"]["median"]
            base_peak = b.get("peak_rss_mb", 0)
            smash_steady = s.get("steady_rss_mb", 0)
            if base_peak > 0 and smash_steady > 0:
                vs_base = (1 - smash_steady / base_peak) * 100
                base_time = b.get("avg_trial_time_sec", 0)
                smash_time = s.get("avg_trial_time_sec", 0)
                overhead = ((smash_time / base_time) - 1) * 100 if base_time > 0 else 0
                print(f"    --> Smash steady vs baseline peak: {vs_base:.1f}% RSS reduction, "
                      f"{overhead:+.1f}% time overhead")

    # Save results
    out_file = output_dir / "gap_results.json"
    with open(out_file, "w") as f:
        json.dump(all_results, f, indent=2)
    print(f"\n  Results saved to {out_file}")

    # METRIC lines
    for algo, algo_results in all_results.items():
        config = "smash" if "smash" in algo_results else "baseline"
        if config in algo_results:
            m = algo_results[config]["median"]
            for key, val in m.items():
                if isinstance(val, (int, float)):
                    print(f"METRIC gap_{algo}_{key} {val}")


if __name__ == "__main__":
    main()
