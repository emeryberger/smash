#!/usr/bin/env python3
"""Generate LaTeX macros from benchmark results JSON files.

Reads paper_results/{ablation,compress_only,duckdb_compression}_results.json
and writes paper/results_macros.tex with \newcommand definitions that the
paper can reference directly.

Usage:
    cd paper && python3 gen_macros.py
"""

import json
import math
import os
import sys
from pathlib import Path

PAPER_DIR = Path(__file__).resolve().parent
RESULTS_DIR = PAPER_DIR.parent / "paper_results"
OUTPUT = PAPER_DIR / "results_macros.tex"


def fmt_int(v):
    """Format as integer."""
    if v is None or v == "N/A":
        return "---"
    return str(int(round(v)))


def fmt_pct(v, decimals=0):
    """Format as percentage without % sign."""
    if v is None or v == "N/A":
        return "---"
    if decimals == 0:
        return str(int(round(v)))
    return f"{v:.{decimals}f}"


def fmt_ratio(v, decimals=1):
    """Format as a ratio with x suffix."""
    if v is None or v == "N/A":
        return "---"
    return f"{v:.{decimals}f}"


def safe_get(d, *keys, default=None):
    """Nested dict get."""
    for k in keys:
        if not isinstance(d, dict):
            return default
        d = d.get(k, default)
    return d


def compute_auc_reduction(baseline_auc, target_auc):
    if baseline_auc is None or target_auc is None:
        return None
    if baseline_auc == 0:
        return 0.0
    return (baseline_auc - target_auc) / baseline_auc * 100


def main():
    lines = []
    lines.append("% Auto-generated from benchmark results. Do not edit by hand.")
    lines.append("% Regenerate with: cd paper && python3 gen_macros.py")
    lines.append("")

    # ── Load data ────────────────────────────────────────────────────────
    ablation = {}
    compress_only = {}
    duckdb_comp = {}

    abl_path = RESULTS_DIR / "ablation_results.json"
    co_path = RESULTS_DIR / "compress_only_results.json"
    dc_path = RESULTS_DIR / "duckdb_compression_results.json"

    if abl_path.exists():
        ablation = json.loads(abl_path.read_text())
    if co_path.exists():
        compress_only = json.loads(co_path.read_text())
    if dc_path.exists():
        duckdb_comp = json.loads(dc_path.read_text())

    # ── Helper: define a LaTeX macro ─────────────────────────────────────
    defined = set()

    def defcmd(name, value):
        if name in defined:
            return  # skip duplicates
        defined.add(name)
        # LaTeX command names can't have underscores or digits easily,
        # so we use camelCase naming
        lines.append(f"\\newcommand{{\\{name}}}{{{value}}}")

    # ── Ablation results (RQ1 + RQ3) ────────────────────────────────────
    # Canonical app names for macros
    app_macro_names = {
        "sqlite": "Sqlite",
        "rocksdb": "Rocksdb",
        "memcached": "Memcached",
        "redis": "Redis",
        "redis_ext": "RedisExt",
        "duckdb": "Duckdb",
    }

    config_macro_names = {
        "B0": "Baseline",
        "B1": "Default",
        "MESH": "Mesh",
        "DICT": "Dict",
        "T1a": "NoArenas",
        "T1c": "LzFourOnly",
        "T2a": "NoZeroDefer",
        "T1e": "NoPrefetch",
        "T1f": "SingleWorker",
        "B2": "NoCompress",
    }

    lines.append("")
    lines.append("% ── Ablation results (per-app, per-config) ──")

    for app, app_m in app_macro_names.items():
        if app not in ablation:
            continue
        for cfg_id, cfg_m in config_macro_names.items():
            med = safe_get(ablation, app, cfg_id, "median", default={})
            if not med:
                continue

            prefix = f"abl{app_m}{cfg_m}"

            rss_red = med.get("rss_reduction_pct")
            fill_rss = med.get("peak_rss_mb")
            serve_rss = med.get("steady_rss_mb")
            min_rss = med.get("min_rss_mb")
            cool_rss = med.get("post_cool_rss_mb")
            auc = med.get("auc_mb_sec")
            ops = med.get("ops_per_sec")

            if rss_red is not None:
                defcmd(f"{prefix}RssRed", fmt_pct(rss_red, 1))
                defcmd(f"{prefix}RssRedInt", fmt_pct(rss_red, 0))
            if fill_rss is not None:
                defcmd(f"{prefix}FillRss", fmt_int(fill_rss))
            if serve_rss is not None:
                defcmd(f"{prefix}ServeRss", fmt_int(serve_rss))
            if min_rss is not None:
                defcmd(f"{prefix}MinRss", fmt_int(min_rss))
            if cool_rss is not None:
                defcmd(f"{prefix}CoolRss", fmt_int(cool_rss))
            if auc is not None:
                defcmd(f"{prefix}Auc", fmt_int(auc))
            if ops is not None and ops != "N/A":
                defcmd(f"{prefix}Ops", fmt_int(ops))

    # ── Derived: delta from default for ablation configs ─────────────────
    lines.append("")
    lines.append("% ── Ablation deltas (pp from default) ──")

    ablation_configs_order = ["DICT", "T1a", "T1c", "T2a", "T1e", "T1f", "B2"]

    for app, app_m in app_macro_names.items():
        if app not in ablation:
            continue
        default_red = safe_get(ablation, app, "B1", "median", "rss_reduction_pct")
        if default_red is None:
            continue
        for cfg_id in ablation_configs_order:
            cfg_m = config_macro_names[cfg_id]
            cfg_red = safe_get(ablation, app, cfg_id, "median", "rss_reduction_pct")
            if cfg_red is None:
                continue
            delta = cfg_red - default_red
            sign = "+" if delta >= 0 else ""
            defcmd(f"abl{app_m}{cfg_m}Delta", f"{sign}{delta:.1f}")

    # ── AUC reductions ──────────────────────────────────────────────────
    lines.append("")
    lines.append("% ── AUC reductions (%) ──")

    for app, app_m in app_macro_names.items():
        if app not in ablation:
            continue
        b0_auc = safe_get(ablation, app, "B0", "median", "auc_mb_sec")
        b1_auc = safe_get(ablation, app, "B1", "median", "auc_mb_sec")
        mesh_auc = safe_get(ablation, app, "MESH", "median", "auc_mb_sec")

        if b0_auc is not None and b1_auc is not None:
            red = compute_auc_reduction(b0_auc, b1_auc)
            defcmd(f"auc{app_m}SmashRed", fmt_pct(red, 1))
        if b0_auc is not None and mesh_auc is not None:
            red = compute_auc_reduction(b0_auc, mesh_auc)
            defcmd(f"auc{app_m}MeshRed", fmt_pct(red, 1))

    # ── Compress-only results (RQ5) ─────────────────────────────────────
    lines.append("")
    lines.append("% ── Compress-only results ──")

    for app, app_m in app_macro_names.items():
        for suffix, co_suffix in [("baseline", "Baseline"), ("compress_only", "CompOnly"),
                                   ("full_smash", "FullSmash")]:
            key = f"{app}_{suffix}"
            med = safe_get(compress_only, key, "median", default={})
            if not med:
                continue

            prefix = f"co{app_m}{co_suffix}"
            serve_rss = med.get("steady_rss_mb")
            cool_rss = med.get("post_cool_rss_mb")
            rss_red = med.get("rss_reduction_pct")
            ops = med.get("ops_per_sec")

            rss_val = cool_rss if cool_rss is not None else serve_rss
            if rss_val is not None:
                defcmd(f"{prefix}Rss", fmt_int(rss_val))
            if rss_red is not None:
                defcmd(f"{prefix}RssRed", fmt_pct(rss_red, 1))
            if ops is not None and ops != "N/A":
                defcmd(f"{prefix}Ops", fmt_int(ops))

    # ── Compute compress-only overhead/reduction vs baseline ────────────
    lines.append("")
    lines.append("% ── Compress-only relative to baseline ──")

    for app, app_m in app_macro_names.items():
        bl_key = f"{app}_baseline"
        co_key = f"{app}_compress_only"
        fs_key = f"{app}_full_smash"

        bl_rss = safe_get(compress_only, bl_key, "median", "post_cool_rss_mb")
        if bl_rss is None:
            bl_rss = safe_get(compress_only, bl_key, "median", "steady_rss_mb")
        co_rss = safe_get(compress_only, co_key, "median", "post_cool_rss_mb")
        if co_rss is None:
            co_rss = safe_get(compress_only, co_key, "median", "steady_rss_mb")
        fs_rss = safe_get(compress_only, fs_key, "median", "post_cool_rss_mb")
        if fs_rss is None:
            fs_rss = safe_get(compress_only, fs_key, "median", "steady_rss_mb")

        if bl_rss and co_rss and bl_rss > 0:
            overhead = (co_rss - bl_rss) / bl_rss * 100
            defcmd(f"co{app_m}CompOnlyOverhead", fmt_pct(overhead, 0))
        if bl_rss and fs_rss and bl_rss > 0:
            reduction = (bl_rss - fs_rss) / bl_rss * 100
            defcmd(f"co{app_m}FullSmashReduction", fmt_pct(reduction, 0))

    # ── Summary statistics ──────────────────────────────────────────────
    lines.append("")
    lines.append("% ── Summary ranges ──")

    # RSS reduction range (excluding DuckDB which is near-zero)
    app_reds = []
    for app in ["sqlite", "rocksdb", "memcached", "redis", "redis_ext"]:
        r = safe_get(ablation, app, "B1", "median", "rss_reduction_pct")
        if r is not None:
            app_reds.append(r)
    if app_reds:
        defcmd("rssRedMin", fmt_pct(min(app_reds), 0))
        defcmd("rssRedMax", fmt_pct(max(app_reds), 0))

    # AUC reduction range
    auc_reds = []
    for app in app_macro_names:
        b0_auc = safe_get(ablation, app, "B0", "median", "auc_mb_sec")
        b1_auc = safe_get(ablation, app, "B1", "median", "auc_mb_sec")
        if b0_auc and b1_auc:
            auc_reds.append(compute_auc_reduction(b0_auc, b1_auc))
    if auc_reds:
        defcmd("aucRedMin", fmt_pct(min(auc_reds), 0))
        defcmd("aucRedMax", fmt_pct(max(auc_reds), 0))

    # Ablation: average delta for each config
    for cfg_id in ablation_configs_order:
        cfg_m = config_macro_names[cfg_id]
        deltas = []
        for app in app_macro_names:
            if app not in ablation:
                continue
            default_red = safe_get(ablation, app, "B1", "median", "rss_reduction_pct")
            cfg_red = safe_get(ablation, app, cfg_id, "median", "rss_reduction_pct")
            if default_red is not None and cfg_red is not None:
                deltas.append(cfg_red - default_red)
        if deltas:
            avg = sum(deltas) / len(deltas)
            sign = "+" if avg >= 0 else ""
            defcmd(f"ablAvg{cfg_m}Delta", f"{sign}{avg:.1f}")

    # ── Write output ────────────────────────────────────────────────────
    lines.append("")
    OUTPUT.write_text("\n".join(lines) + "\n")
    print(f"Wrote {len(defined)} macros to {OUTPUT}")


if __name__ == "__main__":
    main()
