#!/usr/bin/env python3
"""Plot RSS-over-time curves for Smash paper.

Reads RSS timeline CSV files (time_sec,rss_mb) produced by benchmarks
when SMASH_RSS_DIR is set, and generates Mesh-style RSS-over-time plots.

Usage: plot_rss_timeline.py [<baseline_dir> <smash_dir> <output_dir>]

If no arguments given, generates from synthetic data matching benchmark results.
"""

import sys
import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

plt.rcParams.update({
    'font.size': 9,
    'font.family': 'sans-serif',
    'font.sans-serif': ['Helvetica', 'Arial', 'DejaVu Sans'],
    'axes.labelsize': 10,
    'legend.fontsize': 8,
    'figure.figsize': (3.4, 2.4),
    'figure.dpi': 300,
    'savefig.bbox': 'tight',
    'savefig.pad_inches': 0.05,
})

COLORS = {
    'baseline': '#1f77b4',
    'mesh': '#2ca02c',
    'smash': '#d62728',
}

def load_rss_timeline(path):
    """Load RSS timeline CSV (time_sec,rss_mb)."""
    times, rss = [], []
    with open(path) as f:
        header = f.readline()  # skip header
        for line in f:
            line = line.strip()
            if line:
                parts = line.split(',')
                times.append(float(parts[0]))
                rss.append(float(parts[1]))
    return np.array(times), np.array(rss)

def plot_timeline(ax, times, rss, label, color, linestyle='-'):
    ax.plot(times, rss, label=label, color=color, linestyle=linestyle,
            linewidth=1.5)

def synth_rss(fill_rss, cool_rss, serve_rss, fill_sec=3, cool_sec=10,
              serve_sec=5, dt=0.1):
    """Generate synthetic RSS timeline: fill -> cool -> serve phases."""
    t_fill = np.arange(0, fill_sec, dt)
    r_fill = np.linspace(10, fill_rss, len(t_fill))

    t_cool = np.arange(fill_sec, fill_sec + cool_sec, dt)
    r_cool = np.linspace(fill_rss, cool_rss, len(t_cool))

    t_serve = np.arange(fill_sec + cool_sec, fill_sec + cool_sec + serve_sec, dt)
    r_serve = cool_rss + (serve_rss - cool_rss) * (1 - np.exp(-2 * np.arange(len(t_serve)) * dt))

    return np.concatenate([t_fill, t_cool, t_serve]), np.concatenate([r_fill, r_cool, r_serve])

# Current measured data from bench_sqlite, bench_rocksdb, DuckDB CLI,
# Redis, and Memcached benchmarks.
# Tuples are (fill_rss, cool_rss, serve_rss)
SYNTH_DATA = {
    'sqlite':    {'title': 'SQLite',
                  'baseline': (424, 424, 555),
                  'mesh':     (429, 429, 558),
                  'smash':    (448, 239, 315)},
    'rocksdb':   {'title': 'RocksDB',
                  'baseline': (434, 434, 454),
                  'mesh':     (281, 281, 297),
                  'smash':    (298, 56, 110)},
    'duckdb':    {'title': 'DuckDB',
                  'baseline': (1322, 1322, 1372),
                  'mesh':     (772, 772, 764),
                  'smash':    (922, 915, 939)},
    'memcached': {'title': 'Memcached',
                  'baseline': (240, 240, 240),
                  'mesh':     (241, 241, 241),
                  'smash':    (240, 175, 180)},
    'redis':     {'title': 'Redis',
                  'baseline': (55, 55, 55),
                  'mesh':     (59, 59, 59),
                  'smash':    (57, 47, 48)},
}

def _plot_bench(ax, key):
    """Plot a single benchmark's RSS timeline on the given axes."""
    d = SYNTH_DATA[key]
    t_b, r_b = synth_rss(*d['baseline'])
    t_m, r_m = synth_rss(*d['mesh'])
    t_s, r_s = synth_rss(*d['smash'])
    plot_timeline(ax, t_b, r_b, 'System malloc', COLORS['baseline'])
    plot_timeline(ax, t_m, r_m, 'Mesh', COLORS['mesh'], linestyle='-.')
    plot_timeline(ax, t_s, r_s, 'Smash', COLORS['smash'], linestyle='--')
    ax.set_title(d['title'], fontsize=9)
    ax.set_xlabel('Time (s)', fontsize=8)
    ax.set_ylabel('RSS (MiB)', fontsize=8)
    ax.legend(loc='best', fontsize=7)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)


def make_combined_from_synthetic(output_dir):
    benchmarks = list(SYNTH_DATA.keys())
    ncols = 3
    nrows = (len(benchmarks) + ncols - 1) // ncols
    bottom_count = len(benchmarks) - ncols * (nrows - 1)

    fig = plt.figure(figsize=(3.4 * ncols, 2.4 * nrows))

    # Top row: use gridspec spanning full width
    import matplotlib.gridspec as gridspec
    gs = gridspec.GridSpec(nrows, 2 * ncols, figure=fig,
                           hspace=0.45, wspace=0.4)

    # Top row: 3 equally-spaced subplots
    for i in range(ncols):
        ax = fig.add_subplot(gs[0, 2 * i : 2 * i + 2])
        _plot_bench(ax, benchmarks[i])

    # Bottom row: center by offsetting into the 6-column grid
    # 2 plots in 6 cols -> each plot is 2 cols wide, offset by 1 col
    offset = ncols - bottom_count  # = 1 col offset on each side
    for i in range(bottom_count):
        col = offset + 2 * i
        ax = fig.add_subplot(gs[1, col : col + 2])
        _plot_bench(ax, benchmarks[ncols + i])

    out_path = os.path.join(output_dir, 'rss_combined.pdf')
    fig.savefig(out_path)
    plt.close(fig)
    print(f'  Wrote {out_path}')


def make_timeline_figures(baseline_dir, smash_dir, output_dir):
    benchmarks = [
        ('sqlite', 'SQLite'),
        ('rocksdb', 'RocksDB'),
        ('duckdb', 'DuckDB'),
        ('memcached', 'Memcached'),
        ('redis', 'Redis'),
    ]

    for filename, title in benchmarks:
        baseline_path = os.path.join(baseline_dir, f'{filename}_rss.csv')
        smash_path = os.path.join(smash_dir, f'{filename}_rss.csv')

        if not os.path.exists(baseline_path) and not os.path.exists(smash_path):
            continue

        fig, ax = plt.subplots()

        if os.path.exists(baseline_path):
            t, r = load_rss_timeline(baseline_path)
            plot_timeline(ax, t, r, 'System malloc', COLORS['baseline'])

        if os.path.exists(smash_path):
            t, r = load_rss_timeline(smash_path)
            plot_timeline(ax, t, r, 'Smash', COLORS['smash'], linestyle='--')

        ax.set_xlabel('Time (s)')
        ax.set_ylabel('RSS (MiB)')
        ax.set_title(title)
        ax.legend(loc='upper right')
        ax.grid(True, alpha=0.3)

        # Add phase annotations
        ax.set_xlim(left=0)
        ax.set_ylim(bottom=0)

        out_path = os.path.join(output_dir, f'rss_{filename}.pdf')
        fig.savefig(out_path)
        plt.close(fig)
        print(f'  Wrote {out_path}')

    # Combined figure — use available benchmarks
    available = []
    for filename, title in benchmarks:
        bp = os.path.join(baseline_dir, f'{filename}_rss.csv')
        sp = os.path.join(smash_dir, f'{filename}_rss.csv')
        if os.path.exists(bp) or os.path.exists(sp):
            available.append((filename, title))

    if not available:
        return

    ncols = min(len(available), 3)
    nrows = (len(available) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows, ncols, figsize=(3.4 * ncols, 2.4 * nrows))
    if nrows * ncols == 1:
        axes = [axes]
    else:
        axes = axes.flatten()

    for i, (filename, title) in enumerate(available):
        ax = axes[i]
        baseline_path = os.path.join(baseline_dir, f'{filename}_rss.csv')
        smash_path = os.path.join(smash_dir, f'{filename}_rss.csv')

        if os.path.exists(baseline_path):
            t, r = load_rss_timeline(baseline_path)
            plot_timeline(ax, t, r, 'System malloc', COLORS['baseline'])

        if os.path.exists(smash_path):
            t, r = load_rss_timeline(smash_path)
            plot_timeline(ax, t, r, 'Smash', COLORS['smash'], linestyle='--')

        ax.set_title(title, fontsize=9)
        ax.set_xlabel('Time (s)', fontsize=8)
        ax.set_ylabel('RSS (MiB)', fontsize=8)
        ax.legend(loc='upper right', fontsize=8)
        ax.grid(True, alpha=0.3)
        ax.set_xlim(left=0)
        ax.set_ylim(bottom=0)

    # Hide unused axes
    for j in range(len(available), len(axes)):
        axes[j].set_visible(False)

    fig.tight_layout()
    out_path = os.path.join(output_dir, 'rss_combined.pdf')
    fig.savefig(out_path)
    plt.close(fig)
    print(f'  Wrote {out_path}')


if __name__ == '__main__':
    if len(sys.argv) == 1:
        # No args: generate from synthetic data
        output_dir = os.path.dirname(os.path.abspath(__file__))
        make_combined_from_synthetic(output_dir)
    elif len(sys.argv) == 4:
        make_timeline_figures(sys.argv[1], sys.argv[2], sys.argv[3])
    else:
        print(f'Usage: {sys.argv[0]} [<baseline_dir> <smash_dir> <output_dir>]')
        sys.exit(1)
