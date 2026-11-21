#!/usr/bin/env python3
"""A/B: does turning SMASH_TRACK_EXTERNAL on cost anything on malloc-only workloads?

Runs the two quick-CI benches under the preloaded libsmash with
SMASH_TRACK_EXTERNAL=0 and =1, in full and large-only mode, interleaved in a
random order for --runs repetitions, and reports per-config medians:

  bench_rss --wait 20      best RSS reduction over the window (%)
  bench_sqlite --quick     rss_reduction_pct, ops_per_sec, fill_time_sec,
                           hot_p99_us, peak_rss_mb
  both                     child CPU seconds (user+sys) and wall seconds

    python3 bench/ab_track_external.py --build-dir build-linux --runs 5
"""
import argparse
import os
import random
import re
import resource
import statistics
import subprocess
import sys
import time


def run(cmd, env, timeout=300):
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.time()
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace",
                           timeout=timeout)
        rc, out = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        rc = "timeout"
        out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
    wall = time.time() - t0
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)
    return rc, out, wall, cpu


def metric(out, name):
    m = re.search(r"^METRIC\s+%s\s+(-?[\d.]+)\s*$" % re.escape(name), out, re.MULTILINE)
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--modes", default="0,1", help="SMASH_LARGE_ONLY values")
    ap.add_argument("--settings", default="0,1",
                    help="SMASH_TRACK_EXTERNAL values; 'auto' leaves it unset")
    ap.add_argument("--sqlite-modes", default=None,
                    help="SMASH_LARGE_ONLY values for bench_sqlite (default: --modes)")
    a = ap.parse_args()
    bd = os.path.abspath(a.build_dir)
    lib = os.path.join(bd, "libsmash.dylib" if sys.platform == "darwin" else "libsmash.so")
    preload = "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"

    sq_modes = (a.sqlite_modes or a.modes).split(",")
    jobs = [(bench, lo, te, r) for r in range(a.runs) for bench in ("rss", "sqlite")
            for lo in (a.modes.split(",") if bench == "rss" else sq_modes) for te in a.settings.split(",")]
    random.shuffle(jobs)
    rows = {}
    failures = 0
    for bench, lo, te, r in jobs:
        env = dict(os.environ)
        env.update({preload: lib, "SMASH_LARGE_ONLY": lo, "SMASH_TRACK_EXTERNAL": te,
                    "SMASH_COLD_TIMEOUT_SEC": "2"})
        if te == "auto":  # unset: smash decides (on iff userfaultfd reclaim)
            env.pop("SMASH_TRACK_EXTERNAL")
        if sys.platform == "darwin":
            env["MallocNanoZone"] = "0"
        if bench == "rss":
            rc, out, wall, cpu = run([os.path.join(bd, "bench", "bench_rss"), "--wait", "20"], env)
            red = [int(x) for x in re.findall(
                r"^\s*t=\s*\d+s:\s+RSS=[\d.]+ MB\s+\((-?\d+)% reduction", out, re.MULTILINE)]
            vals = {"best_reduction_pct": max(red) if red else None}
        else:
            rc, out, wall, cpu = run([os.path.join(bd, "bench", "bench_sqlite"), "--quick"], env)
            vals = {k: metric(out, k) for k in ("rss_reduction_pct", "ops_per_sec", "fill_time_sec",
                                               "hot_p99_us", "peak_rss_mb")}
        vals["cpu_s"] = cpu
        vals["wall_s"] = wall
        if rc != 0:
            failures += 1
            print(f"FAIL {bench} large_only={lo} track={te} rc={rc}: {out[-300:]}", flush=True)
        key = (bench, lo, te)
        rows.setdefault(key, []).append(vals)
        print(f"{bench:6s} large_only={lo} track={te} run{r} rc={rc} "
              + " ".join(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}"
                         for k, v in vals.items()), flush=True)

    print("\nmedians:")
    for key in sorted(rows):
        vs = rows[key]
        med = {}
        for k in vs[0]:
            xs = [v[k] for v in vs if v.get(k) is not None]
            med[k] = statistics.median(xs) if xs else None
        print(f"  {key[0]:6s} large_only={key[1]} track={key[2]} n={len(vs)} "
              + " ".join(f"{k}={v:.2f}" for k, v in med.items() if v is not None))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
