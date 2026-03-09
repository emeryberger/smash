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
ACCENT_COLOR = COLORS[3]     # red


def fig_rss_reduction():
    """Figure 1: RSS reduction across applications (grouped bar chart)."""
    apps = ['RocksDB', 'SQLite', 'DuckDB', 'Memcached', 'Redis']
    smash_rss = [80.0, 43.8, 31.6, 25.0, 17.0]

    fig, ax = plt.subplots(figsize=(3.5, 2.2))
    x = np.arange(len(apps))
    bars = ax.bar(x, smash_rss, width=0.6, color=SMASH_COLOR, edgecolor='white',
                  linewidth=0.5, zorder=3)

    # Value labels on bars
    for bar, val in zip(bars, smash_rss):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.8,
                f'{val:.0f}%', ha='center', va='bottom', fontsize=8, fontweight='bold')

    ax.set_ylabel('RSS Reduction (%)')
    ax.set_title('RSS Reduction by Application')
    ax.set_xticks(x)
    ax.set_xticklabels(apps, rotation=25, ha='right')
    ax.set_ylim(0, 95)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'rss_reduction.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'rss_reduction.png'))
    plt.close(fig)


def fig_algo_compare():
    """Figure 2: Compression ratio comparison across algorithms."""
    algos = ['LZ4', 'zstd-1', 'zstd-3', 'zstd-9']
    apps = ['SQLite', 'RocksDB', 'Memcached', 'Redis', 'DuckDB']

    # Average compression ratio (%) from real application heap page sampling
    ratios = {
        'LZ4':    [8.1,  11.6, 21.3, 15.9, 48.6],
        'zstd-1': [4.9,  6.5,  11.4, 8.5,  35.7],
        'zstd-3': [4.9,  6.4,  10.8, 8.4,  34.9],
        'zstd-9': [4.8,  6.0,  10.8, 7.9,  33.4],
    }

    fig, ax = plt.subplots(figsize=(4.5, 2.8))
    x = np.arange(len(apps))
    width = 0.18
    offsets = np.arange(len(algos)) - len(algos)/2 + 0.5

    algo_colors = [COLORS[0], COLORS[1], COLORS[2], COLORS[3]]

    for i, algo in enumerate(algos):
        bars = ax.bar(x + offsets[i]*width, ratios[algo], width,
                      label=algo, color=algo_colors[i], edgecolor='white',
                      linewidth=0.3, zorder=3)

    ax.set_ylabel('Compression Ratio (%)\n(lower = better)')
    ax.set_xticks(x)
    ax.set_xticklabels(apps)
    ax.set_yscale('log')
    ax.set_ylim(0.5, 200)
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
        'No adaptive\n(LZ4 only)',
        'No zero-defer',
        'No prefetch',
        'Single worker',
        'No compress',
    ]

    # Deltas (pp) from default for each benchmark
    sqlite_delta    = [-5.1,   0.0,   0.0,   0.0,   0.0,   0.0,  -20.3]
    rocksdb_delta   = [-7.7,  -0.6,  -1.2,  -1.5,  -1.1,  -0.9,  -80.3]
    memcached_delta = [-24.5, -0.1,   0.0,   0.0,   0.0,  +0.2,  -40.0]
    redis_delta     = [-30.2, -0.2,  -0.1,  -0.2,  +0.1,  +0.1,  -42.3]

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
    """Figure 6: Multi-page compression ratios."""
    groups = ['1x16K', '2x16K', '4x16K']

    data = {
        'SQLite-LZ4':    [5.8,  5.3,  5.0],
        'SQLite-zstd-9': [4.0,  3.4,  3.0],
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

    ax.set_ylabel('Compression Ratio (%)\n(lower = better)')
    ax.set_title('Multi-Page Compression')
    ax.set_xticks(x)
    ax.set_xticklabels(groups)
    ax.set_xlabel('Pages per compression unit')
    ax.legend(ncol=2, fontsize=8, frameon=True, framealpha=0.9,
              loc='upper right')
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
    """Figure: Page compression ratios across allocator substrates.
    Data from bench_allocator_compare.cpp, avg across sizes 64-512B, zstd-9.
    Two panels: base allocators (left) and +zero variants (right)."""
    # Panel 1: Base allocators
    base_allocs = ['System', 'mimalloc', 'jemalloc', 'tcmalloc', 'Hoard', 'Mesh', 'DieHard']
    # Panel 2: With zeroing  (+DieHarder which has built-in zeroing, +Smash)
    zero_allocs = ['System+z', 'mimalloc+z', 'jemalloc+z', 'tcmalloc+z',
                   'Mesh+z', 'DieHard+z', 'DieHarder', 'Smash']

    data_types = ['JSON', 'KV', 'Struct', 'Mixed']

    # Real measured data: avg zstd-9 ratio across obj sizes 64/128/256/512
    base_ratios = {
        'JSON':   [0.039, 0.039, 0.028, 0.038, 0.040, 0.039, 0.049],
        'KV':     [0.096, 0.113, 0.120, 0.113, 0.115, 0.097, 0.123],
        'Struct': [0.082, 0.108, 0.118, 0.109, 0.109, 0.082, 0.110],
        'Mixed':  [0.077, 0.121, 0.120, 0.121, 0.122, 0.096, 0.122],
    }
    zero_ratios = {
        'JSON':   [0.039, 0.028, 0.020, 0.028, 0.039, 0.031, 0.031, 0.026],
        'KV':     [0.096, 0.085, 0.083, 0.085, 0.096, 0.077, 0.077, 0.092],
        'Struct': [0.082, 0.071, 0.064, 0.071, 0.082, 0.061, 0.061, 0.075],
        'Mixed':  [0.075, 0.085, 0.079, 0.085, 0.076, 0.076, 0.076, 0.090],
    }

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 3.5),
                                    gridspec_kw={'width_ratios': [7, 8]})

    dt_colors = [COLORS[0], COLORS[1], COLORS[2], COLORS[3]]
    width = 0.18

    # Panel 1: Base allocators
    x1 = np.arange(len(base_allocs))
    offsets = np.arange(len(data_types)) - len(data_types)/2 + 0.5
    for i, dt in enumerate(data_types):
        ax1.bar(x1 + offsets[i]*width, base_ratios[dt], width,
                label=dt, color=dt_colors[i], edgecolor='white',
                linewidth=0.3, zorder=3)
    ax1.set_ylabel('Compression Ratio (zstd-9)\n(lower = better)', fontsize=10)
    ax1.set_xticks(x1)
    ax1.set_xticklabels(base_allocs, fontsize=9, rotation=25, ha='right')
    ax1.set_ylim(0, 0.15)
    ax1.set_title('Base allocators', fontsize=11)
    ax1.yaxis.grid(True, alpha=0.3)
    ax1.set_axisbelow(True)
    ax1.tick_params(axis='y', labelsize=9)

    # Panel 2: +zero variants
    x2 = np.arange(len(zero_allocs))
    for i, dt in enumerate(data_types):
        ax2.bar(x2 + offsets[i]*width, zero_ratios[dt], width,
                color=dt_colors[i], edgecolor='white',
                linewidth=0.3, zorder=3)
    ax2.set_xticks(x2)
    ax2.set_xticklabels(zero_allocs, fontsize=9, rotation=25, ha='right')
    ax2.set_ylim(0, 0.15)
    ax2.set_title('With zero-on-free', fontsize=11)
    ax2.yaxis.grid(True, alpha=0.3)
    ax2.set_axisbelow(True)
    ax2.tick_params(axis='y', labelsize=9)

    # Shared legend
    fig.legend(data_types, ncol=4, loc='upper center', frameon=True,
               framealpha=0.9, bbox_to_anchor=(0.5, 1.05), fontsize=9)

    fig.suptitle('Page Compressibility Across Allocators', fontsize=12, y=1.10)

    for ax in (ax1, ax2):
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
    print("Done.")
