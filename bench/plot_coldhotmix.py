#!/usr/bin/env python3
"""Publication-ready plots for bench_coldhotmix CSV output.

Generates three figures from the CSV files produced by bench_coldhotmix:
  1. RSS timeline           — RSS (MiB) over time for each run
  2. Hot-access latency CDF — CDF of hot-path access latencies
  3. Cold-access latency CDF — CDF of cold re-access (decompression) latencies

Usage:
  # Plot a single run
  python3 plot_coldhotmix.py results/baseline

  # Compare two runs (e.g. baseline vs smash)
  python3 plot_coldhotmix.py results/baseline results/smash

  # Specify output directory
  python3 plot_coldhotmix.py -o figures/ results/baseline results/smash

  # Custom labels
  python3 plot_coldhotmix.py -l "System malloc" -l "Smash" results/baseline results/smash

Each positional argument is a label prefix (the same value passed to
bench_coldhotmix --label). The script looks for <prefix>_rss.csv,
<prefix>_hot.csv, and <prefix>_cold.csv in the same directory (or the
directory given by --dir).
"""

import argparse
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

try:
    import seaborn as sns
    sns.set_theme(style='whitegrid', context='paper', font_scale=1.2)
except ImportError:
    pass

# ── Style ────────────────────────────────────────────────────────────────────

plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['Helvetica', 'Arial', 'DejaVu Sans'],
    'axes.titlesize': 11,
    'axes.labelsize': 10,
    'xtick.labelsize': 9,
    'ytick.labelsize': 9,
    'legend.fontsize': 8,
    'figure.dpi': 300,
    'savefig.dpi': 300,
})

COLORS = ['#1f77b4', '#d62728', '#2ca02c', '#ff7f0e', '#9467bd']
LINESTYLES = ['-', '--', '-.', ':', (0, (3, 1, 1, 1))]

# ── Data loading ─────────────────────────────────────────────────────────────

def load_csv(path):
    """Load a single-column or two-column CSV (with header) into numpy arrays."""
    if not os.path.isfile(path):
        return None
    data = np.genfromtxt(path, delimiter=',', names=True, dtype=None, encoding=None)
    return data


def resolve_paths(prefix):
    """Given a prefix like 'results/baseline', return dict of CSV paths."""
    d = os.path.dirname(prefix) or '.'
    b = os.path.basename(prefix)
    return {
        'rss':  os.path.join(d, f'{b}_rss.csv'),
        'hot':  os.path.join(d, f'{b}_hot.csv'),
        'cold': os.path.join(d, f'{b}_cold.csv'),
    }


# ── Plot: RSS timeline ──────────────────────────────────────────────────────

def plot_rss_timeline(runs, labels, outdir):
    fig, ax = plt.subplots(figsize=(3.5, 2.5))

    for i, (run, label) in enumerate(zip(runs, labels)):
        data = load_csv(run['rss'])
        if data is None:
            print(f'  Warning: {run["rss"]} not found, skipping', file=sys.stderr)
            continue
        ax.plot(data['time_sec'], data['rss_mb'],
                label=label,
                color=COLORS[i % len(COLORS)],
                linestyle=LINESTYLES[i % len(LINESTYLES)],
                linewidth=1.5)

    ax.set_xlabel('Time (s)')
    ax.set_ylabel('RSS (MiB)')
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    ax.legend(loc='upper right', frameon=True, framealpha=0.9)

    try:
        sns.despine(left=False, bottom=False)
    except NameError:
        pass

    for fmt in ('pdf', 'png'):
        path = os.path.join(outdir, f'coldhotmix_rss.{fmt}')
        fig.savefig(path, bbox_inches='tight', pad_inches=0.05)
    plt.close(fig)
    print(f'  Wrote coldhotmix_rss.pdf/png')


# ── Plot: latency CDF ───────────────────────────────────────────────────────

def plot_latency_cdf(runs, labels, outdir, kind):
    """Plot CDF for 'hot' or 'cold' latencies."""
    fig, ax = plt.subplots(figsize=(3.5, 2.5))

    has_data = False
    for i, (run, label) in enumerate(zip(runs, labels)):
        data = load_csv(run[kind])
        if data is None:
            print(f'  Warning: {run[kind]} not found, skipping', file=sys.stderr)
            continue
        latencies = np.sort(data['latency_us'])
        if len(latencies) == 0:
            continue
        has_data = True
        cdf = np.arange(1, len(latencies) + 1) / len(latencies)
        ax.plot(latencies, cdf,
                label=label,
                color=COLORS[i % len(COLORS)],
                linestyle=LINESTYLES[i % len(LINESTYLES)],
                linewidth=1.2)

    if not has_data:
        plt.close(fig)
        return

    title_word = 'Hot' if kind == 'hot' else 'Cold'
    ax.set_xlabel(f'{title_word}-access latency ($\\mu$s)')
    ax.set_ylabel('CDF')
    ax.set_ylim(0, 1.02)
    ax.set_xlim(left=0)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    ax.legend(loc='lower right', frameon=True, framealpha=0.9)

    # Use log scale if the range spans more than 2 orders of magnitude
    all_max = []
    all_min = []
    for run in runs:
        data = load_csv(run[kind])
        if data is not None and len(data['latency_us']) > 0:
            arr = data['latency_us']
            all_max.append(np.max(arr))
            all_min.append(np.percentile(arr, 1))
    if all_max and all_min:
        lo = min(all_min)
        hi = max(all_max)
        if lo > 0 and hi / lo > 100:
            ax.set_xscale('log')
            ax.set_xlim(left=max(lo * 0.5, 0.001))

    try:
        sns.despine(left=False, bottom=False)
    except NameError:
        pass

    for fmt in ('pdf', 'png'):
        path = os.path.join(outdir, f'coldhotmix_{kind}_cdf.{fmt}')
        fig.savefig(path, bbox_inches='tight', pad_inches=0.05)
    plt.close(fig)
    print(f'  Wrote coldhotmix_{kind}_cdf.pdf/png')


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Plot bench_coldhotmix results (RSS timeline + latency CDFs).',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    parser.add_argument('prefixes', nargs='+', metavar='PREFIX',
                        help='Path prefix for CSV files (e.g. results/baseline)')
    parser.add_argument('-o', '--outdir', default='.',
                        help='Output directory for figures [default: .]')
    parser.add_argument('-l', '--label', action='append', dest='labels',
                        help='Display label for each prefix (repeat for each run)')

    args = parser.parse_args()

    runs = [resolve_paths(p) for p in args.prefixes]

    if args.labels and len(args.labels) >= len(runs):
        labels = args.labels[:len(runs)]
    else:
        labels = [os.path.basename(p) for p in args.prefixes]

    os.makedirs(args.outdir, exist_ok=True)

    print(f'Plotting {len(runs)} run(s): {", ".join(labels)}')
    plot_rss_timeline(runs, labels, args.outdir)
    plot_latency_cdf(runs, labels, args.outdir, 'hot')
    plot_latency_cdf(runs, labels, args.outdir, 'cold')
    print('Done.')


if __name__ == '__main__':
    main()
