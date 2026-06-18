#!/usr/bin/env python3
"""4-way Redis benchmark: glibc / jemalloc / mimalloc / smash.

Methodology (see bench/BENCHMARKING.md):
- 5 trials, randomized order per trial
- Fresh server instance per measurement
- redis-benchmark with 50 connections for throughput
- Reports median (not mean) to reject outliers
- Verifies data integrity pre/post cooling
"""
import subprocess, time, os, tempfile, socket, random, statistics

REDIS = "bench/deps/bin/redis-server-libc"
JE = "/usr/lib64/libjemalloc.so.2"
MI = os.path.expanduser(
    "~/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/"
    "smash/build/allocators/lib/libmimalloc.so")
SM = "./libsmash.so"

N_KEYS = 200000
VAL_SIZE = 1000
COOL_SEC = 15
N_TRIALS = 5
BENCH_N = 200000
BENCH_CLIENTS = 50

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


def verify_redis(port, n_check=100):
    """Spot-check data integrity."""
    try:
        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", port))
        for i in range(n_check):
            key = f"key{i:06d}"
            s.sendall(f"GET {key}\r\n".encode())
            resp = b""
            while b"\r\n" not in resp[6:]:
                resp += s.recv(4096)
            if not resp.startswith(b"$"):
                s.close()
                return False
        s.close()
        return True
    except Exception:
        return False


def redis_benchmark_get(port):
    """Run redis-benchmark GET and parse throughput."""
    r = subprocess.run(
        ["redis-benchmark", "-p", str(port), "-n", str(BENCH_N),
         "-c", str(BENCH_CLIENTS), "-t", "get", "-q"],
        capture_output=True, text=True, timeout=60)
    # Parse: "GET: 61425.06 requests per second, p50=0.391 msec"
    for line in r.stdout.strip().split("\n"):
        if "requests per second" in line:
            parts = line.split()
            rps = float(parts[1]) if len(parts) > 1 else 0
            p50 = None
            for p in parts:
                if p.startswith("p50="):
                    p50 = float(p.split("=")[1])
            return rps, p50
    return 0, None


def run_one(name, port, env_extra):
    """Run a single trial for one configuration. Returns dict of metrics."""
    env = dict(os.environ)
    env.update(env_extra)

    proc = subprocess.Popen(
        [REDIS, "--port", str(port), "--save", "", "--appendonly", "no",
         "--hz", "1", "--activedefrag", "no", "--loglevel", "warning"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)

    # Fill
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
        for i in range(N_KEYS):
            f.write(f"SET key{i:06d} {chr(65 + (i % 26)) * VAL_SIZE}\n")
        pipe_file = f.name
    subprocess.run(["redis-cli", "-p", str(port), "--pipe"],
                   stdin=open(pipe_file), capture_output=True, timeout=60)
    os.unlink(pipe_file)

    peak_kb = get_rss_kb(proc.pid)
    integrity_pre = verify_redis(port, 100)

    # Cool
    time.sleep(COOL_SEC)
    cool_kb = get_rss_kb(proc.pid)

    # Benchmark: cold pass
    cold_rps, cold_p50 = redis_benchmark_get(port)
    # Benchmark: warm pass
    warm_rps, warm_p50 = redis_benchmark_get(port)

    serve_kb = get_rss_kb(proc.pid)
    integrity_post = verify_redis(port, 100)

    proc.terminate()
    proc.wait()

    reduction = (1 - cool_kb / peak_kb) * 100 if peak_kb > 0 else 0
    return {
        "peak_mb": peak_kb // 1024,
        "cool_mb": cool_kb // 1024,
        "reduction": reduction,
        "cold_rps": cold_rps,
        "warm_rps": warm_rps,
        "cold_p50": cold_p50,
        "warm_p50": warm_p50,
        "serve_mb": serve_kb // 1024,
        "integrity": integrity_pre and integrity_post,
    }


def main():
    # Kill any leftover instances
    subprocess.run(["pkill", "-f", "redis.*198"], capture_output=True)
    time.sleep(1)

    # Check that libraries exist
    for name, env_extra in CONFIGS:
        if "LD_PRELOAD" in env_extra:
            path = env_extra["LD_PRELOAD"]
            if not os.path.exists(path):
                print(f"WARNING: {path} not found, skipping {name}")

    results = {name: [] for name, _ in CONFIGS}
    port = 19800

    # Warmup trial (discarded)
    print("Warmup trial (discarded)...", flush=True)
    r = run_one("warmup", port, {})
    port += 1
    time.sleep(1)

    # Main trials
    for trial in range(N_TRIALS):
        order = list(range(len(CONFIGS)))
        random.shuffle(order)
        print(f"Trial {trial + 1}/{N_TRIALS} "
              f"(order: {[CONFIGS[i][0] for i in order]})", flush=True)
        for idx in order:
            name, env_extra = CONFIGS[idx]
            r = run_one(name, port, env_extra)
            results[name].append(r)
            port += 1
            time.sleep(1)

    # Report
    print(f"\n{'=' * 80}")
    print(f"REDIS 4-WAY ({N_KEYS} keys × {VAL_SIZE}B, "
          f"{BENCH_CLIENTS} clients, {N_TRIALS} trials, median)")
    print(f"{'=' * 80}")
    print(f"{'Allocator':<10} {'Peak':>6} {'Cool':>6} {'Red%':>6} "
          f"{'Cold rps':>10} {'Warm rps':>10} {'p50ms':>6} {'OK':>4}")
    print("-" * 70)
    for name, _ in CONFIGS:
        trials = results[name]
        if not trials:
            continue
        med_cold = statistics.median(t["cold_rps"] for t in trials)
        med_warm = statistics.median(t["warm_rps"] for t in trials)
        med_red = statistics.median(t["reduction"] for t in trials)
        med_peak = statistics.median(t["peak_mb"] for t in trials)
        med_cool = statistics.median(t["cool_mb"] for t in trials)
        med_p50 = statistics.median(
            t["warm_p50"] for t in trials if t["warm_p50"] is not None)
        all_ok = all(t["integrity"] for t in trials)
        print(f"{name:<10} {med_peak:>5.0f}M {med_cool:>5.0f}M {med_red:>5.1f}% "
              f"{med_cold:>10.0f} {med_warm:>10.0f} {med_p50:>6.3f} "
              f"{'OK' if all_ok else 'FAIL':>4}")

    # Raw data
    print(f"\n{'=' * 80}")
    print("Raw trials (warm rps):")
    for name, _ in CONFIGS:
        warm_vals = [f"{t['warm_rps']:.0f}" for t in results[name]]
        print(f"  {name:<10}: {', '.join(warm_vals)}")


if __name__ == "__main__":
    main()
