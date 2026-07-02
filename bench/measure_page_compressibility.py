#!/usr/bin/env python3
"""measure_page_compressibility.py — Compare page compressibility across allocators.

Runs a workload binary under different allocators, then reads the process's
memory via /proc/<pid>/mem and compresses each page with LZ4 and zstd to
measure achievable compression ratios.

This is the EXTERNAL PROCESS approach: the measurement tool does not need
LD_PRELOAD (no bootstrapping issues). It attaches to the target process
after the workload reaches steady state.

Usage:
    python3 bench/measure_page_compressibility.py --binary ./bench/bench_sqlite --quick

Outputs per-allocator compression ratios on the cold pages.
"""

import argparse
import os
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

try:
    import lz4.block
    import zstandard as zstd
except ImportError:
    sys.exit("pip install lz4 zstandard")


PAGE_SIZE = 4096


def get_anon_rw_regions(pid):
    """Parse /proc/<pid>/maps for anonymous RW regions (heap data)."""
    regions = []
    maps_path = f"/proc/{pid}/maps"
    try:
        with open(maps_path) as f:
            for line in f:
                parts = line.split()
                if len(parts) < 5:
                    continue
                perms = parts[1]
                if perms[0] != 'r' or perms[1] != 'w':
                    continue
                inode = int(parts[4])
                if inode != 0:
                    continue  # file-backed
                # Skip stack, vdso, etc.
                if len(parts) > 5 and parts[5].startswith('['):
                    continue
                addr_range = parts[0].split('-')
                start = int(addr_range[0], 16)
                end = int(addr_range[1], 16)
                if end - start < PAGE_SIZE:
                    continue
                regions.append((start, end))
    except (FileNotFoundError, PermissionError):
        pass
    return regions


def compress_pages(pid, regions, max_pages=50000):
    """Read pages from /proc/<pid>/mem and compress each one."""
    mem_path = f"/proc/{pid}/mem"
    try:
        mem_fd = os.open(mem_path, os.O_RDONLY)
    except (FileNotFoundError, PermissionError) as e:
        print(f"  Cannot open {mem_path}: {e}")
        return None

    cctx = zstd.ZstdCompressor(level=1)
    total_bytes = 0
    lz4_bytes = 0
    zstd1_bytes = 0
    pages_read = 0
    pages_failed = 0

    for start, end in regions:
        for addr in range(start, end, PAGE_SIZE):
            if pages_read >= max_pages:
                break
            try:
                data = os.pread(mem_fd, PAGE_SIZE, addr)
                if len(data) != PAGE_SIZE:
                    pages_failed += 1
                    continue
            except OSError:
                pages_failed += 1
                continue

            pages_read += 1
            total_bytes += PAGE_SIZE

            # LZ4
            compressed = lz4.block.compress(data, store_size=False)
            lz4_bytes += len(compressed)

            # zstd level 1
            compressed = cctx.compress(data)
            zstd1_bytes += len(compressed)

        if pages_read >= max_pages:
            break

    os.close(mem_fd)

    if total_bytes == 0:
        return None

    return {
        "pages": pages_read,
        "total_mib": total_bytes / (1024 * 1024),
        "lz4_ratio": total_bytes / lz4_bytes if lz4_bytes > 0 else 0,
        "zstd1_ratio": total_bytes / zstd1_bytes if zstd1_bytes > 0 else 0,
        "pages_failed": pages_failed,
    }


def run_workload(binary, args, preload=None, cool_sec=5):
    """Start workload, wait for cooling, return pid."""
    env = os.environ.copy()
    if preload:
        env["LD_PRELOAD"] = preload
    # For smash
    env["SMASH_COLD_TIMEOUT_SEC"] = "1"

    proc = subprocess.Popen(
        [str(binary)] + args,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    # Wait for fill + cooling
    time.sleep(cool_sec)
    return proc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, default=Path("bench/bench_rss"))
    ap.add_argument("--args", default="--total-mb 64 --alloc-size 4096 --wait 15")
    ap.add_argument("--cool", type=int, default=8,
                    help="Seconds to wait before measuring (let pages go cold)")
    ap.add_argument("--smash-lib", type=Path, default=None)
    ap.add_argument("--jemalloc-lib", type=Path, default=None)
    ap.add_argument("--mimalloc-lib", type=Path, default=None)
    args = ap.parse_args()

    binary = args.binary
    bench_args = args.args.split()

    # Auto-detect libraries
    smash_lib = args.smash_lib
    if not smash_lib:
        for p in ["./libsmash.so", "./libsmash.dylib"]:
            if Path(p).exists():
                smash_lib = Path(p)
                break

    jemalloc_lib = args.jemalloc_lib
    if not jemalloc_lib:
        for p in ["/usr/lib64/libjemalloc.so", "/usr/local/lib/libjemalloc.so"]:
            if Path(p).exists():
                jemalloc_lib = Path(p)
                break

    mimalloc_lib = args.mimalloc_lib
    if not mimalloc_lib:
        for p in ["/usr/local/lib64/libmimalloc.so", "/usr/local/lib/libmimalloc.so"]:
            if Path(p).exists():
                mimalloc_lib = Path(p)
                break

    configs = [
        ("glibc", None),
        ("jemalloc", str(jemalloc_lib) if jemalloc_lib else None),
        ("mimalloc", str(mimalloc_lib) if mimalloc_lib else None),
        ("smash", str(smash_lib) if smash_lib else None),
    ]

    print(f"Binary: {binary}")
    print(f"Args: {' '.join(bench_args)}")
    print(f"Cool time: {args.cool}s")
    print()
    print(f"{'Allocator':<12} {'Pages':<8} {'Data':<8} {'LZ4':<8} {'zstd-1':<8}")
    print("-" * 48)

    for name, lib in configs:
        if name != "glibc" and not lib:
            continue

        proc = run_workload(binary, bench_args, preload=lib, cool_sec=args.cool)
        if proc.poll() is not None:
            print(f"{name:<12} CRASHED (exit {proc.returncode})")
            continue

        regions = get_anon_rw_regions(proc.pid)
        result = compress_pages(proc.pid, regions)

        # Kill the workload
        proc.send_signal(signal.SIGKILL)
        proc.wait()

        if result:
            print(f"{name:<12} {result['pages']:<8} "
                  f"{result['total_mib']:.1f}M   "
                  f"{result['lz4_ratio']:.2f}x   "
                  f"{result['zstd1_ratio']:.2f}x")
        else:
            print(f"{name:<12} NO DATA")

    print()
    print("Higher ratio = more compressible pages.")
    print("Smash's allocator layout (arenas, zero-on-free, metadata separation)")
    print("should produce higher ratios than glibc/jemalloc/mimalloc on the same data.")


if __name__ == "__main__":
    main()
