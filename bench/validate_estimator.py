#!/usr/bin/env python3
"""Compare smash's savings estimators against real smash runs (Linux).

Each workload has a load phase, a cool phase and a hot-only serve phase. At
the same point of the serve phase (serve start + --at seconds) it measures:

  glibc     RSS without smash, plus `smash-estimate --window 21 --points 7`
            started 21 s earlier (--window / --points; projected saving, %
            of that RSS)
  observe   LD_PRELOAD libsmash SMASH_MODE=observe: RSS and the projected
            saving from SMASH_OBSERVE_REPORT
  smash     LD_PRELOAD libsmash (full mode, or --large-only): RSS
  smash_nb  the same with SMASH_RECOMPRESS_BACKOFF=0 (--no-decompose skips
            it): how much of the run's RSS is refault backoff

With --baseline-build-dir (a build of the commit to compare against) it also
runs that build's smash-estimate (with --baseline-est-args, default the
pre-segment defaults `--window 10 --points 1`) and observe mode, for a
before/after table. Configs are interleaved in random order for --runs rounds.

Workloads (--workloads, comma separated):
  sqlite     bench/bench_sqlite --rows 500000 --cool 5 --serve 60
  kvstore    bench/bench_kv_store --entries 1000000 --cool 5 --serve 60
  memcached  memcached (PATH or --memcached), --mc-keys keys of --mc-value
             compressible bytes loaded over the text protocol, --cool s idle,
             then GETs of the first 5% of the keys (memtier_benchmark if
             installed, else a Python client)

For runs under libsmash the validator reads the smash_rss_part_bytes metric
(SMASH_METRICS_FILE, 1 s interval) and prints, per workload, the estimators'
implied RSS next to the measured smash RSS, component by component:
other (binary, libraries, stacks), metadata, data pages + compressed store
(and how much of the measured RSS is refault backoff). The component
differences add up to the projection error. --summarize re-prints the
summaries of a saved --json-out file.

  python3 bench/validate_estimator.py --build-dir build-bench --runs 5 \\
      --baseline-build-dir build-base --workloads sqlite,memcached,kvstore
  python3 bench/validate_estimator.py --methods idle,referenced   # one glibc config per method

Needs -DSMASH_BUILD_BENCH=ON. smash-estimate attaches to a non-child pid, so
reading page contents needs ptrace access: the script runs it with `sudo -n`
when that works (use --no-sudo to skip).
"""
import argparse
import json
import os
import random
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time

MiB = float(1 << 20)


def rss_bytes(pid):
    with open("/proc/%d/statm" % pid) as f:
        return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")


def anon_rw_rss(pid):
    """Rss of anonymous private writable mappings that are not stacks (what
    smash-estimate classifies as heap / malloc-arena / anon-mmap)."""
    tot, cur = 0, None
    with open("/proc/%d/smaps" % pid) as f:
        for line in f:
            m = re.match(r"^[0-9a-f]+-[0-9a-f]+ (\S+) \S+ \S+ (\d+)\s*(.*)$", line)
            if m:
                perms, ino, name = m.group(1), int(m.group(2)), m.group(3).strip()
                cur = perms.startswith("rw") and perms[3] == "p" and ino == 0 and \
                    (name == "" or name == "[heap]")
                continue
            if cur and line.startswith("Rss:"):
                tot += int(line.split()[1]) * 1024
    return tot


def read_metrics(path):
    out = {}
    try:
        with open(path) as f:
            for line in f:
                m = re.match(r'smash_rss_part_bytes\{part="(\w+)"\} (\d+)', line)
                if m:
                    out[m.group(1)] = int(m.group(2))
                    continue
                m = re.match(r"(smash_\w+) ([0-9.]+)$", line)
                if m:
                    out[m.group(1)] = float(m.group(2))
    except OSError:
        pass
    return out


def drain(stream):
    def run():
        for _ in stream:
            pass
    th = threading.Thread(target=run, daemon=True)
    th.start()
    return th


# ── workloads ────────────────────────────────────────────────────────────────

class InProcess:
    """A bench binary that prints "Serve phase" on stderr when it starts serving."""

    def __init__(self, a, bd):
        self.a = a
        self.bench = os.path.join(bd, "bench", self.binary)
        if not os.path.exists(self.bench):
            sys.exit("missing %s (configure with -DSMASH_BUILD_BENCH=ON)" % self.bench)

    def check(self):
        if self.proc.poll() is not None:
            raise RuntimeError("%s exited early" % self.binary)

    def start(self, env):
        self.proc = subprocess.Popen([self.bench] + self.args(), env=env, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, universal_newlines=True)
        t0 = time.time()
        while time.time() - t0 < 600:
            line = self.proc.stderr.readline()
            if not line:
                raise RuntimeError("%s exited before its serve phase" % self.binary)
            if "Serve phase" in line:
                drain(self.proc.stderr)
                return self.proc.pid, time.time()
        raise RuntimeError("timeout waiting for the serve phase")

    def stop(self):
        self.proc.kill()
        self.proc.wait()


class Sqlite(InProcess):
    name, binary = "sqlite", "bench_sqlite"

    def args(self):
        return ["--rows", str(self.a.rows), "--cool", str(self.a.cool), "--serve", str(self.a.serve)]


class KvStore(InProcess):
    """std::unordered_map of small compressible values (~200 B), hot 5% served."""
    name, binary = "kvstore", "bench_kv_store"

    def args(self):
        return ["--entries", str(self.a.kv_entries), "--cool", str(self.a.cool), "--serve", str(self.a.serve)]


class Memcached:
    name = "memcached"
    WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november "
             "oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu red green "
             "blue yellow orange purple black white silver gold north south east west spring summer "
             "autumn winter river mountain forest desert ocean island city village").split()

    def __init__(self, a, bd):
        self.a = a
        self.bin = a.memcached or shutil.which("memcached")
        if not self.bin:
            sys.exit("memcached not found (PATH or --memcached)")
        self.memtier = shutil.which("memtier_benchmark")
        rnd = random.Random(7)
        self.bios = []
        for _ in range(512):
            words = []
            while sum(len(w) + 1 for w in words) < a.mc_value:
                words.append(rnd.choice(self.WORDS))
            self.bios.append(" ".join(words))

    def value(self, i):
        head = '{"id":%d,"user":"user%07d","email":"user%07d@example.com","score":%d,"bio":"' % (
            i, i, i, (i * 2654435761) % 100000)
        body = self.bios[(i * 40503) % len(self.bios)]
        v = head + body
        return (v[:self.a.mc_value - 2] + '"}')

    def _wait_port(self, timeout=10):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                socket.create_connection(("127.0.0.1", self.a.mc_port), timeout=1).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("memcached did not open its port")

    def _populate(self):
        s = socket.create_connection(("127.0.0.1", self.a.mc_port))
        batch = []
        for i in range(1, self.a.mc_keys + 1):  # memtier keys start at 1
            v = self.value(i)
            batch.append("set key:%d 0 0 %d noreply\r\n%s\r\n" % (i, len(v), v))
            if len(batch) >= 500:
                s.sendall("".join(batch).encode())
                batch = []
        batch.append("version\r\n")
        s.sendall("".join(batch).encode())
        buf = b""
        while b"VERSION" not in buf:
            d = s.recv(4096)
            if not d:
                raise RuntimeError("memcached closed the connection during load")
            buf += d
        s.close()

    def _serve_python(self, hot, stop):
        s = socket.create_connection(("127.0.0.1", self.a.mc_port))
        f = s.makefile("rb")
        rnd = random.Random(3)
        while not stop.is_set():
            s.sendall(("get key:%d\r\n" % rnd.randint(1, hot)).encode())
            while True:
                line = f.readline()
                if not line or line.startswith(b"END"):
                    break
            time.sleep(0.0002)
        s.close()

    def start(self, env):
        a = self.a
        self.proc = subprocess.Popen(
            [self.bin, "-p", str(a.mc_port), "-l", "127.0.0.1", "-U", "0", "-m", "4096", "-t", "4"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._wait_port()
        self._populate()
        time.sleep(a.cool)
        hot = max(1, a.mc_keys // 20)
        self.client = None
        self.stop_ev = threading.Event()
        if self.memtier:
            self.client = subprocess.Popen(
                [self.memtier, "-s", "127.0.0.1", "-p", str(a.mc_port), "--protocol=memcache_text",
                 "-t", "2", "-c", "2", "--ratio=0:1", "--key-prefix=key:", "--key-minimum=1",
                 "--key-maximum=%d" % hot, "--key-pattern=R:R", "--rate-limiting=2000",
                 "--test-time=%d" % a.serve, "--hide-histogram"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            threading.Thread(target=self._serve_python, args=(hot, self.stop_ev), daemon=True).start()
        return self.proc.pid, time.time()

    def check(self):
        if self.client and self.client.poll() is not None:
            raise RuntimeError("memtier_benchmark exited early (rc=%s)" % self.client.returncode)

    def stop(self):
        self.stop_ev.set()
        if self.client:
            self.client.kill()
            self.client.wait()
        self.proc.kill()
        self.proc.wait()


WORKLOADS = {"sqlite": Sqlite, "memcached": Memcached, "kvstore": KvStore}


# ── one run ──────────────────────────────────────────────────────────────────

def run_one(wl, cfg, a, builds, workdir):
    bd = builds["base"] if cfg.endswith("_base") else builds["new"]
    lib = os.path.join(bd, "libsmash.so")
    est = os.path.join(bd, "smash-estimate")
    env = dict(os.environ)
    report = os.path.join(workdir, "observe.json")
    metrics = os.path.join(workdir, "metrics.prom")
    for p in (report, metrics):
        if os.path.exists(p):
            os.unlink(p)
    kind = cfg.replace("_base", "")
    method = kind.split(":", 1)[1] if ":" in kind else None  # glibc:<method> (--methods)
    kind = kind.split(":", 1)[0]
    if kind in ("observe", "smash", "smash_nb"):
        env["LD_PRELOAD"] = lib
        env["SMASH_METRICS_FILE"] = metrics
        env["SMASH_METRICS_INTERVAL_SEC"] = "1"
        env["SMASH_METRICS_RSS_PARTS"] = "1"
    if kind == "observe":
        env["SMASH_MODE"] = "observe"
        env["SMASH_OBSERVE_REPORT"] = report
        env["SMASH_OBSERVE_INTERVAL"] = "1"
    if kind == "smash_nb":
        env["SMASH_RECOMPRESS_BACKOFF"] = "0"
    if a.large_only and kind != "glibc":
        env["SMASH_LARGE_ONLY"] = "1"
    pid, t_serve = wl.start(env)
    out = {"config": cfg}
    try:
        est_proc = None
        if kind == "glibc":
            if cfg.endswith("_base"):
                eargs = a.baseline_est_args.split()
            else:
                eargs = ["--window", str(a.window), "--points", str(a.points)]
            if method:
                eargs = ["--method", method] + eargs
            win = float(eargs[eargs.index("--window") + 1]) if "--window" in eargs else 10.0
            time.sleep(max(0.0, a.at - win - (time.time() - t_serve)))
            cmd = [est, "--json"] + eargs + [str(pid)]
            if a.sudo:
                cmd = ["sudo", "-n"] + cmd
            est_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        universal_newlines=True)
        time.sleep(max(0.0, a.at - (time.time() - t_serve)))
        wl.check()
        out["rss"] = rss_bytes(pid)
        if kind == "glibc":
            out["anon"] = anon_rw_rss(pid)
        else:
            time.sleep(1.2)  # one metrics interval: the parts below are from after the RSS sample
            out["parts"] = read_metrics(metrics)
        if kind == "observe":
            with open(report) as f:
                js = json.load(f)
            out["obs"] = js
            out["proj_saving"] = js["projected_saving_bytes"]
            out["obs_rss"] = js["rss_bytes"]
        if est_proc:
            so, se = est_proc.communicate(timeout=a.window + 180)
            js = json.loads(so)
            out["est"] = js
            out["est_saving"] = js["projected_saving_bytes"]
            out["est_method"] = js.get("method")
            out["est_rss"] = js["rss_bytes"]
    finally:
        wl.stop()
    return out


# ── summary ──────────────────────────────────────────────────────────────────

def mean(xs):
    xs = [x for x in xs if x is not None]
    return statistics.mean(xs) if xs else float("nan")


def sd(xs):
    xs = [x for x in xs if x is not None]
    return statistics.stdev(xs) if len(xs) > 1 else 0.0


def part(o, k):
    return o.get("parts", {}).get(k, 0)


def data_store(o):
    p = o.get("parts", {})
    return sum(p.get(k, 0) for k in ("data_live", "data_shadow", "data_pending_madvise",
                                      "data_compressed_resident", "data_empty", "store_header",
                                      "store_data"))


def summarize(name, res, a):
    S = res["smash"]
    s_rss = mean([o["rss"] for o in S])
    # glibc configs: "glibc", or one "glibc:<method>" per --methods entry; the
    # first one drives the before/after row and the component table.
    gcfgs = [c for c in res if c.startswith("glibc") and not c.endswith("_base")]
    g0 = gcfgs[0]
    g_rss = mean([o["rss"] for c in gcfgs for o in res[c]])
    o_rss = mean([o["rss"] for o in res["observe"]])
    summ = {"workload": name, "runs": len(S),
            "rss_mib": {c: {"mean": mean([o["rss"] for o in v]) / MiB, "sd": sd([o["rss"] for o in v]) / MiB}
                        for c, v in res.items() if v}}
    act_g = 100.0 * (1 - s_rss / g_rss)
    act_o = 100.0 * (1 - s_rss / o_rss)
    summ["measured_reduction_vs_glibc_pct"] = act_g
    summ["measured_reduction_vs_observe_pct"] = act_o

    def est_pct(runs, key="est_saving"):
        return mean([100.0 * o[key] / o["est_rss"] for o in runs if key in o])

    def obs_pct(runs, key="proj_saving"):
        return mean([100.0 * o[key] / o["obs_rss"] for o in runs if key in o])

    rows = []
    for c in gcfgs:
        label = "smash-estimate" if c == "glibc" else "smash-estimate " + c.split(":", 1)[1]
        base = res.get(c + "_base", res.get("glibc_base", []))
        rows.append((label, est_pct(base), est_pct(res[c]), act_g))
    rows.append(("observe", obs_pct(res.get("observe_base", [])), obs_pct(res["observe"]), act_o))
    summ["estimators"] = {r[0]: {"before_pct": r[1], "after_pct": r[2], "measured_pct": r[3],
                                 "before_error_pp": r[1] - r[3], "after_error_pp": r[2] - r[3]} for r in rows}
    summ["est_method"] = {c: res[c][0].get("est_method") for c in gcfgs if res[c]}
    summ["smash_estimate_pct_by_config"] = {c: est_pct(res[c]) for c in gcfgs}
    summ["gap_by_config_pp"] = {c: v - act_g for c, v in summ["smash_estimate_pct_by_config"].items()}
    # No-backoff variants (what the new estimators print as the upper bound).
    est_nb = mean([100.0 * o["est"]["projected_saving_no_backoff_bytes"] / o["est_rss"]
                   for o in res[g0] if "projected_saving_no_backoff_bytes" in o.get("est", {})])
    obs_nb = mean([100.0 * o["obs"]["projected_saving_no_backoff_bytes"] / o["obs_rss"]
                   for o in res["observe"] if "projected_saving_no_backoff_bytes" in o.get("obs", {})])
    summ["no_backoff_pct"] = {"smash-estimate": est_nb, "observe": obs_nb}

    # Component table: implied smash RSS (predicted) vs measured smash RSS.
    s_other = mean([part(o, "other") for o in S])
    s_meta = mean([part(o, "metadata") for o in S])
    s_data = mean([data_store(o) for o in S])
    nb = res.get("smash_nb", [])
    # Backoff = RSS of the real run minus the SMASH_RECOMPRESS_BACKOFF=0 run
    # (RSS, not the parts: those are read ~1 s later, and a churning workload
    # moves between the two reads).
    nb_backoff = s_rss - mean([o["rss"] for o in nb]) if nb else float("nan")
    g = res[g0]
    e_other = mean([o["rss"] - o["anon"] + o["est"].get("smash_runtime_bytes", 0) for o in g])
    e_meta = mean([o["est"].get("smash_metadata_bytes", 0) for o in g])
    e_data = mean([o["anon"] - o["est"].get("compression_saving_bytes", o["est_saving"]) +
                   o["est"].get("layout_bytes", 0) for o in g])
    ob = res["observe"]
    o_other = mean([part(o, "other") for o in ob])
    o_meta = mean([part(o, "metadata") - o["obs"].get("observe_metadata_bytes", 0) +
                   o["obs"].get("run_metadata_bytes", 0) for o in ob])
    o_data = mean([data_store(o) - o["obs"]["would_compress_bytes"] + o["obs"]["blob_bytes"] +
                   o["obs"].get("backoff_held_bytes", 0) + o["obs"].get("read_held_bytes", 0) for o in ob])
    comp = {
        "other": {"measured": s_other, "smash-estimate": e_other, "observe": o_other},
        "metadata": {"measured": s_meta, "smash-estimate": e_meta, "observe": o_meta},
        "data+store": {"measured": s_data, "smash-estimate": e_data, "observe": o_data,
                       "of_which_backoff": nb_backoff},
    }
    comp["total"] = {k: sum(comp[c][k] for c in ("other", "metadata", "data+store"))
                     for k in ("measured", "smash-estimate", "observe")}
    summ["components_mib"] = {c: {k: v / MiB for k, v in d.items()} for c, d in comp.items()}
    # Store detail of the real run: blob bytes vs resident store pages.
    summ["store_mib"] = {
        "live_blob": mean([part(o, "smash_store_live_bytes") for o in S]) / MiB,
        "resident_data": mean([part(o, "store_data") for o in S]) / MiB,
        "resident_header": mean([part(o, "store_header") for o in S]) / MiB,
        "regions": mean([part(o, "smash_store_regions") for o in S]),
    }
    return summ


def print_summary(su):
    print("\n== %s%s: %d rounds, at serve+%gs (cloudnew numbers are dev numbers) ==" % (
        su["workload"], " (large-only; smash-estimate rows project full mode)" if su.get("large_only") else "",
        su["runs"], su["at_s"]))
    print("RSS MiB (mean ± sd): " + ", ".join(
        "%s %.1f±%.1f" % (c, v["mean"], v["sd"]) for c, v in su["rss_mib"].items()))
    print("measured reduction: %.1f%% vs glibc, %.1f%% vs observe RSS" % (
        su["measured_reduction_vs_glibc_pct"], su["measured_reduction_vs_observe_pct"]))
    print("%-16s %18s %18s %10s" % ("estimator", "before", "after", "measured"))
    for k, v in su["estimators"].items():
        fmt = lambda p, e: ("%.1f%% (%+.1f pp)" % (p, e)) if p == p else "n/a"
        print("%-16s %18s %18s %9.1f%%" % (k, fmt(v["before_pct"], v["before_error_pp"]),
                                           fmt(v["after_pct"], v["after_error_pp"]), v["measured_pct"]))
    nb = su["no_backoff_pct"]
    print("  (no-backoff upper bounds: smash-estimate %.1f%%, observe %.1f%%)" % (
        nb["smash-estimate"], nb["observe"]))
    print("smash RSS by component, MiB: measured vs implied by each estimator (difference = error)")
    print("%-12s %10s %16s %10s" % ("", "measured", "smash-estimate", "observe"))
    for c, d in su["components_mib"].items():
        line = "%-12s %10.1f %9.1f (%+5.1f) %5.1f (%+5.1f)" % (
            c, d["measured"], d["smash-estimate"], d["smash-estimate"] - d["measured"],
            d["observe"], d["observe"] - d["measured"])
        if "of_which_backoff" in d and d["of_which_backoff"] == d["of_which_backoff"]:
            line += "   [measured backoff: %.1f]" % d["of_which_backoff"]
        print(line)
    st = su["store_mib"]
    print("store: %.1f MiB live blobs in %.1f MiB resident data pages + %.2f MiB headers (%d regions)" % (
        st["live_blob"], st["resident_data"], st["resident_header"], st["regions"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build-bench")
    ap.add_argument("--baseline-build-dir", help="build of the commit to compare against (before/after)")
    ap.add_argument("--baseline-est-args", default="--window 10 --points 1",
                    help="smash-estimate arguments for the baseline build")
    ap.add_argument("--workloads", default="sqlite,memcached")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--rows", type=int, default=500000)
    ap.add_argument("--cool", type=int, default=5)
    ap.add_argument("--serve", type=int, default=60)
    # bench_sqlite's in-memory pager rehashes its page cache as the DB grows
    # (an access to every page); with the defaults one lands ~30 s into the
    # serve phase. Measure before it so all configs see the same steady state.
    ap.add_argument("--at", type=float, default=25.0, help="seconds into the serve phase to measure")
    ap.add_argument("--window", type=float, default=21.0,
                    help="smash-estimate window (21 s with 7 points has idle ages 12 s = cold timeout + 2 "
                         "and 21 s = 2x cold timeout + 1: models refault backoff)")
    ap.add_argument("--points", type=int, default=7)
    ap.add_argument("--kv-entries", type=int, default=1000000)
    ap.add_argument("--memcached")
    ap.add_argument("--mc-keys", type=int, default=400000)
    ap.add_argument("--mc-value", type=int, default=1000)
    ap.add_argument("--mc-port", type=int, default=11411)
    ap.add_argument("--large-only", action="store_true",
                    help="observe and smash runs with SMASH_LARGE_ONLY=1 (smash-estimate still projects "
                         "full mode, so its rows do not apply)")
    ap.add_argument("--no-decompose", dest="decompose", action="store_false")
    ap.add_argument("--no-sudo", dest="sudo", action="store_false")
    ap.add_argument("--methods", default="auto",
                    help="comma-separated smash-estimate --method values; each is its own glibc config")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json-out")
    ap.add_argument("--summarize", metavar="JSON",
                    help="print the summaries of a previous --json-out file instead of running")
    a = ap.parse_args()
    if a.summarize:
        with open(a.summarize) as f:
            saved = json.load(f)
        for su in saved["summaries"]:
            name = su["workload"]
            new = summarize(name, saved["raw"][name], a)
            new["at_s"], new["large_only"] = su.get("at_s", a.at), su.get("large_only", False)
            print_summary(new)
        return
    builds = {"new": os.path.abspath(a.build_dir)}
    if a.baseline_build_dir:
        builds["base"] = os.path.abspath(a.baseline_build_dir)
    for b in builds.values():
        for p in ("libsmash.so", "smash-estimate"):
            if not os.path.exists(os.path.join(b, p)):
                sys.exit("missing %s/%s" % (b, p))
    if a.sudo and subprocess.call(["sudo", "-n", "true"], stderr=subprocess.DEVNULL) != 0:
        a.sudo = False
    methods = [m for m in a.methods.split(",") if m]
    gcfgs = ["glibc"] if methods == ["auto"] else ["glibc:" + m for m in methods]
    cfgs = gcfgs + ["observe", "smash"] + (["smash_nb"] if a.decompose else []) + \
        ([c + "_base" for c in gcfgs] + ["observe_base"] if "base" in builds else [])
    wls = [WORKLOADS[w](a, builds["new"]) for w in a.workloads.split(",")]
    rnd = random.Random(a.seed)
    res = {w.name: {c: [] for c in cfgs} for w in wls}
    os.makedirs("tmp", exist_ok=True)
    with tempfile.TemporaryDirectory(dir="tmp") as wd:
        for r in range(a.runs):
            order = [(w, c) for w in wls for c in cfgs]
            rnd.shuffle(order)
            for w, c in order:
                o = run_one(w, c, a, builds, wd)
                brief = {k: v for k, v in o.items() if k not in ("obs", "est", "parts")}
                print("run %d %s %s %s" % (r, w.name, c, json.dumps(brief)), flush=True)
                res[w.name][c].append(o)
    out = []
    for w in wls:
        su = summarize(w.name, res[w.name], a)
        su["at_s"] = a.at
        su["large_only"] = a.large_only
        print_summary(su)
        out.append(su)
    if a.json_out:
        with open(a.json_out, "w") as f:
            json.dump({"summaries": out, "raw": res}, f, indent=1)


if __name__ == "__main__":
    main()
