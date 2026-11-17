#!/usr/bin/env python3
"""Peak-RSS comparison: glibc vs smash vs smash + eager fill compression.

Capacity is provisioned for a process's PEAK resident set, so this harness
reports the peak alongside the time-averaged RSS (avgRSS = AUC / duration) and
the minimum, for the same binary under:

  glibc        no preload (system malloc)
  smash        LD_PRELOAD=libsmash.so
  smash_eager  LD_PRELOAD=libsmash.so SMASH_EAGER_FILL=1
  smash_guard  LD_PRELOAD=libsmash.so SMASH_PEAK_GUARD=1
  smash_both   LD_PRELOAD=libsmash.so SMASH_EAGER_FILL=1 SMASH_PEAK_GUARD=1

Each smash config runs in full mode and/or large-only mode (SMASH_LARGE_ONLY).
Runs are randomized and interleaved: every round runs each (config, mode)
once, in a fresh random order, so drift and the first-run cold penalty spread
over all configs instead of landing on one.

RSS is sampled externally from /proc/<pid>/statm every --sample-ms until the
bench prints "METRIC workload_done" (teardown is excluded). Peak is the max of
those samples and the bench's own VmHWM (exact kernel high-water mark). The
smash runs also log SMASH_PRESSURE_LOG, summed into compressions (enter),
refaults (read+write faults on compressed pages) and shadow discards.

Linux only (needs /proc and LD_PRELOAD). Examples:

    python3 bench/run_peak_compare.py --build-dir build --bench sqlite --runs 5
    python3 bench/run_peak_compare.py --bench sqlite --sqlite-rows 1000000 \\
        --cold-timeout 1 --modes full,large --runs 5
    python3 bench/run_peak_compare.py --bench rss --rss-fill-sec 8 --runs 5

Numbers depend on the host; label laptop/VM results as development numbers.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

PAGE = os.sysconf("SC_PAGE_SIZE") if hasattr(os, "sysconf") else 4096


def find_build_dir(arg: str | None) -> Path:
    if arg:
        return Path(arg).resolve()
    if os.environ.get("BUILD_DIR"):
        return Path(os.environ["BUILD_DIR"]).resolve()
    root = Path(__file__).resolve().parent.parent
    cands = [d for d in root.glob("build*") if (d / "libsmash.so").exists()
             and (d / "bench").is_dir()]
    if not cands:
        raise SystemExit("no build dir with libsmash.so + bench/ found; pass --build-dir")
    return max(cands, key=lambda d: (d / "libsmash.so").stat().st_mtime)


def bench_cmd(bench: str, build: Path, a: argparse.Namespace) -> list[str]:
    b = build / "bench"
    if bench == "sqlite":
        cmd = [str(b / "bench_sqlite")]
        if a.sqlite_rows:
            cmd += ["--rows", str(a.sqlite_rows), "--cool", str(a.sqlite_cool),
                    "--serve", str(a.sqlite_serve)]
        else:
            cmd += ["--quick"]
        if a.sqlite_batch:
            cmd += ["--batch", str(a.sqlite_batch)]
        return cmd
    if bench == "rss":
        return [str(b / "bench_rss"), "--total-mb", str(a.rss_total_mb),
                "--fill-sec", str(a.rss_fill_sec), "--wait", str(a.rss_wait)]
    if bench == "rocksdb":
        return [str(b / "bench_rocksdb")] + (a.rocksdb_args.split() if a.rocksdb_args else [])
    raise SystemExit(f"unknown bench {bench}")


def read_rss_kb(pid: int) -> int | None:
    try:
        with open(f"/proc/{pid}/statm") as f:
            return int(f.read().split()[1]) * PAGE // 1024
    except (OSError, IndexError, ValueError):
        return None


def summarize_pressure(path: Path) -> dict[str, int]:
    out = {"enter": 0, "refault_read": 0, "refault_write": 0, "shadow_discard": 0}
    if not path.exists():
        return out
    keys = {"enter": "enter", "rr": "refault_read", "rw": "refault_write",
            "sd": "shadow_discard"}
    for line in path.read_text(errors="replace").splitlines():
        if not line.startswith("tick "):
            continue
        for k, v in re.findall(r"\b(enter|rr|rw|sd)=(\d+)", line):
            out[keys[k]] += int(v)
    return out


def run_once(cmd: list[str], env: dict[str, str], sample_ms: int,
             timeout: int) -> dict:
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, bufsize=1)
    samples: list[tuple[float, int]] = []
    done = threading.Event()
    lines: list[str] = []

    def reader() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.append(line)
            if line.startswith("METRIC workload_done"):
                done.set()
        done.set()

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    while not done.is_set():
        kb = read_rss_kb(proc.pid)
        if kb is not None:
            samples.append((time.monotonic() - t0, kb))
        if time.monotonic() - t0 > timeout:
            proc.kill()
            raise RuntimeError(f"timeout: {' '.join(cmd)}")
        done.wait(sample_ms / 1000.0)
    t_end = time.monotonic() - t0
    rc = proc.wait(timeout=timeout)
    th.join(timeout=5)
    metrics: dict[str, float] = {}
    for line in lines:
        m = re.match(r"^METRIC\s+(\S+)\s+(-?[\d.]+)", line)
        if m:
            metrics[m.group(1)] = float(m.group(2))
    if rc != 0 or "workload_done" not in metrics:
        raise RuntimeError(f"rc={rc} (workload_done={'workload_done' in metrics}): {' '.join(cmd)}")
    # avgRSS = trapezoidal AUC over the workload / its duration.
    auc = 0.0
    for (ta, ra), (tb, rb) in zip(samples, samples[1:]):
        auc += (ra + rb) / 2.0 * (tb - ta)
    dur = samples[-1][0] - samples[0][0] if len(samples) > 1 else 0.0
    peak_kb = max((r for _, r in samples), default=0)
    peak_t = next((t for t, r in samples if r == peak_kb), 0.0)
    hwm_mb = metrics.get("hwm_rss_mb", 0.0)
    return {
        "peak_mb": max(peak_kb / 1024.0, hwm_mb),
        "sampled_peak_mb": peak_kb / 1024.0,
        "peak_t_s": peak_t,
        # Downsampled timeline (<= ~400 points) for plotting / inspection.
        "timeline": [(round(t, 3), r // 1024) for t, r in samples[::max(1, len(samples) // 400)]],
        "hwm_mb": hwm_mb,
        "avg_mb": (auc / dur / 1024.0) if dur > 0 else 0.0,
        "min_mb": metrics.get("min_rss_mb", min((r for _, r in samples), default=0) / 1024.0),
        "duration_s": t_end,
        "metrics": metrics,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir")
    ap.add_argument("--bench", default="sqlite", help="comma list: sqlite,rss,rocksdb")
    ap.add_argument("--configs", default="glibc,smash,smash_eager",
                    help="comma list of glibc,smash,smash_eager,smash_guard,smash_both")
    ap.add_argument("--modes", default="full,large", help="smash modes: full,large")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--cold-timeout", help="SMASH_COLD_TIMEOUT_SEC for smash configs "
                    "(default: smash's own default)")
    ap.add_argument("--eager-env", default="SMASH_EAGER_FILL=1",
                    help="space-separated VAR=VAL set for smash_eager")
    ap.add_argument("--extra-env", default="",
                    help="space-separated VAR=VAL set for every smash config")
    ap.add_argument("--sample-ms", type=int, default=20)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--sqlite-rows", type=int, default=0, help="0 = --quick (250K rows)")
    ap.add_argument("--sqlite-batch", type=int, default=0,
                    help="COMMIT every N fill rows (0 = one transaction)")
    ap.add_argument("--sqlite-cool", type=int, default=10)
    ap.add_argument("--sqlite-serve", type=int, default=20)
    ap.add_argument("--rss-total-mb", type=int, default=256)
    ap.add_argument("--rss-fill-sec", type=float, default=8.0)
    ap.add_argument("--rss-wait", type=int, default=15)
    ap.add_argument("--rocksdb-args", default="")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--json", type=Path, help="write all runs as JSON here")
    ap.add_argument("--log-dir", type=Path, help="pressure logs (default: <build>/peak_logs)")
    a = ap.parse_args()

    if not sys.platform.startswith("linux"):
        raise SystemExit("Linux only (uses /proc and LD_PRELOAD)")
    build = find_build_dir(a.build_dir)
    lib = build / "libsmash.so"
    log_dir = a.log_dir or (build / "peak_logs")
    log_dir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(a.seed)

    def parse_env(s: str) -> dict[str, str]:
        return dict(kv.split("=", 1) for kv in s.split() if "=" in kv)

    variants: list[tuple[str, str]] = []
    for c in a.configs.split(","):
        if c == "glibc":
            variants.append((c, "-"))
        else:
            variants += [(c, m) for m in a.modes.split(",")]

    results: list[dict] = []
    for bench in a.bench.split(","):
        cmd = bench_cmd(bench, build, a)
        for rnd in range(a.runs):
            order = variants[:]
            rng.shuffle(order)
            for cfg, mode in order:
                env = dict(os.environ)
                for k in [k for k in env if k.startswith("SMASH_")]:
                    del env[k]
                plog = None
                if cfg != "glibc":
                    env["LD_PRELOAD"] = str(lib)
                    env["SMASH_LARGE_ONLY"] = "1" if mode == "large" else "0"
                    if a.cold_timeout:
                        env["SMASH_COLD_TIMEOUT_SEC"] = a.cold_timeout
                    env.update(parse_env(a.extra_env))
                    if cfg in ("smash_eager", "smash_both"):
                        env.update(parse_env(a.eager_env))
                    if cfg in ("smash_guard", "smash_both"):
                        env["SMASH_PEAK_GUARD"] = "1"
                    plog = log_dir / f"{bench}_{cfg}_{mode}_{rnd}.log"
                    plog.unlink(missing_ok=True)
                    env["SMASH_PRESSURE_LOG"] = str(plog)
                try:
                    r = run_once(cmd, env, a.sample_ms, a.timeout)
                except RuntimeError as e:
                    print(f"  [{bench} r{rnd} {cfg}/{mode}] FAILED: {e}", flush=True)
                    results.append({"bench": bench, "config": cfg, "mode": mode,
                                    "round": rnd, "error": str(e)})
                    continue
                r.update({"bench": bench, "config": cfg, "mode": mode, "round": rnd})
                if plog is not None:
                    r["pressure"] = summarize_pressure(plog)
                results.append(r)
                m = r["metrics"]
                extra = ""
                if "ops_per_sec" in m:
                    extra += f" ops/s={m['ops_per_sec']:.0f}"
                if "fill_time_sec" in m:
                    extra += f" fill={m['fill_time_sec']:.2f}s"
                if "pressure" in r:
                    p = r["pressure"]
                    extra += (f" comp={p['enter']} refault={p['refault_read'] + p['refault_write']}"
                              f" sd={p['shadow_discard']}")
                print(f"  [{bench} r{rnd} {cfg:11s} {mode:5s}] peak={r['peak_mb']:.1f}@{r['peak_t_s']:.1f}s "
                      f"avg={r['avg_mb']:.1f} min={r['min_mb']:.1f} MB{extra}", flush=True)

    # Medians per (bench, config, mode).
    print()
    hdr = (f"{'bench':8s} {'config':11s} {'mode':5s} {'n':>2s} {'peak MB':>8s} {'avg MB':>8s} "
           f"{'min MB':>8s} {'fill s':>7s} {'ops/s':>9s} {'comp':>8s} {'refault':>8s} {'sd':>7s}")
    print(hdr)
    print("-" * len(hdr))
    for bench in a.bench.split(","):
        for cfg, mode in variants:
            rs = [r for r in results if r["bench"] == bench and r["config"] == cfg
                  and r["mode"] == mode and "error" not in r]
            if not rs:
                continue
            med = lambda f: statistics.median(f(r) for r in rs)  # noqa: E731
            fill = med(lambda r: r["metrics"].get("fill_time_sec", 0.0))
            ops = med(lambda r: r["metrics"].get("ops_per_sec", 0.0))
            comp = med(lambda r: r.get("pressure", {}).get("enter", 0))
            ref = med(lambda r: r.get("pressure", {}).get("refault_read", 0)
                      + r.get("pressure", {}).get("refault_write", 0))
            sd = med(lambda r: r.get("pressure", {}).get("shadow_discard", 0))
            print(f"{bench:8s} {cfg:11s} {mode:5s} {len(rs):2d} {med(lambda r: r['peak_mb']):8.1f} "
                  f"{med(lambda r: r['avg_mb']):8.1f} {med(lambda r: r['min_mb']):8.1f} "
                  f"{fill:7.2f} {ops:9.0f} {comp:8.0f} {ref:8.0f} {sd:7.0f}")
    if a.json:
        a.json.write_text(json.dumps({"argv": sys.argv, "results": results}, indent=1))
        print(f"\nwrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
