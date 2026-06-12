#!/usr/bin/env python3
"""plot_results.py — bar charts from a compress_only_results.json.

Reads the JSON written by run_paper_experiments.py (--compress-only-only) and
emits three PNGs into an output directory:
  - rss_reduction.png  : min RSS per allocator (grouped bars)
  - auc_reduction.png  : serve-phase RSS integral per allocator (grouped bars)
  - timing.png         : throughput per allocator (grouped bars)

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
    import numpy as np
except ImportError:
    sys.exit("matplotlib/numpy not installed: pip install matplotlib numpy")


# ── Style ─────────────────────────────────────────────────────────────────────

plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['Helvetica', 'Arial', 'DejaVu Sans'],
    'font.size': 14,
    'axes.labelsize': 15,
    'axes.titlesize': 16,
    'xtick.labelsize': 13,
    'ytick.labelsize': 13,
    'legend.fontsize': 12,
    'figure.titlesize': 17,
    'axes.spines.top': False,
    'axes.spines.right': False,
    'pdf.fonttype': 42,
    'ps.fonttype': 42,
})

# Okabe-Ito colorblind-safe palette. Smash is green (the "good" color).
ALLOC_STYLE = {
    "glibc":    {"color": "#56B4E9", "label": "glibc"},       # sky blue
    "jemalloc": {"color": "#E69F00", "label": "jemalloc"},    # orange
    "mimalloc": {"color": "#CC79A7", "label": "mimalloc"},    # reddish purple
    "smash":    {"color": "#009E73", "label": "smash"},       # bluish green
}

ALLOC_ORDER = ["glibc", "jemalloc", "mimalloc", "smash"]
ALLOC_KEYS = ["base", "jemalloc", "mimalloc", "full"]


# ── Display order + pretty labels ────────────────────────────────────────────

APP_ORDER = [
    ("sqlite", "SQLite"),
    ("rocksdb", "RocksDB"),
    ("memcached", "memcached"),
    ("redis", "Redis"),
    ("redis_ext", "Redis-ext"),
    ("redis_patched", "Redis†"),
    ("redis_ext_patched", "Redis-ext†"),
]


# ── Data loading ──────────────────────────────────────────────────────────────

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


# ── Plotting helpers ──────────────────────────────────────────────────────────

def _grouped_bar(ax, labels, alloc_data, active_allocs, fmt="{:.0f}"):
    """Draw grouped bars for the given allocators with value labels."""
    x = np.arange(len(labels))
    n = len(active_allocs)
    w = 0.75 / n
    for i, alloc_name in enumerate(active_allocs):
        key = ALLOC_KEYS[ALLOC_ORDER.index(alloc_name)]
        style = ALLOC_STYLE[alloc_name]
        offset = (i - n / 2 + 0.5) * w
        bars = ax.bar(x + offset, alloc_data[key], w,
                      label=style["label"], color=style["color"],
                      edgecolor="white", linewidth=0.5)
        for bar in bars:
            h = bar.get_height()
            if h <= 0:
                continue
            ax.annotate(fmt.format(h),
                        xy=(bar.get_x() + bar.get_width() / 2, h),
                        xytext=(0, 3), textcoords="offset points",
                        ha="center", va="bottom", fontsize=9,
                        color=style["color"], fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=25, ha="right")
    ax.legend(frameon=False)


def _active_allocs(alloc_data):
    """Return allocator names that have at least one nonzero entry."""
    active = []
    for name, key in zip(ALLOC_ORDER, ALLOC_KEYS):
        if any(v > 0 for v in alloc_data[key]):
            active.append(name)
    return active


# ── Charts ────────────────────────────────────────────────────────────────────

def plot_rss(rows, outdir: Path):
    labels = [r["label"] for r in rows]
    alloc_data = {k: [] for k in ALLOC_KEYS}
    for r in rows:
        alloc_data["base"].append(r["base"].get("min_rss_mb", r["base"].get("peak_rss_mb", 0)))
        alloc_data["full"].append(r["full"].get("min_rss_mb", 0))
        alloc_data["jemalloc"].append(
            r["jemalloc"].get("min_rss_mb", r["jemalloc"].get("peak_rss_mb", 0))
            if r.get("jemalloc") else 0)
        alloc_data["mimalloc"].append(
            r["mimalloc"].get("min_rss_mb", r["mimalloc"].get("peak_rss_mb", 0))
            if r.get("mimalloc") else 0)

    active = _active_allocs(alloc_data)
    fig, ax = plt.subplots(figsize=(11, 5.5))
    _grouped_bar(ax, labels, alloc_data, active, fmt="{:.0f}")
    ax.set_title("Minimum RSS (lower is better)")
    ax.set_ylabel("RSS (MiB)")
    ax.set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(outdir / "rss_reduction.png", dpi=150)
    plt.close(fig)


def plot_auc(rows, outdir: Path):
    labels, alloc_data = [], {k: [] for k in ALLOC_KEYS}
    for r in rows:
        b_auc = r["base"].get("serve_auc_mb_sec", r["base"].get("auc_mb_sec"))
        f_auc = r["full"].get("serve_auc_mb_sec", r["full"].get("auc_mb_sec"))
        if not b_auc or b_auc <= 0 or f_auc is None:
            continue
        labels.append(r["label"])
        alloc_data["base"].append(b_auc)
        alloc_data["full"].append(f_auc)
        j = r.get("jemalloc", {})
        alloc_data["jemalloc"].append(
            j.get("serve_auc_mb_sec", j.get("auc_mb_sec", b_auc)) if j else b_auc)
        m = r.get("mimalloc", {})
        alloc_data["mimalloc"].append(
            m.get("serve_auc_mb_sec", m.get("auc_mb_sec", b_auc)) if m else b_auc)
    if not labels:
        return

    active = _active_allocs(alloc_data)
    fig, ax = plt.subplots(figsize=(11, 5.5))
    _grouped_bar(ax, labels, alloc_data, active, fmt="{:.0f}")
    ax.set_title("RSS × Time During Serve (lower is better)")
    ax.set_ylabel("MB · s")
    ax.set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(outdir / "auc_reduction.png", dpi=150)
    plt.close(fig)


def plot_timing(rows, outdir: Path):
    labels, alloc_data = [], {k: [] for k in ALLOC_KEYS}
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

    active = _active_allocs(alloc_data)
    fig, ax = plt.subplots(figsize=(11, 5.5))
    _grouped_bar(ax, labels, alloc_data, active, fmt="{:.0f}")
    ax.set_title("Throughput (higher is better)")
    ax.set_ylabel("kops/s")
    ax.set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(outdir / "timing.png", dpi=150)
    plt.close(fig)


# ── Main ──────────────────────────────────────────────────────────────────────

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
