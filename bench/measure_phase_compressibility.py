#!/usr/bin/env python3
"""Measure page compressibility during cooling and serve phases."""
import os, subprocess, time, signal, sys

try:
    import lz4.block, zstandard as zstd
except ImportError:
    sys.exit("pip install lz4 zstandard")

PAGE_SIZE = 4096

def scan_pid(pid, max_pages=100000):
    regions = []
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 5: continue
            if parts[1][0:2] != "rw": continue
            if int(parts[4]) != 0: continue
            if len(parts) > 5 and parts[5].startswith("[") and parts[5] != "[heap]": continue
            s, e = parts[0].split("-")
            regions.append((int(s, 16), int(e, 16)))
    cctx = zstd.ZstdCompressor(level=1)
    cctx9 = zstd.ZstdCompressor(level=9)
    fd = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    nz = 0
    total = lz4_t = z1_t = z9_t = 0
    for start, end in regions:
        for addr in range(start, end, PAGE_SIZE):
            if nz >= max_pages: break
            try:
                data = os.pread(fd, PAGE_SIZE, addr)
                if len(data) != PAGE_SIZE: continue
            except: continue
            if data == b"\x00" * PAGE_SIZE: continue
            nz += 1
            total += PAGE_SIZE
            lz4_t += len(lz4.block.compress(data, store_size=False))
            z1_t += len(cctx.compress(data))
            z9_t += len(cctx9.compress(data))
        if nz >= max_pages: break
    os.close(fd)
    lz4r = total / lz4_t if lz4_t else 0
    z1r = total / z1_t if z1_t else 0
    z9r = total / z9_t if z9_t else 0
    return nz, total / 1048576, lz4r, z1r, z9r


if __name__ == "__main__":
    binary = "./bench/bench_sqlite"
    args = ["--rows", "500000", "--cool", "30", "--serve", "20"]

    print("Page compressibility: cooling vs serve (SQLite 500K rows)")
    print("%-10s %-6s %7s %7s %7s %7s %7s" %
          ("Alloc", "Phase", "Pages", "Data", "LZ4", "zstd1", "zstd9"))
    print("-" * 56)

    for name, lib in [("glibc", None), ("smash", "./libsmash.so")]:
        env = os.environ.copy()
        if lib:
            env["LD_PRELOAD"] = lib
        env["SMASH_COLD_TIMEOUT_SEC"] = "1"
        proc = subprocess.Popen(
            [binary] + args, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        # Measure during cooling: fill ~3s + 12s into 30s cooling = t=15
        time.sleep(15)
        if proc.poll() is not None:
            print(f"{name}: CRASHED during cool")
            continue
        nz, dm, lz4r, z1r, z9r = scan_pid(proc.pid)
        print("%-10s %-6s %7d %5.1fM %6.2fx %6.2fx %6.2fx" %
              (name, "cool", nz, dm, lz4r, z1r, z9r))

        # Measure during serve: +25s = t=40 (7s into serve)
        time.sleep(25)
        if proc.poll() is not None:
            print(f"{name}: died before serve")
            continue
        nz, dm, lz4r, z1r, z9r = scan_pid(proc.pid)
        print("%-10s %-6s %7d %5.1fM %6.2fx %6.2fx %6.2fx" %
              (name, "serve", nz, dm, lz4r, z1r, z9r))

        proc.send_signal(signal.SIGKILL)
        proc.wait()
