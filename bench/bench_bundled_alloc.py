#!/usr/bin/env python3
"""Does smash reach memory that does NOT come from the interposed malloc?

Workloads whose heap is managed by an allocator smash does not replace:
  valkey   Valkey built with its default bundled jemalloc (MALLOC=jemalloc)
  mi       bundled_alloc_workload linked to mimalloc, calling mi_malloc
  tc       bundled_alloc_workload linked to gperftools tcmalloc, calling tc_malloc
  python   CPython (system python3) dict-heavy workload; obmalloc arenas are mmap'd

Each is run without smash ("none") and under LD_PRELOAD=libsmash with
SMASH_TRACK_EXTERNAL=0 and =1, in full and large-only mode. Reports peak RSS
(sampled at the end of the fill), the minimum RSS over the cool phase, the
reduction 1 - min/peak, and an integrity verdict:
  valkey   DEBUG DIGEST after fill == after cool (every value re-read), then
           BGSAVE (forked child serializes the possibly-compressed heap) +
           DEBUG RELOAD NOSAVE (load that RDB; the server log must confirm the
           reload) and the digest must still match
  others   every object re-read and compared (CHECK ok)

Linux only. Run order is randomized across --runs repetitions.

    python3 bench/bench_bundled_alloc.py --build-dir build-linux \\
        --valkey-dir tmp/deps/valkey/src --apps valkey,mi,tc,python --runs 3
"""
import argparse
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def rss_mb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    return 0.0


def smash_env(lib, mode, track, cold_timeout="1"):
    """track 'auto' leaves SMASH_TRACK_EXTERNAL unset; cold_timeout '' keeps
    smash's default cold timeout."""
    env = os.environ.copy()
    env.pop("SMASH_TRACK_EXTERNAL", None)
    if mode == "none":
        return env
    env["LD_PRELOAD"] = lib
    env["SMASH_LARGE_ONLY"] = "1" if mode == "large" else "0"
    if track != "auto":
        env["SMASH_TRACK_EXTERNAL"] = str(track)
    if cold_timeout:
        env["SMASH_COLD_TIMEOUT_SEC"] = cold_timeout
    return env


def vma_count(pid):
    try:
        with open(f"/proc/{pid}/maps", "rb") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def sample_cool(pid, cool, peak, stats=None):
    """Min RSS over the cool phase; stats (dict) also gets the max VMA count."""
    lo = peak
    vmax = 0
    for _ in range(cool):
        time.sleep(1)
        r = rss_mb(pid)
        if r > 0:
            lo = min(lo, r)
        vmax = max(vmax, vma_count(pid))
    if stats is not None:
        stats["vma_max"] = vmax
    return lo


def run_child_workload(cmd, env, cool):
    """Drive a FILLED / CHECK child, sampling its RSS during the cool phase."""
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace")
    t0 = time.time()
    line = p.stdout.readline()
    fill_s = time.time() - t0
    if not line.startswith("FILLED"):
        err = p.stderr.read()
        p.kill()
        return {"ok": False, "error": f"no FILLED (rc={p.wait()}): {err[-400:]}"}
    peak = rss_mb(p.pid)
    st = {"vma_fill": vma_count(p.pid)}
    lo = sample_cool(p.pid, cool, peak, st)
    out, err = p.communicate(timeout=600)
    check = out.strip().splitlines()[-1] if out.strip() else ""
    ok = check.startswith("CHECK ok") and p.returncode == 0
    return {"ok": ok, "peak_mb": peak, "min_mb": lo, "fill_s": fill_s,
            "check": check, "rc": p.returncode, **st,
            "error": "" if ok else err[-400:]}


def wait_port(port, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def run_valkey(vdir, env, cool, n_ops, value_size, port):
    srv = os.path.join(vdir, "valkey-server")
    cli = os.path.join(vdir, "valkey-cli")
    bench = os.path.join(vdir, "valkey-benchmark")
    work = tempfile.mkdtemp(prefix="smash-valkey-", dir=os.environ.get("SMASH_TMP"))

    def c(*args, timeout=600):
        return subprocess.run([cli, "-p", str(port), *args], capture_output=True,
                              text=True, timeout=timeout).stdout.strip()

    log_path = os.path.join(work, "server.log")
    logf = open(log_path, "w")

    def log_tail():
        logf.flush()
        try:
            with open(log_path, errors="replace") as f:
                return f.read()[-600:]
        except OSError:
            return ""

    p = subprocess.Popen([srv, "--port", str(port), "--save", "", "--appendonly", "no",
                          "--hz", "1", "--dynamic-hz", "no", "--enable-debug-command", "yes",
                          "--dir", work, "--loglevel", "notice"],
                         env=env, stdout=logf, stderr=subprocess.STDOUT)
    try:
        if not wait_port(port):
            return {"ok": False, "error": "server did not start: " + log_tail()}
        t0 = time.time()
        subprocess.run([bench, "-p", str(port), "-c", "50", "-n", str(n_ops), "-r", str(n_ops),
                        "-d", str(value_size), "-t", "set", "-q"], capture_output=True, timeout=1200)
        fill_s = time.time() - t0
        d1 = c("DEBUG", "DIGEST")
        dbsize = c("DBSIZE")
        peak = rss_mb(p.pid)
        st = {"vma_fill": vma_count(p.pid)}
        lo = sample_cool(p.pid, cool, peak, st)
        d2 = c("DEBUG", "DIGEST")
        st["vma_digest"] = vma_count(p.pid)
        subprocess.run([bench, "-p", str(port), "-c", "50", "-n", str(n_ops), "-r", str(n_ops),
                        "-d", str(value_size), "-t", "get", "-q"], capture_output=True, timeout=1200)
        c("BGSAVE")
        t1 = time.time()
        time.sleep(0.5)
        while time.time() - t1 < 300 and p.poll() is None:
            if "rdb_bgsave_in_progress:0" in c("INFO", "persistence"):
                break
            time.sleep(0.2)
        bgsave_s = time.time() - t1
        st["vma_bgsave"] = vma_count(p.pid)
        bg = "rdb_last_bgsave_status:ok" in c("INFO", "persistence")
        c("DEBUG", "RELOAD", "NOSAVE")
        d3 = c("DEBUG", "DIGEST")
        logf.flush()
        with open(log_path, errors="replace") as f:
            reloaded = "DB reloaded by DEBUG RELOAD" in f.read()
        alive = p.poll() is None
        ok = alive and bool(d1) and d1 == d2 == d3 and bg and reloaded
        return {"ok": ok, "peak_mb": peak, "min_mb": lo, "fill_s": fill_s, "bgsave_s": bgsave_s, **st,
                "check": f"dbsize={dbsize} digest_fill={d1[:12]} cool={d2[:12]} "
                         f"reload={d3[:12]} reloaded={reloaded} bgsave_ok={bg} alive={alive}",
                "error": "" if ok else log_tail().replace("\n", " | ")}
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
        logf.close()
        keep = os.environ.get("BUNDLED_KEEP_FAILED_LOGS")
        if keep and not locals().get("ok", False):
            os.makedirs(keep, exist_ok=True)
            shutil.copy(log_path, os.path.join(keep, f"valkey-{port}-{int(time.time())}.log"))
        shutil.rmtree(work, ignore_errors=True)


def lib_flags(libdir, lib):
    """-l flags for a library, optionally from a non-default directory."""
    if not libdir:
        return ["-l" + lib]
    return ["-L" + libdir, "-Wl,-rpath," + libdir, "-l" + lib]


def build_workload(out_dir, kind, a):
    exe = os.path.join(out_dir, f"bundled_alloc_workload_{kind}")
    src = os.path.join(HERE, "bundled_alloc_workload.c")
    if os.path.exists(exe) and os.path.getmtime(exe) > os.path.getmtime(src):
        return exe
    if kind == "mi":
        flags = ["-DUSE_MIMALLOC"] + (["-I" + a.mimalloc_inc] if a.mimalloc_inc else []) \
            + lib_flags(a.mimalloc_lib, "mimalloc")
    elif kind == "tc":
        flags = ["-DUSE_TCMALLOC"] + lib_flags(a.tcmalloc_lib, a.tcmalloc_name)
    else:
        flags = []
    subprocess.check_call(["cc", "-O2", "-o", exe, src] + flags)
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--valkey-dir", default="")
    ap.add_argument("--apps", default="valkey,mi,tc,python")
    ap.add_argument("--configs", default="none,full:0,full:1,large:0,large:1",
                    help="mode:track[@lib] list (track 0, 1, or auto = unset); 'none' = no smash; "
                         "@lib selects an --alt-lib")
    ap.add_argument("--alt-lib", action="append", default=[],
                    help="NAME=path/to/libsmash.so, usable as mode:track@NAME (A/B builds)")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--cool", type=int, default=20)
    ap.add_argument("--mib", type=int, default=512, help="mi/tc live heap target")
    ap.add_argument("--py-entries", type=int, default=2_000_000)
    ap.add_argument("--valkey-ops", type=int, default=1_000_000)
    ap.add_argument("--valkey-value", type=int, default=256)
    ap.add_argument("--port", type=int, default=16499, help="valkey port")
    ap.add_argument("--json", default="")
    ap.add_argument("--cold-timeout", default="1",
                    help="SMASH_COLD_TIMEOUT_SEC; empty = smash's default (10 s)")
    ap.add_argument("--python", default=sys.executable, help="interpreter for the python app")
    ap.add_argument("--mimalloc-inc", default="", help="mimalloc include dir (if not system)")
    ap.add_argument("--mimalloc-lib", default="", help="mimalloc library dir (if not system)")
    ap.add_argument("--tcmalloc-lib", default="", help="tcmalloc library dir (if not system)")
    ap.add_argument("--tcmalloc-name", default="tcmalloc_minimal",
                    help="tcmalloc library name (tcmalloc_minimal or tcmalloc)")
    a = ap.parse_args()

    lib = os.path.abspath(os.path.join(a.build_dir, "libsmash.so"))
    apps = a.apps.split(",")
    configs = a.configs.split(",")
    jobs = [(app, cfg, r) for r in range(a.runs) for app in apps for cfg in configs]
    random.shuffle(jobs)
    results = []
    alt = dict(x.split("=", 1) for x in a.alt_lib)
    for app, cfg, r in jobs:
        base_cfg, _, libname = cfg.partition("@")
        mode, track = (base_cfg.split(":") + ["0"])[:2] if base_cfg != "none" else ("none", "0")
        env = smash_env(os.path.abspath(alt[libname]) if libname else lib, mode, track, a.cold_timeout)
        if app == "valkey":
            res = run_valkey(a.valkey_dir, env, a.cool, a.valkey_ops, a.valkey_value, a.port)
        elif app == "python":
            res = run_child_workload([a.python, "-I", os.path.join(HERE, "bundled_alloc_python.py"),
                                      str(a.py_entries), str(a.cool)], env, a.cool)
        else:
            res = run_child_workload([build_workload(a.build_dir, app, a), app, str(a.mib), str(a.cool)],
                                     env, a.cool)
        res.update(app=app, config=cfg, run=r)
        if res.get("peak_mb"):
            res["reduction_pct"] = 100.0 * (1 - res["min_mb"] / res["peak_mb"])
        results.append(res)
        print(f"{app:7s} {cfg:13s} run{r} ok={res['ok']} peak={res.get('peak_mb', 0):8.1f} "
              f"min={res.get('min_mb', 0):8.1f} red={res.get('reduction_pct', 0):5.1f}% "
              f"fill={res.get('fill_s', 0):.2f}s vmas={res.get('vma_max', 0)} "
              f"{res.get('check', '')} {res.get('error', '')}", flush=True)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    print("\nmedian over runs: app config ok peak_MB min_MB reduction% fill_s vma_max")
    for app in apps:
        for cfg in configs:
            rs = [x for x in results if x["app"] == app and x["config"] == cfg]
            if not rs:
                continue
            def med(k):
                v = sorted(x.get(k, 0.0) for x in rs)
                return v[len(v) // 2]
            print(f"  {app:7s} {cfg:13s} {sum(x['ok'] for x in rs)}/{len(rs)} "
                  f"{med('peak_mb'):8.1f} {med('min_mb'):8.1f} {med('reduction_pct'):6.1f} "
                  f"{med('fill_s'):7.2f} {med('vma_max'):7.0f}")
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
