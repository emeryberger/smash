#!/usr/bin/env python3
"""Generate all paper figures using matplotlib + seaborn."""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np
import os

# Style setup
sns.set_theme(style="whitegrid", context="paper", font_scale=1.2)
plt.rcParams.update({
    'figure.dpi': 300,
    'savefig.dpi': 300,
    'savefig.bbox': 'tight',
    'font.family': 'sans-serif',
    'font.sans-serif': ['Helvetica', 'Arial', 'DejaVu Sans'],
    'axes.titlesize': 11,
    'axes.labelsize': 10,
    'xtick.labelsize': 9,
    'ytick.labelsize': 9,
    'legend.fontsize': 8,
    'figure.figsize': (3.5, 2.5),  # single-column width
})

OUTDIR = os.path.dirname(os.path.abspath(__file__))

# Color palette - colorblind-friendly
COLORS = sns.color_palette("colorblind", 8)
SMASH_COLOR = COLORS[0]      # blue
BASELINE_COLOR = COLORS[7]   # gray
MESH_COLOR = COLORS[2]       # green
ACCENT_COLOR = COLORS[3]     # red


def fig_rss_reduction():
    """Figure 1: RSS reduction across applications (grouped bar chart)."""
    # Linux results from run_paper_experiments.py (March 2026)
    apps = ['Memcached', 'RocksDB', 'SQLite', 'Redis-ext', 'Redis']
    mesh_rss =  [0.0,  0.5, 23.0, 30.7, 28.7]
    smash_rss = [82.4, 79.5, 69.1, 23.0, 21.2]

    fig, ax = plt.subplots(figsize=(4.5, 2.5))
    x = np.arange(len(apps))
    width = 0.35

    bars_mesh = ax.bar(x - width/2, mesh_rss, width, label='Mesh',
                       color=MESH_COLOR, edgecolor='white', linewidth=0.5, zorder=3)
    bars_smash = ax.bar(x + width/2, smash_rss, width, label='Smash',
                        color=SMASH_COLOR, edgecolor='white', linewidth=0.5, zorder=3)

    # Value labels on Smash bars only (to avoid clutter)
    for bar, val in zip(bars_smash, smash_rss):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                f'{val:.0f}%', ha='center', va='bottom', fontsize=7, fontweight='bold')

    ax.set_ylabel('RSS Reduction (%)')
    ax.set_title('RSS Reduction: Mesh vs Smash')
    ax.set_xticks(x)
    ax.set_xticklabels(apps, rotation=25, ha='right')
    ax.set_ylim(0, 95)
    ax.legend(loc='upper right', fontsize=8)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'rss_reduction.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'rss_reduction.png'))
    plt.close(fig)


def fig_algo_compare():
    """Figure 2: Compression ratio comparison across algorithms.
    Compression ratio = original / compressed (higher = better)."""
    algos = ['LZ4', 'zstd-1', 'zstd-3', 'zstd-9']
    apps = ['SQLite', 'RocksDB', 'Memcached', 'Redis']

    # Compression ratio (original/compressed, ×) from real application heap page sampling
    ratios = {
        'LZ4':    [100/8.1,  100/11.6, 100/21.3, 100/15.9],
        'zstd-1': [100/4.9,  100/6.5,  100/11.4, 100/8.5],
        'zstd-3': [100/4.9,  100/6.4,  100/10.8, 100/8.4],
        'zstd-9': [100/4.8,  100/6.0,  100/10.8, 100/7.9],
    }

    fig, ax = plt.subplots(figsize=(4.5, 2.8))
    x = np.arange(len(apps))
    width = 0.18
    offsets = np.arange(len(algos)) - len(algos)/2 + 0.5

    algo_colors = [COLORS[0], COLORS[1], COLORS[2], COLORS[3]]

    for i, algo in enumerate(algos):
        ax.bar(x + offsets[i]*width, ratios[algo], width,
               label=algo, color=algo_colors[i], edgecolor='white',
               linewidth=0.3, zorder=3)

    ax.set_ylabel('Compression Ratio\n(higher = better)')
    ax.set_xticks(x)
    ax.set_xticklabels(apps)
    ax.set_ylim(0, 22)
    ax.legend(ncol=5, loc='upper center', frameon=True, framealpha=0.9,
              bbox_to_anchor=(0.5, 1.12), fontsize=8)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'algo_compare.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'algo_compare.png'))
    plt.close(fig)


def fig_algo_throughput():
    """Figure 3: Compression and decompression throughput."""
    algos = ['LZ4', 'zstd-3', 'zstd-9']

    comp_json   = [1004, 553, 90]
    decomp_json = [6064, 1498, 1437]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.5, 2.5), sharey=False)

    x = np.arange(len(algos))
    width = 0.35
    algo_colors = [COLORS[0], COLORS[2], COLORS[3]]

    # Compression
    bars1 = ax1.bar(x, comp_json, width=0.6, color=algo_colors, edgecolor='white',
                    linewidth=0.5, zorder=3)
    ax1.set_ylabel('Throughput (MB/s)')
    ax1.set_title('Compression', fontsize=10)
    ax1.set_xticks(x)
    ax1.set_xticklabels(algos)
    ax1.yaxis.grid(True, alpha=0.3)
    ax1.set_axisbelow(True)

    # Decompression
    bars2 = ax2.bar(x, decomp_json, width=0.6, color=algo_colors, edgecolor='white',
                    linewidth=0.5, zorder=3)
    ax2.set_ylabel('Throughput (MB/s)')
    ax2.set_title('Decompression', fontsize=10)
    ax2.set_xticks(x)
    ax2.set_xticklabels(algos)
    ax2.yaxis.grid(True, alpha=0.3)
    ax2.set_axisbelow(True)

    for ax in (ax1, ax2):
        sns.despine(ax=ax, left=False, bottom=False)

    fig.suptitle('JSON Data (16 KiB pages)', fontsize=10, y=1.02)
    fig.tight_layout()

    fig.savefig(os.path.join(OUTDIR, 'algo_throughput.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'algo_throughput.png'))
    plt.close(fig)


def fig_memcached():
    """Figure 4: Memcached RSS over time (conceptual phases)."""
    phases = ['Fill', 'Peak', 'Post-cool', 'Serve', 'Min', 'Cold\nre-access']

    baseline = [233.1, 240.0, 240.0, 240.0, 233.1, 240.0]
    smash    = [259.8, 262.5, 238.6, 179.9, 190.3, 201.1]

    fig, ax = plt.subplots(figsize=(4.5, 2.5))
    x = np.arange(len(phases))
    width = 0.35

    ax.bar(x - width/2, baseline, width, label='System malloc',
           color=BASELINE_COLOR, edgecolor='white', linewidth=0.5, zorder=3)
    ax.bar(x + width/2, smash, width, label='Smash',
           color=SMASH_COLOR, edgecolor='white', linewidth=0.5, zorder=3)

    ax.set_ylabel('RSS (MiB)')
    ax.set_title('Memcached RSS by Phase')
    ax.set_xticks(x)
    ax.set_xticklabels(phases, fontsize=8)
    ax.legend(frameon=True, framealpha=0.9, loc='upper left', fontsize=8)
    ax.set_ylim(0, 310)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'memcached.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'memcached.png'))
    plt.close(fig)


def fig_ablation():
    """Figure 5: Ablation study - RSS reduction deltas from baseline."""
    configs = [
        'With dicts',
        'No arenas',
        'No adaptive\n(zstd-1 only)',
        'No zero-defer',
        'No prefetch',
        'Single worker',
        'No compress',
    ]

    # Deltas (pp) from default for each benchmark - Linux results (March 2026)
    sqlite_delta    = [-0.2,   0.0,   0.0,   0.0,   0.0,   0.0,  -47.8]
    rocksdb_delta   = [-1.2,  -1.5,   0.0,  -1.1,  -0.3,   0.1,  -79.0]
    memcached_delta = [-0.4,  -0.2,  -0.4,  -0.1,  -0.6,  -0.4,  -82.4]
    redis_delta     = [-0.4,  -2.5,   0.0,   0.6,   0.8,  -1.0,   0.4]

    fig, ax = plt.subplots(figsize=(7.0, 4.5))
    x = np.arange(len(configs))
    width = 0.18

    ax.bar(x - 1.5*width, sqlite_delta, width, label='SQLite',
           color=COLORS[0], edgecolor='white', linewidth=0.3, zorder=3)
    ax.bar(x - 0.5*width, rocksdb_delta, width, label='RocksDB',
           color=COLORS[1], edgecolor='white', linewidth=0.3, zorder=3)
    ax.bar(x + 0.5*width, memcached_delta, width, label='Memcached',
           color=COLORS[3], edgecolor='white', linewidth=0.3, zorder=3)
    ax.bar(x + 1.5*width, redis_delta, width, label='Redis',
           color=COLORS[4], edgecolor='white', linewidth=0.3, zorder=3)

    ax.axhline(y=0, color='black', linewidth=0.5, zorder=2)
    ax.set_ylabel('Change in RSS Reduction (pp)\nfrom default', fontsize=16)
    ax.set_xticks(x)
    ax.set_xticklabels(configs, fontsize=14, rotation=30, ha='right')
    ax.tick_params(axis='y', labelsize=14)
    ax.legend(frameon=True, framealpha=0.9, ncol=4, fontsize=14,
              loc='lower left')
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'ablation.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'ablation.png'))
    plt.close(fig)


def fig_multipage():
    """Figure 6: Multi-page compression ratios (original/compressed)."""
    groups = ['1x16K', '2x16K', '4x16K']

    data = {
        'SQLite-LZ4':    [100/5.8,  100/5.3,  100/5.0],
        'SQLite-zstd-9': [100/4.0,  100/3.4,  100/3.0],
    }

    fig, ax = plt.subplots(figsize=(4.5, 2.8))
    x = np.arange(len(groups))

    markers = ['o', 's']
    line_colors = [COLORS[0], COLORS[3]]
    line_styles = ['-', '--']

    for i, (label, vals) in enumerate(data.items()):
        ax.plot(x, vals, marker=markers[i], color=line_colors[i],
                linestyle=line_styles[i], linewidth=1.5, markersize=5,
                label=label, zorder=3)

    ax.set_ylabel('Compression Ratio\n(higher = better)')
    ax.set_title('Multi-Page Compression')
    ax.set_xticks(x)
    ax.set_xticklabels(groups)
    ax.set_xlabel('Pages per compression unit')
    ax.legend(ncol=2, fontsize=8, frameon=True, framealpha=0.9,
              loc='lower right')
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'multipage.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'multipage.png'))
    plt.close(fig)


def fig_state_machine():
    """Figure 7: Page state machine (text-based, skip for now - in LaTeX)."""
    pass


def fig_allocator_compare():
    """Figure: Compression ratios (original/compressed) across allocator substrates.
    Data from bench_allocator_compare.cpp with real application workloads, zstd-9.
    Single graph; dagger marks allocators that zero freed memory."""
    # Allocators in order; dagger for those that zero on free
    allocs = ['System\u2020', 'mimalloc', 'jemalloc', 'tcmalloc',
              'Hoard', 'Mesh', 'DieHard', 'DieHarder\u2020', 'Smash\u2020']

    apps = ['SQLite', 'Memcached', 'Redis']

    # Real measured data: compression ratio = 1/ratio (original/compressed)
    # SQLite: 50K rows, no frees; others: 100K objects, 256B, 50% freed
    # System zeroes on macOS; DieHarder zeroes for security; Smash defers to compress time
    ratios = {
        'SQLite':    [1/0.041, 1/0.041, 1/0.043, 1/0.043, 1/0.029, 1/0.041, 1/0.025, 1/0.024, 1/0.025],
        'Memcached': [1/0.054, 1/0.074, 1/0.071, 1/0.074, 1/0.074, 1/0.055, 1/0.086, 1/0.051, 1/0.071],
        'Redis':     [1/0.030, 1/0.030, 1/0.024, 1/0.030, 1/0.030, 1/0.030, 1/0.032, 1/0.024, 1/0.024],
    }

    fig, ax = plt.subplots(1, 1, figsize=(7.0, 3.5))

    app_colors = [COLORS[0], COLORS[1], COLORS[2]]
    width = 0.22

    x = np.arange(len(allocs))
    offsets = np.arange(len(apps)) - len(apps)/2 + 0.5
    for i, app in enumerate(apps):
        ax.bar(x + offsets[i]*width, ratios[app], width,
               label=app, color=app_colors[i], edgecolor='white',
               linewidth=0.3, zorder=3)

    ax.set_ylabel('Compression Ratio (zstd-9)\n(higher = better)', fontsize=10)
    ax.set_xticks(x)
    ax.set_xticklabels(allocs, fontsize=9, rotation=25, ha='right')
    ax.set_yscale('log')
    ax.set_ylim(1, 65)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    ax.tick_params(axis='y', labelsize=9)

    ax.legend(apps, ncol=3, loc='upper center', frameon=True,
              framealpha=0.9, fontsize=9, bbox_to_anchor=(0.5, 1.12))

    sns.despine(ax=ax, left=False, bottom=False)

    fig.tight_layout()
    fig.savefig(os.path.join(OUTDIR, 'allocator_compare.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'allocator_compare.png'))
    plt.close(fig)


def fig_dict_overhead():
    """Figure 8: Dictionary CDict memory overhead vs compression benefit."""
    levels = [1, 3, 9, 15]
    cdict_kb = [78, 430, 686, 1070]
    json_improvement = [6.8, 8.1, -0.5, -3.0]  # % improvement from dict

    fig, ax1 = plt.subplots(figsize=(3.5, 2.5))
    ax2 = ax1.twinx()

    color1 = COLORS[3]
    color2 = COLORS[0]

    bars = ax1.bar(np.arange(len(levels)), cdict_kb, 0.4, color=color1,
                   alpha=0.7, edgecolor='white', linewidth=0.5, zorder=3,
                   label='CDict size')
    ax1.set_ylabel('CDict Size (KiB)', color=color1)
    ax1.tick_params(axis='y', labelcolor=color1)

    line = ax2.plot(np.arange(len(levels)), json_improvement, 'o-',
                    color=color2, linewidth=2, markersize=6, zorder=4,
                    label='Compression gain')
    ax2.axhline(y=0, color='gray', linewidth=0.5, linestyle='--', zorder=2)
    ax2.set_ylabel('Compression Improvement (%)\n(JSON data)', color=color2)
    ax2.tick_params(axis='y', labelcolor=color2)

    ax1.set_xticks(np.arange(len(levels)))
    ax1.set_xticklabels([f'Level {l}' for l in levels])
    ax1.set_xlabel('zstd Compression Level')

    # Combined legend above the plot
    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2, ncol=2,
               loc='upper center', fontsize=8, frameon=True, framealpha=0.9,
               bbox_to_anchor=(0.5, 1.18))

    ax1.yaxis.grid(True, alpha=0.3)
    ax1.set_axisbelow(True)

    fig.savefig(os.path.join(OUTDIR, 'dict_overhead.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'dict_overhead.png'))
    plt.close(fig)


def fig_auc_comparison():
    """Figure: AUC (Area Under Curve) reduction - total memory pressure over time."""
    # Linux results from run_paper_experiments.py (March 2026)
    apps = ['Memcached', 'RocksDB', 'SQLite', 'Redis-ext', 'Redis']

    # AUC reduction % from baseline (positive = better)
    mesh_auc_red =  [-0.9, -9.5, -91.2, -0.5, 0.3]
    smash_auc_red = [71.9, 59.8, 33.9, -40.0, -36.7]

    fig, ax = plt.subplots(figsize=(4.5, 2.5))
    x = np.arange(len(apps))
    width = 0.35

    ax.bar(x - width/2, mesh_auc_red, width, label='Mesh',
           color=MESH_COLOR, edgecolor='white', linewidth=0.5, zorder=3)
    ax.bar(x + width/2, smash_auc_red, width, label='Smash',
           color=SMASH_COLOR, edgecolor='white', linewidth=0.5, zorder=3)

    ax.axhline(y=0, color='black', linewidth=0.5, zorder=2)
    ax.set_ylabel('AUC Reduction (%)\n(higher = better)')
    ax.set_title('Memory Pressure Over Time (AUC)')
    ax.set_xticks(x)
    ax.set_xticklabels(apps, rotation=25, ha='right')
    ax.set_ylim(-100, 85)
    ax.legend(loc='upper right', fontsize=8)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'auc_comparison.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'auc_comparison.png'))
    plt.close(fig)


if __name__ == '__main__':
    print("Generating figures...")
    fig_rss_reduction()
    print("  rss_reduction.pdf")
    fig_algo_compare()
    print("  algo_compare.pdf")
    fig_algo_throughput()
    print("  algo_throughput.pdf")
    fig_memcached()
    print("  memcached.pdf")
    fig_ablation()
    print("  ablation.pdf")
    fig_multipage()
    print("  multipage.pdf")
    fig_dict_overhead()
    print("  dict_overhead.pdf")
    fig_allocator_compare()
    print("  allocator_compare.pdf")
    fig_auc_comparison()
    print("  auc_comparison.pdf")
    print("Done.")
