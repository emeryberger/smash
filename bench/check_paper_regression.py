#!/usr/bin/env python3
"""Gate the paper workloads' RSS reduction against regression floors.

Reads the compress_only_results.json written by
    run_paper_experiments.py --compress-only-only --configs full_smash
and compares each app's full-smash median rss_reduction_pct (the README /
paper metric: peak -> minimum RSS during cooling, within the smash run) with:

  paper : the published figure (README "Results" table, EPYC 9R14).  Falling
          short only prints a WARN; the CI runner is a different machine.
  floor : the regression gate.  Below it the check fails (exit 1).  Floors sit
          about 2 points under what main measures on GitHub's ubuntu-latest
          runner (run-to-run spread there is a few tenths of a point), so a
          real regression -- e.g. Redis-ext dropping to ~4% when shadow reclaim
          stopped verifying pages with freed slots (#97) -- fails loudly.

Raise a floor when a change durably improves an app; lower one only with a
written reason in the commit message.

Usage:
    python3 bench/check_paper_regression.py RESULTS_JSON [--summary FILE]
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys

# app -> (paper %, CI floor %).  Floors calibrated 2026-11-13 on master
# d54ceca; ubuntu-latest measured (3 runs each) SQLite 73.2, RocksDB 84.9-85.0,
# memcached 88.0, Redis 56.6, Redis-ext 65.2, with spread <= 0.1 point.
EXPECT = {
    "sqlite":    (73.3, 71.0),
    "rocksdb":   (84.0, 83.0),
    "memcached": (87.8, 86.0),
    "redis":     (58.0, 54.5),
    "redis_ext": (68.7, 63.0),
}

LABEL = {
    "sqlite": "SQLite",
    "rocksdb": "RocksDB",
    "memcached": "memcached",
    "redis": "Redis",
    "redis_ext": "Redis-ext (50% DELETE)",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", help="compress_only_results.json")
    ap.add_argument("--summary", default=os.environ.get("GITHUB_STEP_SUMMARY"),
                    help="append a Markdown table here (default: $GITHUB_STEP_SUMMARY)")
    args = ap.parse_args()

    with open(args.results) as f:
        results = json.load(f)

    rows = []
    failed = []
    for app, (paper, floor) in EXPECT.items():
        runs = results.get(f"{app}_full_smash", {}).get("runs", [])
        vals = [r["rss_reduction_pct"] for r in runs
                if r.get("rss_reduction_pct") is not None]
        if not vals:
            rows.append((app, "-", None, paper, floor, "FAIL (no runs)"))
            failed.append(app)
            continue
        med = statistics.median(vals)
        if med < floor:
            status = "FAIL"
            failed.append(app)
        elif med >= paper - 0.5:
            status = "ok (matches/beats paper)"
        else:
            status = "ok (WARN: below paper)"
        rows.append((app, ", ".join(f"{v:.1f}" for v in vals), med, paper, floor, status))

    hdr = f"{'app':<24} {'runs':<20} {'median':>7} {'paper':>6} {'floor':>6}  status"
    print(hdr)
    print("-" * len(hdr))
    for app, runs, med, paper, floor, status in rows:
        m = f"{med:.1f}" if med is not None else "-"
        print(f"{LABEL[app]:<24} {runs:<20} {m:>7} {paper:>6.1f} {floor:>6.1f}  {status}")

    if args.summary:
        with open(args.summary, "a") as f:
            f.write("### Paper workloads: RSS reduction (full smash)\n\n")
            f.write("| Workload | Runs (%) | Median | Paper | Floor | Status |\n")
            f.write("|---|---|--:|--:|--:|---|\n")
            for app, runs, med, paper, floor, status in rows:
                m = f"{med:.1f}" if med is not None else "-"
                f.write(f"| {LABEL[app]} | {runs} | {m} | {paper:.1f} | {floor:.1f} | {status} |\n")
            f.write("\n")

    if failed:
        print(f"\nFAIL: below regression floor: {', '.join(failed)}", file=sys.stderr)
        return 1
    print("\nAll paper workloads at or above their regression floors.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
