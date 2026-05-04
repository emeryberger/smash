#!/usr/bin/env python3
"""Generate publication-quality heatmap for Smash coldness × compressibility experiment.

Produces a LaTeX-ready PDF/PNG with Sanborn-inspired thermal colormap.
"""

import argparse
import json
import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns
from pathlib import Path

# Set seaborn style for clean, modern aesthetics
sns.set_theme(style="whitegrid", context="paper", font_scale=1.1)

# Use sans-serif fonts for modern look
plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['Helvetica', 'Arial', 'DejaVu Sans', 'Liberation Sans'],
    'font.size': 10,
    'axes.labelsize': 11,
    'axes.titlesize': 12,
    'xtick.labelsize': 10,
    'ytick.labelsize': 10,
    'text.usetex': False,
    'figure.dpi': 300,
    'savefig.dpi': 300,
    'savefig.bbox': 'tight',
    'savefig.pad_inches': 0.05,
})


def load_results(json_path):
    """Load heatmap results and compute benefit matrix."""
    with open(json_path) as f:
        data = json.load(f)

    # Extract hot fractions and compressibilities from data
    hot_pcts = sorted(set(r['hot_pct'] for r in data['baseline']))
    compress_pcts = sorted(set(r['compress_pct'] for r in data['baseline']))

    # Build lookup dicts
    baseline = {(r['hot_pct'], r['compress_pct']): r for r in data['baseline']}
    smash = {(r['hot_pct'], r['compress_pct']): r for r in data['smash']}

    # Compute benefit matrix (rows = hot%, cols = compress%)
    benefit = np.zeros((len(hot_pcts), len(compress_pcts)))
    for i, hot in enumerate(hot_pcts):
        for j, comp in enumerate(compress_pcts):
            base_rss = baseline[(hot, comp)]['steady_rss_mb']
            smash_rss = smash[(hot, comp)]['steady_rss_mb']
            if base_rss > 0:
                benefit[i, j] = 100 * (base_rss - smash_rss) / base_rss

    return benefit, hot_pcts, compress_pcts


def create_heatmap(benefit, hot_pcts, compress_pcts, output_path, title=None):
    """Create publication-quality heatmap using seaborn."""

    # Figure size for single-column (3.5") or double-column (7") LaTeX
    fig, ax = plt.subplots(figsize=(5, 4))

    # Create custom diverging colormap centered at zero
    # Blue for overhead (negative), white at zero, red for benefit (positive)
    vmin = min(-10, benefit.min())
    vmax = benefit.max()
    center = 0

    # Format annotations with percentage signs
    annot_data = np.empty_like(benefit, dtype=object)
    for i in range(benefit.shape[0]):
        for j in range(benefit.shape[1]):
            val = benefit[i, j]
            if val >= 0:
                annot_data[i, j] = f'+{val:.0f}%' if val >= 10 else f'+{val:.1f}%'
            else:
                annot_data[i, j] = f'{val:.0f}%' if val <= -10 else f'{val:.1f}%'

    # Use seaborn heatmap with diverging colormap
    sns.heatmap(
        benefit,
        ax=ax,
        cmap='RdBu_r',
        center=center,
        vmin=vmin,
        vmax=vmax,
        annot=annot_data,
        fmt='',
        annot_kws={'size': 9, 'weight': 'medium'},
        linewidths=0.5,
        linecolor='white',
        cbar_kws={'label': 'RSS Reduction (%)', 'shrink': 0.85},
        xticklabels=[f'{c}%' for c in compress_pcts],
        yticklabels=[f'{h}%' for h in hot_pcts],
    )

    ax.set_xlabel('Data Compressibility', fontweight='medium')
    ax.set_ylabel('Hot Data Fraction', fontweight='medium')

    if title:
        ax.set_title(title, fontweight='bold', pad=10)

    plt.tight_layout()

    # Save in multiple formats
    output_path = Path(output_path)
    fig.savefig(output_path.with_suffix('.pdf'), format='pdf')
    fig.savefig(output_path.with_suffix('.png'), format='png', dpi=300)
    print(f"Saved: {output_path.with_suffix('.pdf')}")
    print(f"Saved: {output_path.with_suffix('.png')}")

    plt.close()


def create_heatmap_v2(benefit, hot_pcts, compress_pcts, output_path, title=None):
    """Alternative version with seaborn styling and coolwarm palette."""

    fig, ax = plt.subplots(figsize=(5, 4))

    # Format annotations with percentage signs
    annot_data = np.empty_like(benefit, dtype=object)
    for i in range(benefit.shape[0]):
        for j in range(benefit.shape[1]):
            val = benefit[i, j]
            if val >= 0:
                annot_data[i, j] = f'+{val:.0f}' if val >= 10 else f'+{val:.1f}'
            else:
                annot_data[i, j] = f'{val:.0f}' if val <= -10 else f'{val:.1f}'

    # Normalize with zero centered
    vmin = min(-15, benefit.min())
    vmax = benefit.max()

    # Use seaborn heatmap with coolwarm palette for a warmer Sanborn-like feel
    sns.heatmap(
        benefit,
        ax=ax,
        cmap='coolwarm',
        center=0,
        vmin=vmin,
        vmax=vmax,
        annot=annot_data,
        fmt='',
        annot_kws={'size': 10, 'weight': 'bold'},
        linewidths=1,
        linecolor='white',
        cbar_kws={'label': 'RSS Reduction (%)', 'shrink': 0.9},
        xticklabels=[f'{c}%' for c in compress_pcts],
        yticklabels=[f'{h}%' for h in hot_pcts],
        square=False,
    )

    ax.set_xlabel('Data Compressibility', fontsize=11, fontweight='medium')
    ax.set_ylabel('Hot Data Fraction', fontsize=11, fontweight='medium')

    if title:
        ax.set_title(title, fontsize=12, fontweight='bold', pad=12)

    # Clean up spines
    for spine in ax.spines.values():
        spine.set_linewidth(0.5)
        spine.set_color('#888888')

    plt.tight_layout()

    output_path = Path(output_path)
    fig.savefig(output_path.with_suffix('.pdf'), format='pdf')
    fig.savefig(output_path.with_suffix('.png'), format='png', dpi=300)
    print(f"Saved: {output_path.with_suffix('.pdf')}")
    print(f"Saved: {output_path.with_suffix('.png')}")

    plt.close()


def main():
    parser = argparse.ArgumentParser(description='Generate heatmap visualization')
    parser.add_argument('--input', '-i', required=True, help='Input JSON file')
    parser.add_argument('--output', '-o', default='heatmap', help='Output file basename')
    parser.add_argument('--title', '-t', default=None, help='Plot title')
    parser.add_argument('--style', choices=['v1', 'v2'], default='v2',
                        help='Visualization style (v1=imshow, v2=pcolormesh)')
    args = parser.parse_args()

    benefit, hot_pcts, compress_pcts = load_results(args.input)

    print(f"Loaded {len(hot_pcts)} x {len(compress_pcts)} heatmap")
    print(f"Benefit range: {benefit.min():.1f}% to {benefit.max():.1f}%")

    if args.style == 'v1':
        create_heatmap(benefit, hot_pcts, compress_pcts, args.output, args.title)
    else:
        create_heatmap_v2(benefit, hot_pcts, compress_pcts, args.output, args.title)


if __name__ == '__main__':
    main()
