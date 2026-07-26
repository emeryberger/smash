#!/usr/bin/env python3
"""Sample peak/avg RSS of a process tree.

Usage:
    rss_sampler.py --interval 0.5 --label foo --out foo.rss.csv -- prog [args...]

Tracks the launched process AND any descendants it forks (ProcessPoolExecutor
children, hlo2penguin subprocesses, etc.) each tick. Writes a CSV
(timestamp_s, total_rss_kb, num_procs) and prints peak/avg/wall on stdout when
the child exits.

Works on macOS AND Linux via the shared tools/footprint helper: macOS samples
phys_footprint (matching the C++ ri_phys_footprint monitors), Linux samples
/proc VmRSS. The CSV carries a `# metric_source:` header so its provenance is
self-describing.
"""
import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

# Shared per-pid footprint helper (tools/footprint.py, same directory).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from footprint import METRIC_SOURCE, descendant_pids, pid_footprint_kb


def descendants(root_pid: int) -> list[int]:
    """Return root_pid + every PID currently in its descendant tree."""
    return descendant_pids(root_pid)


def rss_of(pid: int) -> int:
    """Footprint in kilobytes for a single PID, or 0 if dead."""
    return pid_footprint_kb(pid)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--interval', type=float, default=0.5,
                    help='sampling interval in seconds')
    ap.add_argument('--label', default='run', help='label written into the CSV')
    ap.add_argument('--out', help='CSV output path')
    ap.add_argument('cmd', nargs=argparse.REMAINDER,
                    help='-- followed by the program to run')
    args = ap.parse_args()
    if not args.cmd:
        ap.error('command is required after --')
    if args.cmd[0] == '--':
        args.cmd = args.cmd[1:]

    out_f = Path(args.out).open('w') if args.out else None
    if out_f:
        out_f.write(f'# metric_source: {METRIC_SOURCE}\n')
        out_f.write('label,timestamp_s,total_rss_kb,num_procs\n')

    start = time.monotonic()
    proc = subprocess.Popen(args.cmd)
    samples: list[tuple[float, int, int]] = []
    try:
        while proc.poll() is None:
            pids = descendants(proc.pid)
            total_kb = sum(rss_of(p) for p in pids)
            t = time.monotonic() - start
            samples.append((t, total_kb, len(pids)))
            if out_f:
                out_f.write(f'{args.label},{t:.3f},{total_kb},{len(pids)}\n')
                out_f.flush()
            time.sleep(args.interval)
    except KeyboardInterrupt:
        proc.send_signal(signal.SIGTERM)
        proc.wait()
        return 130
    finally:
        if out_f:
            out_f.close()

    rc = proc.returncode
    wall = time.monotonic() - start
    if not samples:
        print(f'[rss_sampler] {args.label}: no samples (process exited too fast)',
              file=sys.stderr)
        return rc
    peak_kb = max(s[1] for s in samples)
    avg_kb = sum(s[1] for s in samples) / len(samples)
    peak_procs = max(s[2] for s in samples)
    print(f'[rss_sampler] label={args.label} rc={rc} wall={wall:.1f}s '
          f'peak_rss={peak_kb//1024} MB avg_rss={int(avg_kb)//1024} MB '
          f'peak_procs={peak_procs} samples={len(samples)}',
          file=sys.stderr)
    return rc


if __name__ == '__main__':
    sys.exit(main())
