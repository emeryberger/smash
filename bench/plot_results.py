#!/usr/bin/env python3
"""plot_results.py — bar charts from a compress_only_results.json.

Reads the JSON written by run_paper_experiments.py (--compress-only-only) and
emits three PNGs into an output directory:
  - rss_reduction.png  : peak→min RSS reduction (%) per app, full smash vs baseline
  - auc_reduction.png   : AUC (MB·s) reduction (%) per app vs baseline
  - timing.png          : throughput (ops/s) full smash vs baseline, where available

Each app has a <app>_baseline and <app>_full_smash entry; reductions are computed
baseline→full_smash. Apps without an ops/sec metric are omitted from timing.png.

Usage:
    python3 bench/plot_results.py <results.json> [--outdir docs/figures]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")  # headless
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib not installed: pip install matplotlib")


# Display order + pretty labels.
APP_ORDER = [
    ("sqlite", "SQLite"),
    ("rocksdb", "RocksDB"),
    ("memcached", "memcached"),
    ("redis", "Redis"),
    ("redis_ext", "Redis-ext"),
    ("redis_patched", "Redis†"),
    ("redis_ext_patched", "Redis-ext†"),
]


def load(results_path: Path) -> list:
    data = json.loads(results_path.read_text())

    def first_run(key):
        v = data.get(key)
        if not isinstance(v, dict):
            return None
        runs = v.get("runs")
        return runs[0] if runs else None

    rows = []
    for key, label in APP_ORDER:
        base = first_run(f"{key}_baseline")
        full = first_run(f"{key}_full_smash")
        if not base or not full:
            continue
        row = {"label": label, "base": base, "full": full}
        jemalloc = first_run(f"{key}_jemalloc")
        mimalloc = first_run(f"{key}_mimalloc")
        if jemalloc:
            row["jemalloc"] = jemalloc
        if mimalloc:
            row["mimalloc"] = mimalloc
        rows.append(row)
    return rows


def _bar(ax, labels, values, title, ylabel, color, fmt="{:.0f}"):
    bars = ax.bar(labels, values, color=color)
    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.axhline(0, color="black", linewidth=0.6)
    ax.tick_params(axis="x", rotation=30)
    for b, v in zip(bars, values):
        ax.annotate(fmt.format(v), (b.get_x() + b.get_width() / 2, v),
                    ha="center", va="bottom" if v >= 0 else "top", fontsize=8)
    return bars


def plot_rss(rows, outdir: Path):
    labels = [r["label"] for r in rows]
    red = [r["full"].get("rss_reduction_pct", 0.0) for r in rows]
    fig, ax = plt.subplots(figsize=(9, 4.5))
    _bar(ax, labels, red, "Peak→Min RSS Reduction (full smash)",
         "RSS reduction (%)", "#2a7ab9", "{:.1f}%")
    fig.tight_layout()
    fig.savefig(outdir / "rss_reduction.png", dpi=130)
    plt.close(fig)


def plot_auc(rows, outdir: Path):
    labels, red = [], []
    for r in rows:
        b = r["base"].get("serve_auc_mb_sec", r["base"].get("auc_mb_sec"))
        f = r["full"].get("serve_auc_mb_sec", r["full"].get("auc_mb_sec"))
        if not b or b <= 0 or f is None:
            continue
        labels.append(r["label"])
        red.append((1.0 - f / b) * 100.0)
    if not labels:
        return
    fig, ax = plt.subplots(figsize=(9, 4.5))
    _bar(ax, labels, red, "Serve-Phase AUC Reduction vs baseline (MB·s)",
         "AUC reduction (%)", "#3c9d4e", "{:.1f}%")
    fig.tight_layout()
    fig.savefig(outdir / "auc_reduction.png", dpi=130)
    plt.close(fig)


def plot_timing(rows, outdir: Path):
    import numpy as np

    # Collect data for all allocators that have ops
    alloc_names = ["baseline", "jemalloc", "mimalloc", "full smash"]
    alloc_keys = ["base", "jemalloc", "mimalloc", "full"]
    alloc_colors = ["#999999", "#e6a817", "#4caf50", "#2a7ab9"]

    labels, alloc_data = [], {k: [] for k in alloc_keys}
    for r in rows:
        b = r["base"].get("ops_per_sec")
        f = r["full"].get("ops_per_sec")
        if not b or not f:
            continue
        labels.append(r["label"])
        alloc_data["base"].append(b / 1e3)
        alloc_data["full"].append(f / 1e3)
        alloc_data["jemalloc"].append(
            r["jemalloc"]["ops_per_sec"] / 1e3 if r.get("jemalloc", {}).get("ops_per_sec") else 0)
        alloc_data["mimalloc"].append(
            r["mimalloc"]["ops_per_sec"] / 1e3 if r.get("mimalloc", {}).get("ops_per_sec") else 0)
    if not labels:
        return

    # Only plot allocators that have at least one nonzero entry
    active = [(name, key, color) for name, key, color in
              zip(alloc_names, alloc_keys, alloc_colors)
              if any(v > 0 for v in alloc_data[key])]

    x = np.arange(len(labels))
    n = len(active)
    w = 0.8 / n
    fig, ax = plt.subplots(figsize=(10, 4.5))
    for i, (name, key, color) in enumerate(active):
        offset = (i - n / 2 + 0.5) * w
        vals = alloc_data[key]
        ax.bar(x + offset, vals, w, label=name, color=color)
    ax.set_title("Throughput by allocator")
    ax.set_ylabel("kops/s")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=30)
    ax.legend()
    fig.tight_layout()
    fig.savefig(outdir / "timing.png", dpi=130)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("results", type=Path, help="compress_only_results.json")
    ap.add_argument("--outdir", type=Path, default=Path("docs/figures"))
    args = ap.parse_args()

    rows = load(args.results)
    if not rows:
        sys.exit("no <app>_baseline / <app>_full_smash pairs found in JSON")
    args.outdir.mkdir(parents=True, exist_ok=True)
    plot_rss(rows, args.outdir)
    plot_auc(rows, args.outdir)
    plot_timing(rows, args.outdir)
    print(f"wrote figures to {args.outdir}/ ({len(rows)} apps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
