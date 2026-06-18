#!/usr/bin/env python3
"""4-way memcached benchmark: glibc / jemalloc / mimalloc / smash.

Methodology (see bench/BENCHMARKING.md):
- 5 trials, randomized order per trial
- Fresh server instance per measurement
- memtier_benchmark (4 threads × 10 clients) for throughput
- Reports median across trials
- Socket fill for deterministic 200K × 1KB dataset
"""
import subprocess, time, os, socket, random, statistics

MC = "bench/deps/bin/memcached"
JE = "/usr/lib64/libjemalloc.so.2"
MI = os.path.expanduser(
    "~/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/"
    "smash/build/allocators/lib/libmimalloc.so")
SM = "./libsmash.so"

N_KEYS = 200000
VAL_SIZE = 1000
COOL_SEC = 15
N_TRIALS = 5

CONFIGS = [
    ("glibc", {}),
    ("jemalloc", {"LD_PRELOAD": JE}),
    ("mimalloc", {"LD_PRELOAD": MI}),
    ("smash", {"LD_PRELOAD": SM, "SMASH_COLD_TIMEOUT_SEC": "5",
               "SMASH_NO_MONITOR": "1"}),
]


def get_rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError):
        pass
    return 0


def fill_memcached(port):
    """Fill via raw socket (deterministic, fast)."""
    s = socket.socket()
    s.connect(("127.0.0.1", port))
    val = "x" * VAL_SIZE
    for i in range(N_KEYS):
        cmd = f"set key{i:06d} 0 0 {VAL_SIZE}\r\n{val}\r\n"
        s.sendall(cmd.encode())
        s.recv(64)
    s.close()


def memtier_get(port):
    """Run memtier_benchmark GET-only and parse throughput + latency."""
    r = subprocess.run(
        ["memtier_benchmark", "--server=127.0.0.1", f"--port={port}",
         "--protocol=memcache_text", "--threads=4", "--clients=10",
         "--requests=50000", "--data-size=1000", "--key-maximum=200000",
         "--ratio=0:1", "--hide-histogram", "-x", "1"],
        capture_output=True, text=True, timeout=60)
    rps, p50, p99 = 0.0, None, None
    for line in r.stdout.split("\n"):
        if "Totals" in line:
            parts = line.split()
            if len(parts) >= 7:
                try:
                    rps = float(parts[1])
                    p50 = float(parts[4])
                    p99 = float(parts[6])
                except (ValueError, IndexError):
                    pass
    return rps, p50, p99


def run_one(port, env_extra):
    """Single trial for one configuration."""
    env = dict(os.environ)
    env.update(env_extra)

    proc = subprocess.Popen(
        [MC, "-p", str(port), "-m", "512", "-t", "4"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    fill_memcached(port)
    peak_kb = get_rss_kb(proc.pid)

    time.sleep(COOL_SEC)
    cool_kb = get_rss_kb(proc.pid)

    cold_rps, cold_p50, cold_p99 = memtier_get(port)
    warm_rps, warm_p50, warm_p99 = memtier_get(port)

    proc.terminate()
    proc.wait()

    reduction = (1 - cool_kb / peak_kb) * 100 if peak_kb > 0 else 0
    return {
        "peak_mb": peak_kb // 1024,
        "cool_mb": cool_kb // 1024,
        "reduction": reduction,
        "cold_rps": cold_rps,
        "warm_rps": warm_rps,
        "warm_p50": warm_p50,
        "warm_p99": warm_p99,
    }


def main():
    subprocess.run(["pkill", "-f", "memcached.*199"], capture_output=True)
    time.sleep(1)

    results = {name: [] for name, _ in CONFIGS}
    port = 19900

    # Warmup trial (discarded)
    print("Warmup trial (discarded)...", flush=True)
    run_one(port, {})
    port += 1
    time.sleep(1)

    for trial in range(N_TRIALS):
        order = list(range(len(CONFIGS)))
        random.shuffle(order)
        print(f"Trial {trial + 1}/{N_TRIALS} "
              f"(order: {[CONFIGS[i][0] for i in order]})", flush=True)
        for idx in order:
            name, env_extra = CONFIGS[idx]
            r = run_one(port, env_extra)
            results[name].append(r)
            port += 1
            time.sleep(1)

    # Report
    print(f"\n{'=' * 80}")
    print(f"MEMCACHED 4-WAY ({N_KEYS} keys × {VAL_SIZE}B, "
          f"memtier 4t×10c, {N_TRIALS} trials, median)")
    print(f"{'=' * 80}")
    print(f"{'Allocator':<10} {'Peak':>6} {'Cool':>6} {'Red%':>6} "
          f"{'Cold rps':>10} {'Warm rps':>10} {'p50ms':>6} {'p99ms':>6}")
    print("-" * 72)
    for name, _ in CONFIGS:
        trials = results[name]
        if not trials:
            continue
        med_cold = statistics.median(t["cold_rps"] for t in trials)
        med_warm = statistics.median(t["warm_rps"] for t in trials)
        med_red = statistics.median(t["reduction"] for t in trials)
        med_peak = statistics.median(t["peak_mb"] for t in trials)
        med_cool = statistics.median(t["cool_mb"] for t in trials)
        p50s = [t["warm_p50"] for t in trials if t["warm_p50"] is not None]
        p99s = [t["warm_p99"] for t in trials if t["warm_p99"] is not None]
        med_p50 = statistics.median(p50s) if p50s else 0
        med_p99 = statistics.median(p99s) if p99s else 0
        print(f"{name:<10} {med_peak:>5.0f}M {med_cool:>5.0f}M {med_red:>5.1f}% "
              f"{med_cold:>10.0f} {med_warm:>10.0f} {med_p50:>6.3f} {med_p99:>6.3f}")

    # Raw data
    print(f"\n{'=' * 80}")
    print("Raw trials (warm rps):")
    for name, _ in CONFIGS:
        warm_vals = [f"{t['warm_rps']:.0f}" for t in results[name]]
        print(f"  {name:<10}: {', '.join(warm_vals)}")


if __name__ == "__main__":
    main()
