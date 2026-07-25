#!/usr/bin/env python3
"""Run a command and sample the summed RSS of its whole process subtree.

Usage: rss_sample.py [--interval SEC] [--label NAME] [--csv FILE] -- CMD ARGS...

Prints a timeline and, at exit, peak-tree-RSS and the RSS at the process's own
low-water mark (min after peak) so we can see compressor-driven RSS reduction.
Also reports per-process peak for the largest child (e.g. walrus_driver).

Works on macOS AND Linux via the shared tools/footprint helper: macOS samples
phys_footprint (matching the C++ ri_phys_footprint monitors), Linux samples
/proc VmRSS. The CSV carries a `# metric_source:` header so its provenance is
self-describing.
"""
import os, sys, time, subprocess, signal

# Shared per-pid footprint helper (repo tools/footprint.py).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools"))
from footprint import METRIC_SOURCE, descendant_pids, pid_footprint_kb

def parse():
    interval, label, csv, cmd = 0.1, "run", None, None
    a = sys.argv[1:]
    while a:
        if a[0] == "--interval": interval = float(a[1]); a = a[2:]
        elif a[0] == "--label": label = a[1]; a = a[2:]
        elif a[0] == "--csv": csv = a[1]; a = a[2:]
        elif a[0] == "--": cmd = a[1:]; break
        else: a = a[1:]
    if not cmd:
        sys.exit("need -- CMD")
    return interval, label, csv, cmd

def tree_pids(root):
    """All pids in the subtree rooted at `root` (macOS ps / Linux /proc)."""
    return descendant_pids(root)

def rss_kb(pid):
    """Footprint in kilobytes: macOS phys_footprint, Linux /proc VmRSS."""
    return pid_footprint_kb(pid)

def comm(pid):
    try:
        with open(f"/proc/{pid}/comm") as f: return f.read().strip()
    except OSError: return "?"

def main():
    interval, label, csv, cmd = parse()
    proc = subprocess.Popen(cmd)
    root = proc.pid
    t0 = time.time()
    peak_tree = 0
    per_proc_peak = {}       # comm -> peak kb (single process instance max)
    samples = []             # (t, tree_kb)
    csvf = open(csv, "w") if csv else None
    if csvf:
        csvf.write(f"# metric_source: {METRIC_SOURCE}\n")
        csvf.write("t_sec,tree_rss_kb\n")
    try:
        while proc.poll() is None:
            pids = tree_pids(root)
            tree = 0
            for p in pids:
                r = rss_kb(p)
                tree += r
                c = comm(p)
                if r > per_proc_peak.get(c, 0): per_proc_peak[c] = r
            t = time.time() - t0
            if tree > peak_tree: peak_tree = tree
            samples.append((t, tree))
            if csvf: csvf.write(f"{t:.3f},{tree}\n")
            time.sleep(interval)
    except KeyboardInterrupt:
        proc.send_signal(signal.SIGINT)
    rc = proc.wait()
    if csvf: csvf.close()

    # low-water mark after the peak (compressor reduction visible here)
    peak_i = max(range(len(samples)), key=lambda i: samples[i][1]) if samples else 0
    post = samples[peak_i:] if samples else []
    minpost = min((s[1] for s in post), default=peak_tree)
    reduction = (1 - minpost / peak_tree) * 100 if peak_tree else 0
    print(f"\n=== rss_sample [{label}] rc={rc} ===", flush=True)
    print(f"  peak tree RSS   : {peak_tree/1024:8.0f} MiB", flush=True)
    print(f"  min after peak  : {minpost/1024:8.0f} MiB  ({reduction:.1f}% below peak)", flush=True)
    top = sorted(per_proc_peak.items(), key=lambda kv: -kv[1])[:6]
    print(f"  per-process peak (single-instance max):", flush=True)
    for c, r in top:
        print(f"      {r/1024:8.0f} MiB  {c}", flush=True)
    sys.exit(rc)

if __name__ == "__main__":
    main()
