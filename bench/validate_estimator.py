#!/usr/bin/env python3
"""Compare smash's savings estimators against a real smash run (Linux).

For bench_sqlite (fill -> cool -> hot-only serve), at the same point of the
serve phase (serve start + --at seconds), measure:

  glibc     RSS without smash, plus `smash-estimate --window W <pid>` started
            W seconds earlier (projected saving, % of that RSS)
  observe   LD_PRELOAD libsmash SMASH_MODE=observe: RSS and the projected
            saving from SMASH_OBSERVE_REPORT
  smash     LD_PRELOAD libsmash (full mode): RSS

and report the gap between each estimate and the measured reduction
(1 - smash_rss / glibc_rss, and 1 - smash_rss / observe_rss, which holds the
allocator fixed). Configs are interleaved in random order for --runs rounds.

  python3 bench/validate_estimator.py --build-dir build-bench --runs 5

Needs -DSMASH_BUILD_BENCH=ON. smash-estimate attaches to a non-child pid, so
reading page contents needs ptrace access: the script runs it with `sudo -n`
when that works (use --no-sudo to skip).
"""
import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import tempfile
import time


def rss_bytes(pid):
    with open("/proc/%d/statm" % pid) as f:
        return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")


def wait_line(proc, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        line = proc.stderr.readline()
        if not line:
            raise RuntimeError("bench exited before '%s'" % needle)
        if needle in line:
            return time.time()
    raise RuntimeError("timeout waiting for '%s'" % needle)


def drain(proc):
    # Keep the pipe from filling while we wait.
    import threading

    def run():
        for _ in proc.stderr:
            pass
    th = threading.Thread(target=run, daemon=True)
    th.start()
    return th


def run_one(cfg, a, bench, lib, est, workdir):
    env = dict(os.environ)
    report = os.path.join(workdir, "observe-%d.json" % os.getpid())
    if cfg in ("observe", "smash"):
        env["LD_PRELOAD"] = lib
    if cfg == "observe":
        env["SMASH_MODE"] = "observe"
        env["SMASH_OBSERVE_REPORT"] = report
        env["SMASH_OBSERVE_INTERVAL"] = "1"
        if os.path.exists(report):
            os.unlink(report)
    args = [bench, "--rows", str(a.rows), "--cool", str(a.cool), "--serve", str(a.serve)]
    proc = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            universal_newlines=True)
    out = {"config": cfg}
    try:
        t_serve = wait_line(proc, "Serve phase", 600)
        drain(proc)
        est_proc = None
        if cfg == "glibc":
            time.sleep(max(0.0, a.at - a.window - (time.time() - t_serve)))
            cmd = [est, "--json", "--window", str(a.window), "--points", "1", str(proc.pid)]
            if a.sudo:
                cmd = ["sudo", "-n"] + cmd
            est_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        universal_newlines=True)
        time.sleep(max(0.0, a.at - (time.time() - t_serve)))
        out["rss"] = rss_bytes(proc.pid)
        if cfg == "observe":
            with open(report) as f:
                js = json.load(f)
            out["proj_saving"] = js["projected_saving_bytes"]
            out["obs_rss"] = js["rss_bytes"]
            out["write_tracking"] = js["write_tracking"]
        if est_proc:
            so, se = est_proc.communicate(timeout=a.window + 120)
            js = json.loads(so)
            out["est_saving"] = js["projected_saving_bytes"]
            out["est_ci"] = js["ci95_bytes"]
            out["est_method"] = js["method"]
            out["est_rss"] = js["rss_bytes"]
    finally:
        proc.kill()
        proc.wait()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build-bench")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--rows", type=int, default=500000)
    ap.add_argument("--cool", type=int, default=5)
    ap.add_argument("--serve", type=int, default=60)
    # bench_sqlite's in-memory pager rehashes its page cache as the DB grows
    # (an access to every page); with the defaults one lands ~30 s into the
    # serve phase. Measure before it so all configs see the same steady state.
    ap.add_argument("--at", type=float, default=25.0, help="seconds into the serve phase to measure")
    ap.add_argument("--window", type=float, default=10.0, help="smash-estimate idle window")
    ap.add_argument("--no-sudo", dest="sudo", action="store_false")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json-out")
    a = ap.parse_args()
    bd = os.path.abspath(a.build_dir)
    bench = os.path.join(bd, "bench", "bench_sqlite")
    lib = os.path.join(bd, "libsmash.so")
    est = os.path.join(bd, "smash-estimate")
    for p in (bench, lib, est):
        if not os.path.exists(p):
            sys.exit("missing %s (configure with -DSMASH_BUILD_BENCH=ON)" % p)
    if a.sudo and subprocess.call(["sudo", "-n", "true"], stderr=subprocess.DEVNULL) != 0:
        a.sudo = False
    rnd = random.Random(a.seed)
    res = {"glibc": [], "observe": [], "smash": []}
    os.makedirs("tmp", exist_ok=True)
    with tempfile.TemporaryDirectory(dir="tmp") as wd:
        for r in range(a.runs):
            order = list(res)
            rnd.shuffle(order)
            for cfg in order:
                o = run_one(cfg, a, bench, lib, est, wd)
                print("run %d %s %s" % (r, cfg, json.dumps(o)), flush=True)
                res[cfg].append(o)

    def med(xs):
        return statistics.median(xs) if xs else float("nan")
    g_rss = med([o["rss"] for o in res["glibc"]])
    o_rss = med([o["rss"] for o in res["observe"]])
    s_rss = med([o["rss"] for o in res["smash"]])
    est_pct = med([100.0 * o["est_saving"] / o["est_rss"] for o in res["glibc"]])
    obs_pct = med([100.0 * o["proj_saving"] / o["obs_rss"] for o in res["observe"]])
    act_g = 100.0 * (1 - s_rss / g_rss)
    act_o = 100.0 * (1 - s_rss / o_rss)
    summary = {
        "runs": a.runs, "rows": a.rows, "at_serve_s": a.at, "window_s": a.window,
        "median_rss_mib": {"glibc": g_rss / 2**20, "observe": o_rss / 2**20, "smash": s_rss / 2**20},
        "smash_estimate_pct_of_glibc_rss": est_pct,
        "observe_projected_pct_of_observe_rss": obs_pct,
        "actual_reduction_vs_glibc_pct": act_g,
        "actual_reduction_vs_observe_pct": act_o,
        "gap_estimate_minus_actual_vs_glibc_pp": est_pct - act_g,
        "gap_observe_minus_actual_vs_observe_pp": obs_pct - act_o,
        "est_method": res["glibc"][0].get("est_method") if res["glibc"] else None,
        "observe_write_tracking": res["observe"][0].get("write_tracking") if res["observe"] else None,
        "raw": res,
    }
    print(json.dumps({k: v for k, v in summary.items() if k != "raw"}, indent=1))
    if a.json_out:
        with open(a.json_out, "w") as f:
            json.dump(summary, f, indent=1)


if __name__ == "__main__":
    main()
