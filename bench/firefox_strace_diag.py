#!/usr/bin/env python3
"""Trace Firefox's mmap activity under smash to diagnose why
SMASH_TRACK_EXTERNAL=1 isn't seeing Firefox's allocations.

Two hypotheses to distinguish:
  1. mozjemalloc bypasses our `mmap` symbol entirely (direct syscall
     or unversioned glibc symbol we don't export).
  2. mozjemalloc's mmaps DO reach our wrapper but our filter rejects
     them — registerLinuxExternalRange currently requires
     MAP_ANONYMOUS + PROT_WRITE, but allocator-style code often does
     mmap(PROT_NONE) → mprotect(PROT_RW) on demand.

Usage:
    python3 firefox_strace_diag.py /path/to/libsmash.so /path/to/firefox-bin

Output: a tally of mmap/mprotect calls Firefox actually made vs the
committed-pages count smash actually saw, plus a concrete suggestion
for which fix matches the data.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path


# ── kill leftover firefoxes (without targeting our own pid) ─────────────────


def kill_firefoxes() -> None:
    me = os.getpid()
    parents = {me, os.getppid()}
    victims = []
    for p in Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        pid = int(p.name)
        if pid in parents:
            continue
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue
        base = os.path.basename(exe)
        if base in ("firefox", "firefox-bin", "crashhelper") or "/firefox/" in exe:
            victims.append(pid)
    for pid in victims:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    time.sleep(2)
    for pid in victims:
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    if victims:
        print(f">>> killed {len(victims)} leftover firefox process(es)")


# ── launch + capture ────────────────────────────────────────────────────────


def run(libsmash: str, firefox: str, dwell_sec: int,
        ff_log: str, strace_log: str) -> None:
    profile = tempfile.mkdtemp(prefix="smash-strace-ff-")
    env = os.environ.copy()
    env["LD_PRELOAD"] = os.path.abspath(libsmash)
    env["SMASH_BANNER"] = "1"
    env["SMASH_DEBUG"] = "1"
    env["SMASH_STATS"] = "1"
    env["SMASH_TRACK_EXTERNAL"] = "1"
    env["SMASH_LARGE_ONLY"] = "0"
    env["MOZ_DISABLE_CONTENT_SANDBOX"] = "1"

    cmd = [
        "strace", "-f", "-q",
        "-e", "trace=mmap,mmap2,mprotect,munmap",
        "-o", strace_log,
        firefox, "--headless", "--no-remote",
        "--profile", profile,
        "https://en.wikipedia.org/wiki/Compression",
    ]
    print(f">>> launching: {' '.join(cmd[:6])} … under smash")
    print(f">>> dwell {dwell_sec}s, then SIGTERM")

    with open(ff_log, "wb") as flog:
        proc = subprocess.Popen(
            cmd, env=env, stdout=flog, stderr=subprocess.STDOUT,
            preexec_fn=os.setsid)
        time.sleep(dwell_sec)
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)


# ── analysis ────────────────────────────────────────────────────────────────


# strace formats one mmap line per call:
#   12345 mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f…
MMAP_RE = re.compile(
    r"\bmmap2?\(\w+,\s*(\d+),\s*([A-Z_|]+),\s*([A-Z_|]+),"
)
MPROTECT_RE = re.compile(
    r"\bmprotect\(\w+,\s*(\d+),\s*([A-Z_|]+)\)"
)

STATS_RE = re.compile(
    r"\[smash stats\](?:\s+\[?[0-9-]+ [0-9:]+\]?)?\s+pid=(\d+)\s+"
    r"committed=(\d+)\s+active=(\d+)\s+monitor=(\d+)\s+"
    r"compressing=(\d+)\s+compressed=(\d+)"
)
BANNER_RE = re.compile(r"\[smash\][^\n]*\bloaded\b\s+pid=(\d+)")
DEBUG_RE = re.compile(r"\[smash debug\][^\n]*compressor start\s+pid=(\d+)")


def analyse(ff_log: str, strace_log: str) -> None:
    ff_text = Path(ff_log).read_text(errors="replace") if Path(ff_log).exists() else ""
    st_text = Path(strace_log).read_text(errors="replace") if Path(strace_log).exists() else ""

    banners = set(BANNER_RE.findall(ff_text))
    debugs = set(DEBUG_RE.findall(ff_text))
    committed_max = 0
    compressed_max = 0
    pids_with_stats: set[str] = set()
    for m in STATS_RE.finditer(ff_text):
        pid, c, _a, _m, _ci, cz = m.groups()
        pids_with_stats.add(pid)
        committed_max = max(committed_max, int(c))
        compressed_max = max(compressed_max, int(cz))

    mmap_total = 0
    mmap_anon = 0
    mmap_prot_none = 0
    mmap_prot_write = 0
    mmap_size_buckets: Counter = Counter()
    for m in MMAP_RE.finditer(st_text):
        size, prot, flags = m.groups()
        sz = int(size)
        mmap_total += 1
        if "MAP_ANONYMOUS" in flags or "MAP_ANON" in flags:
            mmap_anon += 1
        if prot == "PROT_NONE":
            mmap_prot_none += 1
        if "PROT_WRITE" in prot:
            mmap_prot_write += 1
        if sz < 64 * 1024:
            mmap_size_buckets["<64K"] += 1
        elif sz < 1024 * 1024:
            mmap_size_buckets["64K-1M"] += 1
        elif sz < 16 * 1024 * 1024:
            mmap_size_buckets["1M-16M"] += 1
        else:
            mmap_size_buckets[">=16M"] += 1

    mprotect_total = 0
    mprotect_to_rw = 0
    for m in MPROTECT_RE.finditer(st_text):
        _size, prot = m.groups()
        mprotect_total += 1
        if "PROT_WRITE" in prot:
            mprotect_to_rw += 1

    print()
    print("=" * 78)
    print("RESULTS")
    print("=" * 78)
    print(f"firefox processes that printed [smash] banner:      {len(banners)}")
    print(f"firefox processes that started a smash compressor:  {len(debugs)}")
    print(f"firefox processes that emitted [smash stats]:       {len(pids_with_stats)}")
    print(f"max committed pages smash ever saw (across all):    {committed_max}")
    print(f"max compressed pages smash ever did:                {compressed_max}")
    print()
    print(f"mmap calls (all)        : {mmap_total}")
    print(f"  with MAP_ANONYMOUS    : {mmap_anon}")
    print(f"  with PROT_NONE        : {mmap_prot_none}")
    print(f"  with PROT_WRITE       : {mmap_prot_write}")
    print(f"mmap size buckets       : {dict(mmap_size_buckets)}")
    print(f"mprotect calls (all)    : {mprotect_total}")
    print(f"  upgrading to PROT_RW  : {mprotect_to_rw}")

    print()
    print("=" * 78)
    print("VERDICT")
    print("=" * 78)
    if mmap_total == 0:
        print("strace captured zero mmap calls — strace itself didn't run, or")
        print("Firefox didn't survive long enough. Check strace.log size.")
        return
    print(f"firefox issued {mmap_total} mmap calls; {mmap_anon} anonymous.")
    print(f"of those, {mmap_prot_write} had PROT_WRITE (would match smash's filter).")
    print(f"{mmap_prot_none} were PROT_NONE (filter currently REJECTS these).")
    print(f"{mprotect_to_rw} mprotect→RW upgrades happened (these are what")
    print("   mozjemalloc-style allocators do after the PROT_NONE reservation).")
    print()
    if committed_max < 50 and mmap_anon > 100:
        print("⚠️  Firefox made many anonymous mmap calls but smash saw < 50 pages")
        print("    of committed memory.")
        if mmap_prot_none > mmap_prot_write * 2:
            print("    Most are PROT_NONE — they are being REJECTED by the")
            print("    SMASH_TRACK_EXTERNAL filter at registerLinuxExternalRange.")
            print("    FIX: relax the filter to also accept PROT_NONE mappings,")
            print("    or hook mprotect to register-on-upgrade.")
        else:
            print("    The mmap *symbol* may be bypassed entirely (Firefox calls")
            print("    syscall(SYS_mmap) or __mmap directly). Check with:")
            print("      LD_DEBUG=symbols LD_PRELOAD=… firefox 2>&1 | grep mmap | head")
    elif committed_max < 50:
        print("strace shows few anonymous mmaps — Firefox isn't allocating much")
        print("here. Try a heavier workload (load multiple tabs / a JS-heavy site).")
    else:
        print(f"smash saw {committed_max} committed pages — meaningful coverage.")


# ── entrypoint ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("libsmash", help="Path to libsmash.so")
    ap.add_argument("firefox", help="Path to firefox or firefox-bin")
    ap.add_argument("--dwell", type=int, default=60,
                    help="Seconds to let firefox run (default 60)")
    ap.add_argument("--ff-log", default="ff.log")
    ap.add_argument("--strace-log", default="strace.log")
    args = ap.parse_args()

    if sys.platform != "linux":
        print("Linux-only.", file=sys.stderr)
        return 2
    if not Path(args.libsmash).exists():
        print(f"libsmash not found: {args.libsmash}", file=sys.stderr)
        return 2
    if not Path(args.firefox).exists():
        print(f"firefox not found: {args.firefox}", file=sys.stderr)
        return 2
    if "/snap/" in os.path.realpath(args.firefox):
        print(f"refuse to run snap firefox ({args.firefox}); use a tarball.",
              file=sys.stderr)
        return 2
    if not subprocess.run(["which", "strace"], capture_output=True).stdout.strip():
        print("strace not installed. sudo apt install strace", file=sys.stderr)
        return 2

    kill_firefoxes()
    run(args.libsmash, args.firefox, args.dwell, args.ff_log, args.strace_log)
    analyse(args.ff_log, args.strace_log)
    print(f"\n>>> firefox stderr: {args.ff_log}")
    print(f">>> strace output : {args.strace_log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
