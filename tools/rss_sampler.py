#!/usr/bin/env python3
"""Sample peak/avg RSS of a process tree.

Usage:
    rss_sampler.py --interval 0.5 --label foo --out foo.rss.csv -- prog [args...]

Tracks the launched process AND any descendants it forks (ProcessPoolExecutor
children, hlo2penguin subprocesses, etc.) by walking /proc each tick. Writes
a CSV (timestamp_s, total_rss_kb, num_procs) and prints peak/avg/wall on
stdout when the child exits.
"""
import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


def descendants(root_pid: int) -> list[int]:
    """Return root_pid + every PID currently in its descendant tree."""
    pids = {root_pid}
    # Build a parent->children map by reading /proc/*/stat once.
    children: dict[int, list[int]] = {}
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        pid = int(entry)
        try:
            with open(f'/proc/{pid}/stat') as f:
                fields = f.read().rsplit(')', 1)[1].split()
            ppid = int(fields[1])
            children.setdefault(ppid, []).append(pid)
        except (FileNotFoundError, ProcessLookupError, IndexError):
            continue
    queue = [root_pid]
    while queue:
        p = queue.pop()
        for c in children.get(p, ()):
            if c not in pids:
                pids.add(c)
                queue.append(c)
    return list(pids)


def rss_of(pid: int) -> int:
    """RSS in kilobytes for a single PID, or 0 if dead."""
    try:
        with open(f'/proc/{pid}/status') as f:
            for line in f:
                if line.startswith('VmRSS:'):
                    return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError):
        pass
    return 0


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
