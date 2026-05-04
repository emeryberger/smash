#!/usr/bin/env python3
"""Generate publication-quality heatmap for Smash coldness × compressibility experiment.

Produces a LaTeX-ready PDF/PNG with Sanborn-inspired thermal colormap.
"""

import argparse
import json
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
from matplotlib.patches import Rectangle
from pathlib import Path

# Use sans-serif fonts for publication
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
    """Create publication-quality heatmap."""

    # Figure size for single-column (3.5") or double-column (7") LaTeX
    fig, ax = plt.subplots(figsize=(4.5, 3.5))

    # Custom diverging colormap: blue (negative/overhead) -> white (zero) -> red (positive/benefit)
    # Sanborn-style uses warm colors for "hot" values
    colors_neg = plt.cm.Blues_r(np.linspace(0.2, 0.8, 128))  # Blue for overhead
    colors_pos = plt.cm.Reds(np.linspace(0.2, 0.9, 128))     # Red for benefit
    all_colors = np.vstack([colors_neg, colors_pos])
    cmap = mcolors.LinearSegmentedColormap.from_list('smash_benefit', all_colors)

    # Determine color scale limits (symmetric around zero, or based on data)
    vmax = max(abs(benefit.min()), abs(benefit.max()))
    vmin = -vmax * 0.15  # Asymmetric: less negative range since overhead is small
    vmax = benefit.max() * 1.05

    # For truly symmetric colormap around zero:
    # vmin, vmax = -max(abs(benefit.min()), benefit.max()), max(abs(benefit.min()), benefit.max())

    # Create custom norm to center white at zero
    # Use TwoSlopeNorm to have different scales for negative and positive
    norm = mcolors.TwoSlopeNorm(vmin=min(-10, benefit.min()), vcenter=0, vmax=benefit.max())

    # Plot heatmap
    im = ax.imshow(benefit, cmap=cmap, norm=norm, aspect='auto')

    # Add text annotations
    for i in range(len(hot_pcts)):
        for j in range(len(compress_pcts)):
            val = benefit[i, j]
            # Choose text color based on background intensity
            # White text on dark backgrounds, black on light
            bg_color = cmap(norm(val))
            luminance = 0.299 * bg_color[0] + 0.587 * bg_color[1] + 0.114 * bg_color[2]
            text_color = 'white' if luminance < 0.6 else 'black'

            # Format: show sign, one decimal place
            if val >= 0:
                text = f'+{val:.0f}%' if val >= 10 else f'+{val:.1f}%'
            else:
                text = f'{val:.0f}%' if val <= -10 else f'{val:.1f}%'

            ax.text(j, i, text, ha='center', va='center',
                    color=text_color, fontsize=9, fontweight='medium')

    # Axis labels
    ax.set_xticks(range(len(compress_pcts)))
    ax.set_xticklabels([f'{c}%' for c in compress_pcts])
    ax.set_yticks(range(len(hot_pcts)))
    ax.set_yticklabels([f'{h}%' for h in hot_pcts])

    ax.set_xlabel('Data Compressibility', fontweight='medium')
    ax.set_ylabel('Hot Data Fraction', fontweight='medium')

    # Title
    if title:
        ax.set_title(title, fontweight='bold', pad=10)

    # Colorbar
    cbar = fig.colorbar(im, ax=ax, shrink=0.85, pad=0.02)
    cbar.set_label('RSS Reduction (%)', fontweight='medium')

    # Add subtle grid lines between cells
    for i in range(len(hot_pcts) + 1):
        ax.axhline(i - 0.5, color='white', linewidth=0.5, alpha=0.3)
    for j in range(len(compress_pcts) + 1):
        ax.axvline(j - 0.5, color='white', linewidth=0.5, alpha=0.3)

    # Remove spines
    for spine in ax.spines.values():
        spine.set_visible(False)

    plt.tight_layout()

    # Save in multiple formats
    output_path = Path(output_path)
    fig.savefig(output_path.with_suffix('.pdf'), format='pdf')
    fig.savefig(output_path.with_suffix('.png'), format='png', dpi=300)
    print(f"Saved: {output_path.with_suffix('.pdf')}")
    print(f"Saved: {output_path.with_suffix('.png')}")

    plt.close()


def create_sanborn_colormap():
    """Create Sanborn-inspired colormap: cool blues -> cream -> warm earth tones."""
    from matplotlib.colors import LinearSegmentedColormap

    # Sanborn color palette: deep blue -> light blue -> cream -> tan -> warm orange -> deep red
    colors = [
        '#2166ac',  # Deep blue (negative/overhead)
        '#67a9cf',  # Light blue
        '#d1e5f0',  # Very light blue
        '#f7f7f7',  # Near white/cream (zero point)
        '#fddbc7',  # Light peach
        '#ef8a62',  # Warm orange
        '#b2182b',  # Deep red (high positive)
    ]

    # Create colormap with more resolution near zero
    return LinearSegmentedColormap.from_list('sanborn', colors, N=256)


def create_heatmap_v2(benefit, hot_pcts, compress_pcts, output_path, title=None):
    """Sanborn-style heatmap with sans-serif fonts."""

    fig, ax = plt.subplots(figsize=(5, 4))

    # Sanborn-inspired colormap: cool blues -> cream -> warm earth tones
    cmap = create_sanborn_colormap()

    # Normalize with zero centered
    vmin = min(-15, benefit.min())
    vmax = benefit.max()
    norm = mcolors.TwoSlopeNorm(vmin=vmin, vcenter=0, vmax=vmax)

    # Plot with pcolormesh for better control
    im = ax.pcolormesh(benefit, cmap=cmap, norm=norm, edgecolors='white', linewidth=1)

    # Add text annotations (offset by 0.5 for pcolormesh)
    for i in range(len(hot_pcts)):
        for j in range(len(compress_pcts)):
            val = benefit[i, j]
            bg_color = cmap(norm(val))
            luminance = 0.299 * bg_color[0] + 0.587 * bg_color[1] + 0.114 * bg_color[2]
            text_color = 'white' if luminance < 0.55 else 'black'

            if val >= 0:
                text = f'+{val:.0f}' if val >= 10 else f'+{val:.1f}'
            else:
                text = f'{val:.0f}' if val <= -10 else f'{val:.1f}'

            ax.text(j + 0.5, i + 0.5, text, ha='center', va='center',
                    color=text_color, fontsize=10, fontweight='bold')

    # Axis setup
    ax.set_xticks(np.arange(len(compress_pcts)) + 0.5)
    ax.set_xticklabels([f'{c}%' for c in compress_pcts])
    ax.set_yticks(np.arange(len(hot_pcts)) + 0.5)
    ax.set_yticklabels([f'{h}%' for h in hot_pcts])

    ax.set_xlabel('Data Compressibility', fontsize=11, fontweight='medium')
    ax.set_ylabel('Hot Data Fraction', fontsize=11, fontweight='medium')

    if title:
        ax.set_title(title, fontsize=12, fontweight='bold', pad=12)

    # Colorbar
    cbar = fig.colorbar(im, ax=ax, shrink=0.9, pad=0.02)
    cbar.set_label('RSS Reduction (%)', fontsize=10, fontweight='medium')
    cbar.ax.tick_params(labelsize=9)

    # Style
    ax.set_xlim(0, len(compress_pcts))
    ax.set_ylim(0, len(hot_pcts))
    ax.invert_yaxis()  # 0% hot at top

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
