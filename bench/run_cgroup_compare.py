#!/usr/bin/env python3
"""glibc+zswap vs smash inside a memory-capped cgroup v2 (containers / k8s).

Reproduces the "smash vs zswap under a container memory limit" comparison on
any Linux host with cgroup v2, swap and (optionally) zswap. Each run gets a
fresh child cgroup with memory.high / memory.max set, the workload is started
inside it, and the cgroup's own counters are sampled from outside:

  memory.current         RAM charged to the container (anon + page cache +
                         kernel + the zswap pool) -- what the limit bills
  memory.swap.current    bytes swapped out of the container (disk or zswap)
  memory.stat            zswpin/zswpout, pgmajfault, workingset_refault_anon
  memory.events          high / max / oom_kill counts
  memory.pressure        PSI 'some'/'full' total stall time (us)

Configs: `glibc` (no preload; the kernel's zswap is the only compressor) plus
any number of `NAME=/path/to/libsmash.so[,ENV=VAL...]` preloads. Runs of all
(app, config) pairs are shuffled together (--runs each, plus --warmup
discarded runs) so drift and the first-run cold penalty spread evenly.

Apps:
  sqlite  bench_sqlite (in-memory DB; reports throughput + cold/hot p99)
  rss     bench_rss (large compressible allocations; memory only)
  redis   redis-server + redis-benchmark. Needs a redis-server that uses the
          libc malloc (MALLOC=libc build); distro packages link jemalloc
          statically, so LD_PRELOAD=libsmash.so has no effect on them.

Must run as root (cgroup creation); workloads run as $SUDO_UID/$SUDO_GID when
set (no CAP_SYS_ADMIN, as in a container). Example:

  sudo python3 bench/run_cgroup_compare.py --build-dir build \\
      --config old=/opt/smash-master/libsmash.so \\
      --config new=build/libsmash.so \\
      --apps sqlite,rss --runs 5 --out results/cgroup.json

Prints a markdown table of medians (min..max). Numbers are only as good as
the host: record kernel, swap device and zswap parameters (printed at start).
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

CG_ROOT = Path("/sys/fs/cgroup")
PARENT = "smash-cgcmp"

# Defaults per app: (memory.high, memory.max). Chosen so the cap sits well
# below glibc's uncapped peak (sqlite ~515 MiB, rss = --rss-mb) but above
# smash's uncapped steady state, i.e. the container is genuinely tight.
DEFAULT_CAPS = {
    "sqlite": ("300M", "450M"),
    "rss": ("160M", "240M"),
    "redis": ("300M", "450M"),
}


def read(p: Path) -> str:
    try:
        return p.read_text()
    except OSError:
        return ""


def read_int(p: Path) -> int:
    s = read(p).strip()
    if not s or s == "max":
        return -1
    try:
        return int(s)
    except ValueError:
        return -1


def keyed(text: str) -> dict[str, int]:
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("-").isdigit():
            out[parts[0]] = int(parts[1])
    return out


def psi_totals(text: str) -> dict[str, int]:
    out = {}
    for line in text.splitlines():
        m = re.match(r"(some|full) .*total=(\d+)", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def host_info() -> dict:
    info = {"kernel": os.uname().release, "machine": os.uname().machine}
    zp = Path("/sys/module/zswap/parameters")
    if zp.is_dir():
        info["zswap"] = {f.name: read(f).strip() for f in sorted(zp.iterdir())}
    info["swaps"] = read(Path("/proc/swaps")).strip().splitlines()[1:]
    try:
        info["nproc"] = os.cpu_count()
        info["memtotal_kb"] = keyed(read(Path("/proc/meminfo")).replace(":", "").replace(" kB", ""))\
            .get("MemTotal")
    except Exception:
        pass
    return info


class Cgroup:
    def __init__(self, name: str, high: str, mx: str):
        parent = CG_ROOT / PARENT
        parent.mkdir(exist_ok=True)
        ctl = read(parent / "cgroup.subtree_control")
        if "memory" not in ctl.split():
            (parent / "cgroup.subtree_control").write_text("+memory")
        self.path = parent / name
        if self.path.exists():
            self.path.rmdir()
        self.path.mkdir()
        (self.path / "memory.high").write_text(high)
        (self.path / "memory.max").write_text(mx)

    def stat(self) -> dict[str, int]:
        return keyed(read(self.path / "memory.stat"))

    def events(self) -> dict[str, int]:
        return keyed(read(self.path / "memory.events"))

    def psi(self) -> dict[str, int]:
        return psi_totals(read(self.path / "memory.pressure"))

    def remove(self):
        for _ in range(50):
            try:
                self.path.rmdir()
                return
            except OSError:
                time.sleep(0.1)


class Sampler(threading.Thread):
    def __init__(self, cg: Cgroup, period: float):
        super().__init__(daemon=True)
        self.cg, self.period = cg, period
        self.samples: list[tuple[float, int, int, int]] = []
        self.stop = threading.Event()

    def run(self):
        t0 = time.monotonic()
        while not self.stop.is_set():
            cur = read_int(self.cg.path / "memory.current")
            swp = read_int(self.cg.path / "memory.swap.current")
            zsw = read_int(self.cg.path / "memory.zswap.current")
            if cur >= 0:
                self.samples.append((time.monotonic() - t0, cur, max(swp, 0), max(zsw, 0)))
            self.stop.wait(self.period)


def demote():
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")

    def fn():
        os.setsid()
        if gid:
            os.setgid(int(gid))
        if uid:
            os.setuid(int(uid))
    return fn


def join_cgroup(cg: Cgroup):
    procs = cg.path / "cgroup.procs"

    def fn():
        procs.write_text(str(os.getpid()))
        demote()()
    return fn


def parse_metrics(text: str) -> dict[str, float]:
    out = {}
    for m in re.finditer(r"^METRIC (\S+) (\S+)$", text, re.M):
        try:
            out[m.group(1)] = float(m.group(2))
        except ValueError:
            pass
    return out


def app_cmd(app: str, args) -> list[str]:
    b = Path(args.build_dir) / "bench"
    if app == "sqlite":
        return [str(b / "bench_sqlite"), "--rows", str(args.sqlite_rows),
                "--cool", str(args.sqlite_cool), "--serve", str(args.sqlite_serve)]
    if app == "rss":
        return [str(b / "bench_rss"), "--total-mb", str(args.rss_mb), "--wait", str(args.rss_wait)]
    raise ValueError(app)


def free_port() -> int:
    import socket
    with socket.socket() as sk:
        sk.bind(("127.0.0.1", 0))
        return sk.getsockname()[1]


def run_redis(cg: Cgroup, env: dict, args) -> dict[str, float]:
    """Start redis-server in the cgroup, fill, idle, then GET-serve.

    The GET phase draws keys uniformly from the whole keyspace after the idle
    period, so its latency tail is dominated by cold (compressed / swapped)
    values. The server binds 127.0.0.1 on a fresh port and its PID is checked
    via INFO so the clients can never talk to some other process's server.
    """
    port = str(free_port())
    cli, bench = args.redis_cli, args.redis_benchmark
    m: dict[str, float] = {}
    srv = subprocess.Popen(
        [args.redis_server, "--bind", "127.0.0.1", "--protected-mode", "yes",
         "--port", port, "--save", "", "--appendonly", "no",
         "--hz", "1", "--activedefrag", "no", "--lazyfree-lazy-user-del", "no"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        preexec_fn=join_cgroup(cg))
    try:
        ok = False
        for _ in range(100):
            info = subprocess.run([cli, "-h", "127.0.0.1", "-p", port, "info", "server"],
                                  capture_output=True, text=True)
            if info.returncode == 0 and f"process_id:{srv.pid}" in info.stdout:
                ok = True
                break
            if srv.poll() is not None:
                break
            time.sleep(0.1)
        if not ok:
            m["exit_code"] = 1.0
            return m
        n = str(args.redis_keys)
        subprocess.run([bench, "-h", "127.0.0.1", "-p", port, "-t", "set", "-n", n, "-r", n,
                        "-d", str(args.redis_value), "-q", "-P", "16"], capture_output=True,
                       timeout=600, preexec_fn=demote())
        time.sleep(args.redis_idle)
        out = subprocess.run([bench, "-h", "127.0.0.1", "-p", port, "-t", "get", "-n", n,
                              "-r", n, "-d", str(args.redis_value), "-c", "8", "--csv"],
                             capture_output=True, text=True, timeout=600,
                             preexec_fn=demote()).stdout
        # csv: "test","rps","avg_latency_ms","min","p50","p95","p99","max"
        for line in out.splitlines():
            cols = [c.strip('"') for c in line.split(",")]
            if cols and cols[0] == "GET" and len(cols) >= 7:
                m["ops_per_sec"] = float(cols[1])
                m["get_p99_us"] = float(cols[6]) * 1000.0
    finally:
        if srv.poll() is None:
            srv.send_signal(signal.SIGTERM)
            try:
                srv.wait(timeout=60)
            except subprocess.TimeoutExpired:
                srv.kill()
                srv.wait()
        m.setdefault("exit_code", float(srv.returncode or 0))
    return m


def one_run(app: str, cfg_name: str, cfg: dict, idx: int, args) -> dict:
    high, mx = cfg.get("cap") or args.caps.get(app, DEFAULT_CAPS[app])
    cg = Cgroup(f"{app}-{cfg_name}-{idx}-{os.getpid()}", high, mx)
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)
    if cfg.get("lib"):
        env["LD_PRELOAD"] = str(Path(cfg["lib"]).resolve())
    env.update(cfg.get("env", {}))
    st0, ev0, psi0 = cg.stat(), cg.events(), cg.psi()
    vm0 = keyed(read(Path("/proc/vmstat")))
    sampler = Sampler(cg, args.sample_period)
    sampler.start()
    t0 = time.monotonic()
    rec = {"app": app, "config": cfg_name, "run": idx, "high": high, "max": mx}
    try:
        if app == "redis":
            rec["metrics"] = run_redis(cg, env, args)
            rec["exit_code"] = int(rec["metrics"].pop("exit_code", 0))
        else:
            p = subprocess.run(app_cmd(app, args), env=env, capture_output=True,
                               text=True, timeout=args.timeout, preexec_fn=join_cgroup(cg))
            rec["exit_code"] = p.returncode
            rec["metrics"] = parse_metrics(p.stdout)
            if p.returncode != 0:
                rec["stderr_tail"] = p.stderr[-2000:]
    finally:
        rec["wall_s"] = time.monotonic() - t0
        sampler.stop.set()
        sampler.join()
        st1, ev1, psi1 = cg.stat(), cg.events(), cg.psi()
        vm1 = keyed(read(Path("/proc/vmstat")))
        cg.remove()
    s = sampler.samples
    if s:
        cur = [x[1] for x in s]
        peak_i = max(range(len(cur)), key=lambda i: cur[i])
        tail = cur[int(len(cur) * 0.7):] or cur
        dur = s[-1][0] - s[0][0] or 1.0
        auc = sum((s[i + 1][0] - s[i][0]) * s[i][1] for i in range(len(s) - 1))
        mib = 1 << 20
        rec.update({
            "peak_current_mib": cur[peak_i] / mib,
            "min_after_peak_mib": min(cur[peak_i:]) / mib,
            "mean_current_mib": auc / dur / mib,
            "tail_median_current_mib": statistics.median(tail) / mib,
            "peak_swap_mib": max(x[2] for x in s) / mib,
            "peak_zswap_pool_mib": max(x[3] for x in s) / mib,
            "mean_current_plus_swap_mib":
                sum((s[i + 1][0] - s[i][0]) * (s[i][1] + s[i][2]) for i in range(len(s) - 1))
                / dur / mib,
        })
    for k in ("zswpin", "zswpout", "pgmajfault", "workingset_refault_anon",
              "workingset_refault_file", "pgscan", "pgsteal"):
        if k in st1:
            rec[k] = st1[k] - st0.get(k, 0)
    for k in ("high", "max", "oom", "oom_kill"):
        rec[f"events_{k}"] = ev1.get(k, 0) - ev0.get(k, 0)
    for k in ("some", "full"):
        if k in psi1:
            rec[f"psi_{k}_ms"] = (psi1[k] - psi0.get(k, 0)) / 1000.0
    # pswpin is only in the global /proc/vmstat (not per-memcg): this delta
    # includes any other activity on the host during the run.
    rec["host_pswpin"] = vm1.get("pswpin", 0) - vm0.get("pswpin", 0)
    return rec


SUMMARY_COLS = [
    ("mean_current_mib", "mean mem.current MiB"),
    ("tail_median_current_mib", "steady mem.current MiB"),
    ("peak_current_mib", "peak mem.current MiB"),
    ("peak_swap_mib", "peak swap MiB"),
    ("m:ops_per_sec", "ops/s"),
    ("m:cold_p99_us", "cold p99 us"),
    ("m:get_p99_us", "GET p99 us"),
    ("zswpin", "zswpin"),
    ("pgmajfault", "majflt"),
    ("psi_some_ms", "PSI some ms"),
]


def val(r: dict, key: str):
    if key.startswith("m:"):
        return r.get("metrics", {}).get(key[2:])
    return r.get(key)


def summarize(results: list[dict]) -> str:
    lines = []
    apps = sorted({r["app"] for r in results})
    for app in apps:
        rs = [r for r in results if r["app"] == app and not r.get("warmup")]
        cfgs = []
        for r in rs:
            if r["config"] not in cfgs:
                cfgs.append(r["config"])
        cols = [c for c in SUMMARY_COLS if any(val(r, c[0]) is not None for r in rs)]
        hi = rs[0]["high"] if rs else ""
        mx = rs[0]["max"] if rs else ""
        lines.append(f"\n### {app} (memory.high={hi}, memory.max={mx})\n")
        lines.append("| config | n | fails | " + " | ".join(c[1] for c in cols) + " |")
        lines.append("|---|---:|---:|" + "---:|" * len(cols))
        for c in sorted(cfgs):
            cr = [r for r in rs if r["config"] == c]
            ok = [r for r in cr if r.get("exit_code") == 0]
            cells = []
            for key, _ in cols:
                v = [val(r, key) for r in ok if val(r, key) is not None]
                if not v:
                    cells.append("-")
                else:
                    cells.append(f"{statistics.median(v):.1f} ({min(v):.0f}..{max(v):.0f})")
            label = c
            if cr and (cr[0]["high"], cr[0]["max"]) != (hi, mx):
                label += f" @{cr[0]['high']}:{cr[0]['max']}"
            lines.append(f"| {label} | {len(ok)} | {len(cr) - len(ok)} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def parse_config(s: str) -> tuple[str, dict]:
    name, _, rest = s.partition("=")
    parts = rest.split(",")
    cfg = {"lib": parts[0] or None, "env": {}}
    for kv in parts[1:]:
        k, _, v = kv.partition("=")
        if k == "@cap":   # per-config HIGH:MAX, overrides --cap for every app
            h, _, m = v.partition(":")
            cfg["cap"] = (h, m or "max")
        else:
            cfg["env"][k] = v
    return name, cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.environ.get("BUILD_DIR", "build"))
    ap.add_argument("--config", action="append", default=[],
                    help="NAME=/path/libsmash.so[,ENV=VAL...][,@cap=HIGH:MAX] (repeatable; an empty "
                         "path runs without a preload); 'glibc' is always added")
    ap.add_argument("--no-glibc", action="store_true")
    ap.add_argument("--apps", default="sqlite,rss")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1, help="discarded runs per (app, config)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cap", action="append", default=[],
                    help="APP=HIGH:MAX override, e.g. sqlite=300M:450M")
    ap.add_argument("--sample-period", type=float, default=0.2)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--sqlite-rows", type=int, default=500000)
    ap.add_argument("--sqlite-cool", type=int, default=30)
    ap.add_argument("--sqlite-serve", type=int, default=20)
    ap.add_argument("--rss-mb", type=int, default=256)
    ap.add_argument("--rss-wait", type=int, default=40)
    ap.add_argument("--redis-server", default=os.environ.get("REDIS_SERVER", "redis-server"))
    ap.add_argument("--redis-cli", default=None)
    ap.add_argument("--redis-benchmark", default=None)
    ap.add_argument("--redis-port", type=int, default=6399)
    ap.add_argument("--redis-keys", type=int, default=300000)
    ap.add_argument("--redis-value", type=int, default=1024)
    ap.add_argument("--redis-idle", type=int, default=30)
    ap.add_argument("--out", default=None, help="JSON output path")
    args = ap.parse_args()

    # sudo resets PATH (secure_path): also look in /usr/local/bin.
    search = os.environ.get("PATH", "") + ":/usr/local/bin:/usr/bin"
    args.redis_cli = args.redis_cli or shutil.which("redis-cli", path=search) or "redis-cli"
    args.redis_benchmark = (args.redis_benchmark or shutil.which("redis-benchmark", path=search)
                            or "redis-benchmark")
    if os.geteuid() != 0:
        sys.exit("must run as root (creates cgroups); workloads drop to $SUDO_UID")
    if not (CG_ROOT / "cgroup.controllers").exists():
        sys.exit("cgroup v2 not mounted at /sys/fs/cgroup")
    args.caps = {}
    for c in args.cap:
        app, _, hm = c.partition("=")
        h, _, m = hm.partition(":")
        args.caps[app] = (h, m or "max")

    configs: dict[str, dict] = {}
    if not args.no_glibc:
        configs["glibc"] = {"lib": None, "env": {}}
    for c in args.config:
        n, cfg = parse_config(c)
        configs[n] = cfg
    apps = [a for a in args.apps.split(",") if a]

    info = host_info()
    print("host:", json.dumps(info, indent=1), flush=True)
    if not info.get("swaps"):
        print("WARNING: no swap active; the cap will OOM-kill or throttle instead of swapping",
              file=sys.stderr)

    plan = []
    for app in apps:
        for name in configs:
            plan += [(app, name, -1 - i) for i in range(args.warmup)]
            plan += [(app, name, i) for i in range(args.runs)]
    rng = random.Random(args.seed)
    warm = [p for p in plan if p[2] < 0]
    meas = [p for p in plan if p[2] >= 0]
    rng.shuffle(warm)
    rng.shuffle(meas)
    results = []
    out_path = Path(args.out) if args.out else None
    for k, (app, name, idx) in enumerate(warm + meas):
        r = one_run(app, name, configs[name], idx, args)
        r["warmup"] = idx < 0
        r["order"] = k
        results.append(r)
        m = r.get("metrics", {})
        print(f"[{k + 1}/{len(plan)}] {app:6s} {name:10s} run={idx:2d} rc={r.get('exit_code')} "
              f"mean={r.get('mean_current_mib', 0):.0f}MiB peak={r.get('peak_current_mib', 0):.0f}MiB "
              f"swap={r.get('peak_swap_mib', 0):.0f}MiB ops={m.get('ops_per_sec', 0):.0f} "
              f"coldp99={m.get('cold_p99_us', m.get('get_p99_us', 0)):.0f}us "
              f"zswpin={r.get('zswpin', 0)}", flush=True)
        if out_path:
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(json.dumps({"host": info, "configs": configs,
                                            "args": {k: v for k, v in vars(args).items()},
                                            "results": results}, indent=1))
    print(summarize(results))


if __name__ == "__main__":
    main()
