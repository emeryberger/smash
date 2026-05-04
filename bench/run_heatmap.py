#!/usr/bin/env python3
"""Run the coldness × compressibility heatmap experiment.

Runs bench_heatmap with and without Smash for each (hot%, compress%) combination,
then outputs a comparison showing where Smash provides the most benefit.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

def run_benchmark(exe, smash_lib, hot_pct, compress_pct, size_mb=512, cool_sec=15, serve_sec=10):
    """Run single benchmark point and return results."""
    env = os.environ.copy()
    if smash_lib:
        env["LD_PRELOAD"] = str(smash_lib)

    cmd = [str(exe), "--json", f"--single", f"{hot_pct},{compress_pct}",
           "--size", str(size_mb), "--cool", str(cool_sec), "--serve", str(serve_sec)]

    try:
        # Longer timeout for larger sizes
        timeout = max(180, cool_sec + serve_sec + 60)
        result = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)
        if result.returncode != 0:
            print(f"  Error: {result.stderr}", file=sys.stderr)
            return None
        data = json.loads(result.stdout)
        if data.get("results"):
            return data["results"][0]
    except Exception as e:
        print(f"  Exception: {e}", file=sys.stderr)
    return None


def main():
    parser = argparse.ArgumentParser(description="Run heatmap experiment")
    parser.add_argument("--build-dir", default=".", help="Build directory")
    parser.add_argument("--output", default="heatmap_results.json", help="Output file")
    parser.add_argument("--size", type=int, default=1024, help="Data size in MB (default: 1024)")
    parser.add_argument("--cool", type=int, default=20, help="Cooling period in seconds (default: 20)")
    parser.add_argument("--serve", type=int, default=10, help="Serve period in seconds (default: 10)")
    parser.add_argument("--quick", action="store_true", help="Quick mode (256MB, 5s cool)")
    args = parser.parse_args()

    if args.quick:
        args.size = 256
        args.cool = 5
        args.serve = 5

    build_dir = Path(args.build_dir).resolve()
    exe = build_dir / "bench" / "bench_heatmap"
    smash_lib = build_dir / "libsmash.so"

    if not exe.exists():
        print(f"Error: {exe} not found. Build with: make bench_heatmap", file=sys.stderr)
        sys.exit(1)

    if not smash_lib.exists():
        print(f"Error: {smash_lib} not found", file=sys.stderr)
        sys.exit(1)

    # Sweep parameters
    hot_fractions = [0, 10, 25, 50, 75, 90, 100]
    compressibilities = [0, 25, 50, 75, 100]

    results = {"baseline": [], "smash": []}

    print(f"Running heatmap experiment: {len(hot_fractions)} x {len(compressibilities)} x 2 = "
          f"{len(hot_fractions) * len(compressibilities) * 2} runs")
    print(f"Config: {args.size}MB data, {args.cool}s cool, {args.serve}s serve")
    print()

    for label, lib in [("baseline", None), ("smash", smash_lib)]:
        print(f"=== {label.upper()} ===")
        for compress_pct in compressibilities:
            for hot_pct in hot_fractions:
                print(f"  hot={hot_pct:3d}% compress={compress_pct:3d}% ... ", end="", flush=True)
                r = run_benchmark(exe, lib, hot_pct, compress_pct,
                                  size_mb=args.size, cool_sec=args.cool, serve_sec=args.serve)
                if r:
                    print(f"peak={r['peak_rss_mb']:.0f}MB min={r['min_rss_mb']:.0f}MB")
                    results[label].append(r)
                else:
                    print("FAILED")
        print()

    # Save raw results
    output_path = Path(args.output)
    output_path.write_text(json.dumps(results, indent=2))
    print(f"Raw results saved to: {output_path}")

    # Print comparison heatmap
    print("\n" + "=" * 70)
    print("SMASH BENEFIT HEATMAP (Steady RSS reduction vs baseline)")
    print("=" * 70)
    print()
    print("                        Compressibility")
    print("Hot %    ", end="")
    for c in compressibilities:
        print(f"{c:>8d}%", end="")
    print()
    print("        ", "-" * (9 * len(compressibilities)))

    for hot_pct in hot_fractions:
        print(f"{hot_pct:5d}%  |", end="")
        for compress_pct in compressibilities:
            # Find matching results
            base = next((r for r in results["baseline"]
                        if r["hot_pct"] == hot_pct and r["compress_pct"] == compress_pct), None)
            smash = next((r for r in results["smash"]
                         if r["hot_pct"] == hot_pct and r["compress_pct"] == compress_pct), None)

            if base and smash:
                base_rss = base["steady_rss_mb"]
                smash_rss = smash["steady_rss_mb"]
                if base_rss > 0:
                    benefit = 100 * (base_rss - smash_rss) / base_rss
                    # Color code: >40% green, 10-40% yellow, <10% red
                    if benefit > 40:
                        marker = "***"
                    elif benefit > 10:
                        marker = " * "
                    elif benefit > 0:
                        marker = "   "
                    else:
                        marker = " - "
                    print(f"{benefit:>6.1f}%{marker[0]}", end="")
                else:
                    print("     N/A", end="")
            else:
                print("   ERROR", end="")
        print()

    print()
    print("Key: * = Smash provides >10% benefit, *** = >40% benefit, - = overhead")
    print("     Best results: low hot% (cold data) + high compress% (compressible)")
    print()

    # Summary statistics
    benefits = []
    for hot_pct in hot_fractions:
        for compress_pct in compressibilities:
            base = next((r for r in results["baseline"]
                        if r["hot_pct"] == hot_pct and r["compress_pct"] == compress_pct), None)
            smash = next((r for r in results["smash"]
                         if r["hot_pct"] == hot_pct and r["compress_pct"] == compress_pct), None)
            if base and smash and base["steady_rss_mb"] > 0:
                benefit = 100 * (base["steady_rss_mb"] - smash["steady_rss_mb"]) / base["steady_rss_mb"]
                benefits.append((hot_pct, compress_pct, benefit))

    benefits.sort(key=lambda x: -x[2])

    print("Top 5 configurations for Smash:")
    for hot, comp, ben in benefits[:5]:
        print(f"  hot={hot:3d}%, compress={comp:3d}% -> {ben:+.1f}% RSS reduction")

    print("\nBottom 5 configurations (Smash overhead):")
    for hot, comp, ben in benefits[-5:]:
        print(f"  hot={hot:3d}%, compress={comp:3d}% -> {ben:+.1f}% RSS {'reduction' if ben > 0 else 'overhead'}")


if __name__ == "__main__":
    main()
