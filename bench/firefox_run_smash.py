#!/usr/bin/env python3
"""Run Firefox under Smash with live diagnostics, then summarise activity.

Usage:
    python3 bench/firefox_run_smash.py /path/to/libsmash.so /path/to/firefox

What it does:
1. Kills any running snap-firefox (it competes for window focus and the
   diagnose script's earlier output showed users often have one running).
2. Launches the firefox you specified under
       LD_PRELOAD=libsmash.so
       SMASH_BANNER=1  SMASH_DEBUG=1  SMASH_STATS=1
       SMASH_DEFER_PHASES_MS=30000  SMASH_TRACK_EXTERNAL=1
       SMASH_LARGE_ONLY=0
3. Streams Firefox's stderr to your terminal and to firefox-smash.log
   so you can see [smash debug] / [smash stats] lines live AND analyse
   them after.
4. Waits for Firefox to exit (browse normally; quit when ready).
5. Parses the log: per-process timeline of committed/compressed pages,
   then a verdict — did Firefox's allocations actually flow through
   smash, or did mozjemalloc bypass us?
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


def stop_snap_firefox() -> None:
    """If snap-firefox is running, stop it. It competes for keyboard
    focus and adds noise to the process listing."""
    if shutil.which("snap"):
        if "firefox" in subprocess.run(
                ["snap", "list"], capture_output=True, text=True).stdout:
            print(">>> stopping snap firefox to avoid window-focus competition")
            subprocess.run(["snap", "stop", "firefox"],
                           check=False, capture_output=True)
    subprocess.run(["pkill", "-f", "/snap/firefox"],
                   check=False, capture_output=True)


def run_firefox(libsmash: str, firefox: str, log_path: str) -> int:
    env = os.environ.copy()
    env["LD_PRELOAD"] = os.path.abspath(libsmash)
    env["SMASH_BANNER"] = "1"
    env["SMASH_DEBUG"] = "1"
    env["SMASH_STATS"] = "1"
    env["SMASH_DEFER_PHASES_MS"] = "30000"
    env["SMASH_TRACK_EXTERNAL"] = "1"
    env["SMASH_LARGE_ONLY"] = "0"

    print(f">>> launching {firefox}")
    print(">>> Browse normally. Quit Firefox when you're done; this script "
          "will then print a summary.")
    print(f">>> Output → terminal AND {log_path}")
    print()

    log = open(log_path, "wb")
    proc = subprocess.Popen(
        [firefox],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0)
    assert proc.stdout is not None  # PIPE → always set
    try:
        while True:
            chunk = proc.stdout.read(1)
            if not chunk:
                break
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            log.write(chunk)
    except KeyboardInterrupt:
        proc.terminate()
    finally:
        log.close()
    return proc.wait()


# ── log analysis ────────────────────────────────────────────────────────────


# Lines look like:
#   [smash stats] [2026-05-07 12:34:56] pid=12345 committed=8265  active=43  monitor=0  compressing=0  compressed=8222  empty=0
STATS_RE = re.compile(
    r"\[smash stats\] \[([0-9-]+ [0-9:]+)\] pid=(\d+) "
    r"committed=(\d+)\s+active=(\d+)\s+monitor=(\d+)\s+"
    r"compressing=(\d+)\s+compressed=(\d+)\s+empty=(\d+)"
)


def analyse(log_path: str) -> None:
    """Read the log; per-pid track committed/compressed peak; print verdict."""
    if not Path(log_path).exists():
        return
    by_pid: dict[int, dict[str, int]] = defaultdict(
        lambda: {"committed_max": 0, "compressed_max": 0,
                 "active_max": 0, "samples": 0})
    for raw in Path(log_path).read_text(errors="replace").splitlines():
        m = STATS_RE.search(raw)
        if not m:
            continue
        _ts, pid_s, committed, active, _mon, _comp_ing, compressed, _empty = m.groups()
        pid = int(pid_s)
        d = by_pid[pid]
        d["committed_max"] = max(d["committed_max"], int(committed))
        d["active_max"] = max(d["active_max"], int(active))
        d["compressed_max"] = max(d["compressed_max"], int(compressed))
        d["samples"] += 1

    if not by_pid:
        print()
        print("=" * 78)
        print("ANALYSIS")
        print("=" * 78)
        print("No [smash stats] lines were captured. Likely causes:")
        print("  - libsmash didn't actually load — re-run firefox_diagnose.py")
        print("  - Firefox exited before any compressor tick fired (~5s+)")
        return

    PAGE = 4096  # Linux x86_64/aarch64 base page size
    print()
    print("=" * 78)
    print("ANALYSIS")
    print("=" * 78)
    print(f"{'pid':>7}  {'samples':>7}  {'committed_max':>14}  "
          f"{'compressed_max':>15}  {'compressed_MB':>12}")
    grand_committed = 0
    grand_compressed = 0
    for pid in sorted(by_pid):
        d = by_pid[pid]
        c = d["committed_max"]
        cz = d["compressed_max"]
        grand_committed += c
        grand_compressed += cz
        mb = cz * PAGE / (1024 * 1024)
        print(f"{pid:>7}  {d['samples']:>7}  {c:>14}  {cz:>15}  {mb:>11.1f}")

    total_mb = grand_committed * PAGE / (1024 * 1024)
    compressed_mb = grand_compressed * PAGE / (1024 * 1024)
    print()
    print(f"across all processes:")
    print(f"  total committed pages: {grand_committed} ({total_mb:.0f} MB)")
    print(f"  total compressed pages: {grand_compressed} ({compressed_mb:.0f} MB)")
    if grand_committed:
        ratio = grand_compressed / grand_committed
        print(f"  compressed/committed: {ratio:.1%}")

    print()
    print("VERDICT:")
    if grand_committed < 256:
        print(f"  ⚠️  committed pages stayed very low ({grand_committed}). Most")
        print(f"     likely cause: Firefox's bundled mozjemalloc is bypassing")
        print(f"     our malloc interposer for its big allocation chunks via")
        print(f"     direct mmap. SMASH_TRACK_EXTERNAL=1 catches some of those")
        print(f"     but coverage is partial. To fully test smash on Firefox,")
        print(f"     build Firefox with `ac_add_options --disable-jemalloc`.")
    elif grand_compressed < grand_committed * 0.05:
        print(f"  Smash sees the heap (committed={grand_committed} pages) but")
        print(f"  is barely compressing it ({grand_compressed} pages = "
              f"{grand_compressed*100/grand_committed:.1f}%).")
        print(f"  Either Firefox keeps every page hot (unlikely under headless),")
        print(f"  or compression is being skipped — investigate with")
        print(f"  SMASH_COLD_TIMEOUT_SEC=1 and a longer dwell.")
    else:
        print(f"  ✅ Smash compressed {grand_compressed*100/grand_committed:.1f}% "
              f"of Firefox's heap pages")
        print(f"     ({compressed_mb:.0f} MB compressed / {total_mb:.0f} MB total).")
        print(f"     Real compression activity is happening.")


# ── entrypoint ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("libsmash", help="Path to libsmash.so")
    ap.add_argument("firefox", help="Path to firefox binary (NOT snap)")
    ap.add_argument("--log", default="firefox-smash.log",
                    help="Output log file (default: firefox-smash.log)")
    ap.add_argument("--no-stop-snap", action="store_true",
                    help="Don't stop snap-firefox before launching")
    args = ap.parse_args()

    if sys.platform != "linux":
        print("Linux-only.", file=sys.stderr)
        return 2
    if not os.path.exists(args.libsmash):
        print(f"libsmash not found: {args.libsmash}", file=sys.stderr)
        return 2
    if not os.path.exists(args.firefox):
        print(f"firefox not found: {args.firefox}", file=sys.stderr)
        return 2
    if "/snap/" in os.path.realpath(args.firefox):
        print(f"refuse to run snap firefox ({args.firefox}); LD_PRELOAD won't "
              "stick through the snap sandbox boundary.", file=sys.stderr)
        return 2

    if not args.no_stop_snap:
        stop_snap_firefox()

    rc = run_firefox(args.libsmash, args.firefox, args.log)
    analyse(args.log)
    print(f"\n>>> full log saved to {args.log}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
