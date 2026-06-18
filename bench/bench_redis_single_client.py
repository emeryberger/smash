#!/usr/bin/env python3
"""Verify: single-client sequential GET shows smash decompression cost,
while glibc/jemalloc/mimalloc all show identical throughput."""
import subprocess, time, os, socket, tempfile

REDIS = "bench/deps/bin/redis-server-libc"
JE = "/usr/lib64/libjemalloc.so.2"
MI = "/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash/build/allocators/lib/libmimalloc.so"
SM = "./libsmash.so"

def get_rss(pid):
    with open(f"/proc/{pid}/status") as f:
        for l in f:
            if l.startswith("VmRSS:"): return int(l.split()[1])
    return 0

def single_client_get(port, n=10000):
    s = socket.socket(); s.connect(("127.0.0.1", port))
    t0 = time.time()
    for i in range(n):
        s.sendall(f"GET key{i:06d}\r\n".encode())
        r = b""
        while b"\r\n" not in r[6:]: r += s.recv(4096)
    elapsed = time.time() - t0
    s.close()
    return n / elapsed

def test(name, port, env_extra=None):
    env = dict(os.environ)
    if env_extra: env.update(env_extra)
    proc = subprocess.Popen([REDIS, "--port", str(port), "--save", "", "--appendonly", "no",
        "--hz", "1", "--activedefrag", "no", "--loglevel", "warning"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
        for i in range(200000):
            f.write(f"SET key{i:06d} {chr(65+(i%26))*1000}\n")
        pf = f.name
    subprocess.run(["redis-cli", "-p", str(port), "--pipe"], stdin=open(pf), capture_output=True)
    os.unlink(pf)
    pk = get_rss(proc.pid)
    time.sleep(15)
    cl = get_rss(proc.pid)
    rps_cold = single_client_get(port, 10000)
    rps_warm = single_client_get(port, 10000)
    proc.terminate(); proc.wait()
    red = (1-cl/pk)*100
    print(f"{name:10s} Peak={pk//1024:>3}MB Cool={cl//1024:>3}MB Red={red:>5.1f}%  "
          f"Cold={rps_cold:>8.0f} rps  Warm={rps_warm:>8.0f} rps", flush=True)

subprocess.run(["pkill", "-f", "redis.*195"], capture_output=True)
time.sleep(1)
print("=== Single-client sequential GET (Python socket, 10K keys) ===", flush=True)
print(f"{'Allocator':<10} {'Peak':>7} {'Cool':>7} {'Red':>6}  {'Cold rps':>12}  {'Warm rps':>12}", flush=True)
test("glibc", 19500)
test("jemalloc", 19501, {"LD_PRELOAD": JE})
test("mimalloc", 19502, {"LD_PRELOAD": MI})
test("smash", 19503, {"LD_PRELOAD": SM, "SMASH_COLD_TIMEOUT_SEC": "5", "SMASH_NO_MONITOR": "1"})
