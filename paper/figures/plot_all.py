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
    'font.family': 'serif',
    'font.serif': ['Times New Roman', 'DejaVu Serif'],
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
    apps = ['JSON', 'KV Store', 'SQLite', 'DuckDB', 'Memcached']
    smash_rss = [17.1, 33.2, 38.8, 30.0, 25.0]

    fig, ax = plt.subplots(figsize=(3.5, 2.2))
    x = np.arange(len(apps))
    bars = ax.bar(x, smash_rss, width=0.6, color=SMASH_COLOR, edgecolor='white',
                  linewidth=0.5, zorder=3)

    # Value labels on bars
    for bar, val in zip(bars, smash_rss):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.8,
                f'{val:.0f}%', ha='center', va='bottom', fontsize=8, fontweight='bold')

    ax.set_ylabel('RSS Reduction (%)')
    ax.set_xticks(x)
    ax.set_xticklabels(apps, rotation=15, ha='right')
    ax.set_ylim(0, 50)
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'rss_reduction.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'rss_reduction.png'))
    plt.close(fig)


def fig_algo_compare():
    """Figure 2: Compression ratio comparison across algorithms."""
    algos = ['WKdm', 'LZ4', 'zstd-1', 'zstd-3', 'zstd-9']
    data_types = ['JSON', 'KV', 'SQLite', 'Zeroed']

    # ratios[algo][data_type]
    ratios = {
        'WKdm':   [98.9, 93.7, 15.6, 6.8],
        'LZ4':    [30.2, 30.8, 5.8,  1.6],
        'zstd-1': [14.6, 23.2, 4.2,  1.1],
        'zstd-3': [15.6, 23.3, 4.1,  1.1],
        'zstd-9': [14.2, 21.0, 4.0,  1.0],
    }

    fig, ax = plt.subplots(figsize=(4.5, 2.8))
    x = np.arange(len(data_types))
    width = 0.15
    offsets = np.arange(len(algos)) - len(algos)/2 + 0.5

    algo_colors = [COLORS[7], COLORS[0], COLORS[1], COLORS[2], COLORS[3]]

    for i, algo in enumerate(algos):
        bars = ax.bar(x + offsets[i]*width, ratios[algo], width,
                      label=algo, color=algo_colors[i], edgecolor='white',
                      linewidth=0.3, zorder=3)

    ax.set_ylabel('Compression Ratio (%)\n(lower = better)')
    ax.set_xticks(x)
    ax.set_xticklabels(data_types)
    ax.set_yscale('log')
    ax.set_ylim(0.5, 200)
    ax.legend(ncol=3, loc='upper center', frameon=True, framealpha=0.9,
              bbox_to_anchor=(0.5, 1.15))
    ax.yaxis.grid(True, alpha=0.3)
    ax.set_axisbelow(True)
    sns.despine(left=False, bottom=False)

    fig.savefig(os.path.join(OUTDIR, 'algo_compare.pdf'))
    fig.savefig(os.path.join(OUTDIR, 'algo_compare.png'))
    plt.close(fig)


def fig_algo_throughput():
    """Figure 3: Compression and decompression throughput."""
    algos = ['WKdm', 'LZ4', 'zstd-3', 'zstd-9']

    comp_json   = [1865, 1004, 553, 90]
    decomp_json = [10077, 6064, 1498, 1437]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.5, 2.5), sharey=False)

    x = np.arange(len(algos))
    width = 0.35
    algo_colors = [COLORS[7], COLORS[0], COLORS[2], COLORS[3]]

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

    # Annotate key delta
    ax.annotate('25% lower', xy=(3 + width/2, 179.9), xytext=(3 + 0.8, 160),
                fontsize=7, fontweight='bold', color=SMASH_COLOR,
                arrowprops=dict(arrowstyle='->', color=SMASH_COLOR, lw=1.0))

    ax.set_ylabel('RSS (MiB)')
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
        'No dicts\n(T1d)',
        'No arenas\n(T1a)',
        'No adaptive\n(T1c)',
        'No zero-eager\n(T1b)',
        'No zero-defer\n(T2a)',
        'No zero-both\n(C1)',
        'No prefetch\n(T1e)',
        'Single worker\n(T1f)',
        'No compress\n(B2)',
    ]

    # Deltas from full baseline (B1) for each benchmark
    json_delta   = [+17.3, -1.5,  0.0,  0.0, +1.8, +2.2, +2.1, +1.3, -16.9]
    kv_delta     = [+7.3,  -0.2, +0.3, -0.3, +0.4, +0.5, -0.1, -0.3, -22.1]
    sqlite_delta = [+5.0,   0.0,  0.0,  0.0,  0.0,  0.0, +0.1, +0.1, -12.9]

    fig, ax = plt.subplots(figsize=(6.5, 3.5))
    x = np.arange(len(configs))
    width = 0.25

    ax.bar(x - width, json_delta, width, label='JSON',
           color=COLORS[0], edgecolor='white', linewidth=0.3, zorder=3)
    ax.bar(x, kv_delta, width, label='KV Store',
           color=COLORS[1], edgecolor='white', linewidth=0.3, zorder=3)
    ax.bar(x + width, sqlite_delta, width, label='SQLite',
           color=COLORS[2], edgecolor='white', linewidth=0.3, zorder=3)

    ax.axhline(y=0, color='black', linewidth=0.5, zorder=2)
    ax.set_ylabel('RSS Reduction Delta (pp)\nfrom full baseline')
    ax.set_xticks(x)
    ax.set_xticklabels(configs, fontsize=7, rotation=30, ha='right')
    ax.legend(frameon=True, framealpha=0.9, loc='lower left', fontsize=8)
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
        'JSON-LZ4':    [30.2, 27.7, 26.2],
        'JSON-zstd-9': [14.2, 12.4, 11.3],
        'KV-LZ4':      [30.8, 29.9, 29.3],
        'KV-zstd-9':   [21.0, 19.4, 18.7],
        'SQLite-LZ4':  [5.8,  5.3,  5.0],
        'SQLite-zstd-9': [4.0, 3.4, 3.0],
    }

    fig, ax = plt.subplots(figsize=(4.5, 2.8))
    x = np.arange(len(groups))

    markers = ['o', 's', 'D', '^', 'v', 'P']
    line_colors = [COLORS[0], COLORS[0], COLORS[1], COLORS[1], COLORS[2], COLORS[2]]
    line_styles = ['-', '--', '-', '--', '-', '--']

    for i, (label, vals) in enumerate(data.items()):
        ax.plot(x, vals, marker=markers[i], color=line_colors[i],
                linestyle=line_styles[i], linewidth=1.5, markersize=5,
                label=label, zorder=3)

    ax.set_ylabel('Compression Ratio (%)\n(lower = better)')
    ax.set_xticks(x)
    ax.set_xticklabels(groups)
    ax.set_xlabel('Pages per compression unit')
    ax.legend(ncol=2, fontsize=7, frameon=True, framealpha=0.9,
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

    # Combined legend
    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2,
               loc='upper left', fontsize=7, frameon=True, framealpha=0.9)

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
    print("Done.")
