#!/usr/bin/env python3
"""Per-phase swap/fault timeline of capped bench_sqlite runs.

  sudo -E python3 bench/cgroup_phase_probe.py OUT.json NAME=lib[,ENV=V..] ...

An empty lib (`glibc=`) runs without a preload. Each config runs once per
rep (order as given; REPS=N env), in a fresh cgroup capped at CAP=HIGH:MAX
(default 250M:400M), bench_sqlite from BUILD_DIR/bench (default
build-linux), ROWS rows. Every 0.1 s it samples memory.current,
memory.swap.current, the zswap pool, memory.stat (pgmajfault, zswpin,
zswpout) and the host's pswpin/pswpout (disk swap; not per-cgroup, so run
nothing else that swaps). The bench's stderr lines are timestamped, so the
per-phase deltas (fill / cool / serve / cold) printed per run line up with
the timeline saved in OUT.json. See agents/containers.md ("Round 2").
"""
import json, os, sys, threading, time, subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
import run_cgroup_compare as rc  # noqa: E402


def main():
    out = sys.argv[1]
    cfgs = [rc.parse_config(c) for c in sys.argv[2:]]
    reps = int(os.environ.get("REPS", 1))
    rows = os.environ.get("ROWS", "500000")
    high, mx = os.environ.get("CAP", "250M:400M").split(":")
    bdir = os.environ.get("BUILD_DIR", "build-linux")
    res = []
    for rep in range(reps):
        for name, cfg in cfgs:
            cg = rc.Cgroup(f"probe-{name}-{rep}-{os.getpid()}", high, mx)
            env = dict(os.environ)
            env.pop("LD_PRELOAD", None)
            if cfg.get("lib"):
                env["LD_PRELOAD"] = str(Path(cfg["lib"]).resolve())
            env.update(cfg["env"])
            tl, ev = [], []
            stop = threading.Event()
            t0 = time.monotonic()

            def samp():
                while not stop.is_set():
                    st = cg.stat()
                    vm = rc.keyed(rc.read(Path("/proc/vmstat")))
                    tl.append([round(time.monotonic() - t0, 2),
                               rc.read_int(cg.path / "memory.current") >> 20,
                               max(rc.read_int(cg.path / "memory.swap.current"), 0) >> 20,
                               max(rc.read_int(cg.path / "memory.zswap.current"), 0) >> 20,
                               st.get("pgmajfault", 0), st.get("zswpin", 0), st.get("zswpout", 0),
                               vm.get("pswpin", 0), vm.get("pswpout", 0)])
                    stop.wait(0.1)
            th = threading.Thread(target=samp, daemon=True)
            th.start()
            p = subprocess.Popen([f"{bdir}/bench/bench_sqlite", "--rows", rows, "--cool", "30", "--serve", "20"],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                 preexec_fn=rc.join_cgroup(cg))
            for line in p.stderr:
                ev.append([round(time.monotonic() - t0, 2), line.rstrip()[:200]])
            so = p.stdout.read()
            p.wait()
            stop.set(); th.join()
            cg.remove()
            r = {"config": name, "rep": rep, "rc": p.returncode, "metrics": rc.parse_metrics(so),
                 "timeline": tl, "events": ev}
            res.append(r)
            # phase deltas
            marks = {"fill": 0.0}
            for t, l in ev:
                if l.startswith("Cooling") and "cool" not in marks: marks["cool"] = t
                if l.startswith("Serve phase"): marks["serve"] = t
                if l.startswith("Cold re-access"): marks["cold"] = t
            marks["end"] = tl[-1][0]
            order = [k for k in ("fill", "cool", "serve", "cold", "end") if k in marks]

            def at(t):
                best = tl[0]
                for row in tl:
                    if row[0] <= t: best = row
                return best
            summ = {}
            for a, b in zip(order, order[1:]):
                ra, rb = at(marks[a]), at(marks[b])
                summ[a] = {"dur": round(marks[b] - marks[a], 1), "majflt": rb[4] - ra[4],
                           "zswpin": rb[5] - ra[5], "zswpout": rb[6] - ra[6], "pswpin": rb[7] - ra[7],
                           "pswpout": rb[8] - ra[8], "cur_end": rb[1], "swap_end": rb[2]}
            r["phases"] = summ
            print(name, rep, "cold_p99=%.0f" % r["metrics"].get("cold_p99_us", 0),
                  json.dumps(summ), flush=True)
            json.dump(res, open(out, "w"))


main()
