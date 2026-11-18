#!/usr/bin/env python3
"""Uncapped redis footprint decomposition: glibc vs smash configs.

  python3 bench/redis_footprint.py OUT.json RUNS NAME=lib[,ENV=V..] ...

Per run: start redis-server (a MALLOC=libc build: $REDIS_SERVER) with the
config's LD_PRELOAD (empty lib = none), fill KEYS SETs of VALUE bytes
(redis-benchmark -P 16), idle IDLE s, then GET-serve (8 clients); sample the
server's RSS every 0.2 s and snapshot RSS, INFO memory (used_memory counts
malloc_usable_size, so it includes size-class rounding), the largest smaps
mappings and smash's SMASH_METRICS_FILE at each phase boundary. One warmup
run per config, then RUNS shuffled runs (SEED). See agents/containers.md.
"""
import json, os, random, re, signal, socket, subprocess, sys, threading, time

SRV = os.environ.get("REDIS_SERVER", "redis-server")
CLI = os.environ.get("REDIS_CLI", "redis-cli")
BENCH = os.environ.get("REDIS_BENCHMARK", "redis-benchmark")


def rss_kb(pid):
    try:
        t = open(f"/proc/{pid}/status").read()
    except OSError:
        return -1, -1
    r = int(re.search(r"VmRSS:\s+(\d+)", t).group(1))
    h = int(re.search(r"VmHWM:\s+(\d+)", t).group(1))
    return r, h


def smaps_top(pid, n=12):
    out, cur = [], None
    try:
        for line in open(f"/proc/{pid}/smaps"):
            if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
                a, b = line.split()[0].split("-")
                cur = {"range": line.split()[0], "size_kb": (int(b, 16) - int(a, 16)) >> 10,
                       "name": line.split()[5] if len(line.split()) > 5 else ""}
                out.append(cur)
            elif line.startswith("Rss:") and cur is not None:
                cur["rss_kb"] = int(line.split()[1])
    except OSError:
        return []
    out.sort(key=lambda m: -m.get("rss_kb", 0))
    return out[:n]


def info_mem(port):
    out = subprocess.run([CLI, "-p", port, "info", "memory"], capture_output=True, text=True).stdout
    d = {}
    for k in ("used_memory", "used_memory_rss", "allocator_allocated"):
        m = re.search(rf"^{k}:(\d+)", out, re.M)
        if m:
            d[k] = int(m.group(1))
    return d


def metrics(path):
    d = {}
    try:
        for line in open(path):
            if line.startswith("smash_") and " " in line and not line.startswith("#"):
                k, v = line.split()[:2]
                if "{" not in k:
                    d[k] = float(v)
    except OSError:
        pass
    return d


def run(cfg, env_extra, args, idx):
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = str(s.getsockname()[1]); s.close()
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)
    mpath = f"{args['tmp']}/m-{cfg}-{idx}.prom"
    if env_extra.get("lib"):
        env["LD_PRELOAD"] = os.path.abspath(env_extra["lib"])
        env["SMASH_METRICS_FILE"] = mpath
        env["SMASH_METRICS_INTERVAL_SEC"] = "1"
    env.update(env_extra.get("env", {}))
    srv = subprocess.Popen([SRV, "--bind", "127.0.0.1", "--port", port, "--save", "", "--appendonly", "no",
                            "--hz", "1", "--activedefrag", "no", "--lazyfree-lazy-user-del", "no"],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if subprocess.run([CLI, "-p", port, "ping"], capture_output=True).returncode == 0:
            break
        time.sleep(0.1)
    samples = []
    stop = threading.Event()

    def sampler():
        t0 = time.monotonic()
        while not stop.is_set():
            r, _ = rss_kb(srv.pid)
            samples.append((time.monotonic() - t0, r))
            stop.wait(0.2)
    th = threading.Thread(target=sampler, daemon=True); th.start()
    rec = {"config": cfg, "run": idx, "phases": {}}

    def snap(name):
        r, h = rss_kb(srv.pid)
        time.sleep(1.2)  # let the metrics file refresh
        rec["phases"][name] = {"rss_kb": r, "hwm_kb": h, "info": info_mem(port), "smash": metrics(mpath),
                               "smaps": smaps_top(srv.pid)}
    n = str(args["keys"])
    snap("start")
    subprocess.run([BENCH, "-p", port, "-t", "set", "-n", n, "-r", n, "-d", str(args["value"]), "-q", "-P", "16"],
                   capture_output=True)
    snap("filled")
    time.sleep(args["idle"])
    snap("idle")
    t = time.monotonic()
    out = subprocess.run([BENCH, "-p", port, "-t", "get", "-n", n, "-r", n, "-d", str(args["value"]), "-c", "8",
                          "--csv"], capture_output=True, text=True).stdout
    rec["get_s"] = time.monotonic() - t
    for line in out.splitlines():
        c = [x.strip('"') for x in line.split(",")]
        if c and c[0] == "GET":
            rec["get_ops"] = float(c[1]); rec["get_p99_ms"] = float(c[6])
    snap("served")
    stop.set(); th.join()
    srv.send_signal(signal.SIGTERM); srv.wait(timeout=60)
    rec["peak_rss_kb"] = max(x[1] for x in samples)
    rec["mean_rss_kb"] = sum(x[1] for x in samples) / len(samples)
    return rec


def main():
    out = sys.argv[1]
    runs = int(sys.argv[2])
    cfgs = {}
    for c in sys.argv[3:]:
        name, _, rest = c.partition("=")
        parts = rest.split(",")
        cfgs[name] = {"lib": parts[0] or None, "env": dict(p.split("=", 1) for p in parts[1:])}
    args = {"keys": int(os.environ.get("KEYS", 300000)), "value": int(os.environ.get("VALUE", 1024)),
            "idle": int(os.environ.get("IDLE", 30)), "tmp": os.path.dirname(os.path.abspath(out))}
    plan = [(c, -1) for c in cfgs] + [(c, i) for c in cfgs for i in range(runs)]
    warm, meas = plan[:len(cfgs)], plan[len(cfgs):]
    random.Random(int(os.environ.get("SEED", 7))).shuffle(meas)
    res = []
    for c, i in warm + meas:
        r = run(c, cfgs[c], args, i)
        res.append(r)
        p = r["phases"]
        print(f"{c:8s} run={i:2d} peak={r['peak_rss_kb']/1024:.0f}MiB filled={p['filled']['rss_kb']/1024:.0f} "
              f"idle={p['idle']['rss_kb']/1024:.0f} served={p['served']['rss_kb']/1024:.0f} "
              f"used_mem={p['filled']['info'].get('used_memory',0)/2**20:.0f} get={r.get('get_ops',0):.0f} "
              f"store={p['idle']['smash'].get('smash_store_bytes',0)/2**20:.0f} "
              f"cpu={p['served']['smash'].get('smash_compressor_cpu_seconds_total',0):.1f}", flush=True)
        json.dump(res, open(out, "w"), indent=1)


main()
