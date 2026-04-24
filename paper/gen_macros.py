#!/usr/bin/env python3
"""Generate LaTeX macros from benchmark results JSON files.

Reads paper_results/{macos,linux}/{ablation,compress_only,duckdb_compression}_results.json
and writes paper/results_macros.tex with \\newcommand definitions that the
paper can reference directly.

Macros are suffixed by platform: *Mac for macOS, *Lin for Linux.
Unprefixed aliases point to the PRIMARY_PLATFORM results.

Usage:
    cd paper && python3 gen_macros.py
"""

import csv
import json
import os
import sys
from collections import defaultdict
from pathlib import Path

PAPER_DIR = Path(__file__).resolve().parent
RESULTS_DIR = PAPER_DIR.parent / "paper_results"
OUTPUT = PAPER_DIR / "results_macros.tex"

PRIMARY_PLATFORM = "linux"  # Linux is the primary platform for paper results

PLATFORMS = {
    "macos": {"dir": "macos", "suffix": "Mac"},
    "linux": {"dir": "linux", "suffix": "Lin"},
}

APP_MACRO_NAMES = {
    "sqlite": "Sqlite",
    "rocksdb": "Rocksdb",
    "memcached": "Memcached",
    "redis": "Redis",
    "redis_ext": "RedisExt",
    "redis_patched": "RedisPatched",
    "redis_ext_patched": "RedisExtPatched",
    "duckdb": "Duckdb",
}

CONFIG_MACRO_NAMES = {
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

ABLATION_CONFIGS_ORDER = ["DICT", "T1a", "T1c", "T2a", "T1e", "T1f", "B2"]


def fmt_int(v):
    if v is None or v == "N/A":
        return "---"
    return str(int(round(v)))


def fmt_pct(v, decimals=0):
    if v is None or v == "N/A":
        return "---"
    if decimals == 0:
        return str(int(round(v)))
    return f"{v:.{decimals}f}"


def fmt_ratio(v, decimals=1):
    if v is None or v == "N/A":
        return "---"
    return f"{v:.{decimals}f}"


def safe_get(d, *keys, default=None):
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


def load_csv_avg(path):
    """Load CSV and return {config: {col: avg_value}}."""
    if not path.exists():
        return {}
    rows = list(csv.DictReader(open(path)))
    if not rows:
        return {}
    groups = defaultdict(list)
    for r in rows:
        cfg = r["config"]
        groups[cfg].append(r)
    result = {}
    for cfg, rs in groups.items():
        avgs = {}
        for col in rs[0].keys():
            if col in ("config", "run"):
                continue
            vals = []
            for r in rs:
                try:
                    vals.append(float(r[col]))
                except (ValueError, TypeError):
                    pass
            if vals:
                avgs[col] = sum(vals) / len(vals)
        result[cfg] = avgs
    return result


def generate_platform_macros(plat_suffix, results_dir, lines, defined):
    """Generate all macros for one platform with the given suffix."""

    def defcmd(name, value):
        full_name = f"{name}{plat_suffix}"
        if full_name in defined:
            return
        defined.add(full_name)
        lines.append(f"\\newcommand{{\\{full_name}}}{{{value}}}")

    # ── Load data ────────────────────────────────────────────────────
    ablation = {}
    compress_only = {}
    duckdb_comp = {}

    abl_path = results_dir / "ablation_results.json"
    co_path = results_dir / "compress_only_results.json"
    dc_path = results_dir / "duckdb_compression_results.json"

    if abl_path.exists():
        ablation = json.loads(abl_path.read_text())
    if co_path.exists():
        compress_only = json.loads(co_path.read_text())
    if dc_path.exists():
        duckdb_comp = json.loads(dc_path.read_text())

    # ── Ablation results (RQ1 + RQ3) ────────────────────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] Ablation results (per-app, per-config) ──")

    for app, app_m in APP_MACRO_NAMES.items():
        if app not in ablation:
            continue
        for cfg_id, cfg_m in CONFIG_MACRO_NAMES.items():
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

    # ── Ablation deltas ────────────────────────────────────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] Ablation deltas (pp from default) ──")

    for app, app_m in APP_MACRO_NAMES.items():
        if app not in ablation:
            continue
        default_red = safe_get(ablation, app, "B1", "median", "rss_reduction_pct")
        if default_red is None:
            continue
        for cfg_id in ABLATION_CONFIGS_ORDER:
            cfg_m = CONFIG_MACRO_NAMES[cfg_id]
            cfg_red = safe_get(ablation, app, cfg_id, "median", "rss_reduction_pct")
            if cfg_red is None:
                continue
            delta = cfg_red - default_red
            sign = "+" if delta >= 0 else ""
            defcmd(f"abl{app_m}{cfg_m}Delta", f"{sign}{delta:.1f}")

    # ── AUC reductions ─────────────────────────────────────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] AUC reductions (%) ──")

    for app, app_m in APP_MACRO_NAMES.items():
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

    # ── Compress-only results (RQ5) ────────────────────────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] Compress-only results ──")

    for app, app_m in APP_MACRO_NAMES.items():
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

    # ── Compress-only overhead/reduction vs baseline ───────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] Compress-only relative to baseline ──")

    for app, app_m in APP_MACRO_NAMES.items():
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

    # ── Value-add results (multi-allocator, RQ5) ───────────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] Value-add: allocator × compression (RQ5) ──")

    va_mc_path = results_dir / "value_add_memcached.csv"
    va_sq_path = results_dir / "value_add_sqlite.csv"

    va_mc = load_csv_avg(va_mc_path)
    va_sq = load_csv_avg(va_sq_path)

    mc_bl_rss = va_mc.get("system_malloc", {}).get("fill_rss")
    if mc_bl_rss:
        defcmd("vaMemcachedBaselineRss", fmt_int(mc_bl_rss))

    for cfg, macro in [("jemalloc", "Jemalloc"), ("mimalloc", "Mimalloc")]:
        d = va_mc.get(cfg, {})
        if "min_rss" in d:
            defcmd(f"vaMemcached{macro}Rss", fmt_int(d["min_rss"]))

    for cfg, macro in [("sys+compress", "SysComp"), ("je+compress", "JeComp"),
                        ("mi+compress", "MiComp")]:
        d = va_mc.get(cfg, {})
        if "min_rss" in d:
            defcmd(f"vaMemcached{macro}Rss", fmt_int(d["min_rss"]))
            if mc_bl_rss and mc_bl_rss > 0:
                oh = (d["min_rss"] - mc_bl_rss) / mc_bl_rss * 100
                defcmd(f"vaMemcached{macro}Oh", fmt_pct(oh, 0))

    noopt_mc = va_mc.get("smash_noopt", {})
    if "min_rss" in noopt_mc:
        defcmd("vaMemcachedNooptRss", fmt_int(noopt_mc["min_rss"]))
        if mc_bl_rss and mc_bl_rss > 0:
            red = (mc_bl_rss - noopt_mc["min_rss"]) / mc_bl_rss * 100
            defcmd("vaMemcachedNooptRed", fmt_pct(red, 0))

    full_mc = va_mc.get("full_smash", {})
    if "min_rss" in full_mc:
        defcmd("vaMemcachedFullRss", fmt_int(full_mc["min_rss"]))
        if mc_bl_rss and mc_bl_rss > 0:
            red = (mc_bl_rss - full_mc["min_rss"]) / mc_bl_rss * 100
            defcmd("vaMemcachedFullRed", fmt_pct(red, 0))

    sq_bl_rss = va_sq.get("system_malloc", {}).get("peak_rss")
    if sq_bl_rss:
        defcmd("vaSqliteBaselineRss", fmt_int(sq_bl_rss))

    for cfg, macro in [("sys+compress", "SysComp"), ("mi+compress", "MiComp")]:
        d = va_sq.get(cfg, {})
        if "min_rss" in d:
            defcmd(f"vaSqlite{macro}Rss", fmt_int(d["min_rss"]))
            if sq_bl_rss and sq_bl_rss > 0:
                oh = (d["min_rss"] - sq_bl_rss) / sq_bl_rss * 100
                defcmd(f"vaSqlite{macro}Oh", fmt_pct(oh, 0))

    noopt_sq = va_sq.get("smash_noopt", {})
    if "min_rss" in noopt_sq:
        defcmd("vaSqliteNooptRss", fmt_int(noopt_sq["min_rss"]))
        if sq_bl_rss and sq_bl_rss > 0:
            red = (sq_bl_rss - noopt_sq["min_rss"]) / sq_bl_rss * 100
            defcmd("vaSqliteNooptRed", fmt_pct(red, 0))

    full_sq = va_sq.get("full_smash", {})
    if "min_rss" in full_sq:
        defcmd("vaSqliteFullRss", fmt_int(full_sq["min_rss"]))
        if sq_bl_rss and sq_bl_rss > 0:
            red = (sq_bl_rss - full_sq["min_rss"]) / sq_bl_rss * 100
            defcmd("vaSqliteFullRed", fmt_pct(red, 0))

    # ── Cross-app comparisons (stock vs patched Redis) ─────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] Cross-app: stock vs patched Redis ──")

    stock_redis_bl = safe_get(ablation, "redis", "B0", "median", "steady_rss_mb")
    patched_redis_bl = safe_get(ablation, "redis_patched", "B0", "median", "steady_rss_mb")
    if stock_redis_bl and patched_redis_bl and stock_redis_bl > 0:
        red = (stock_redis_bl - patched_redis_bl) / stock_redis_bl * 100
        defcmd("redisPatchBaselineRedPct", fmt_pct(red, 0))

    stock_redis_ext_bl = safe_get(ablation, "redis_ext", "B0", "median", "steady_rss_mb")
    patched_redis_ext_bl = safe_get(ablation, "redis_ext_patched", "B0", "median", "steady_rss_mb")
    if stock_redis_ext_bl and patched_redis_ext_bl and stock_redis_ext_bl > 0:
        red = (stock_redis_ext_bl - patched_redis_ext_bl) / stock_redis_ext_bl * 100
        defcmd("redisExtPatchBaselineRedPct", fmt_pct(red, 0))

    # ── Summary statistics ─────────────────────────────────────────────
    lines.append("")
    lines.append(f"% ── [{plat_suffix}] Summary ranges ──")

    app_reds = []
    for app in ["sqlite", "rocksdb", "memcached", "redis", "redis_ext"]:
        r = safe_get(ablation, app, "B1", "median", "rss_reduction_pct")
        if r is not None:
            app_reds.append(r)
    if app_reds:
        defcmd("rssRedMin", fmt_pct(min(app_reds), 0))
        defcmd("rssRedMax", fmt_pct(max(app_reds), 0))

    auc_reds = []
    for app in ["sqlite", "rocksdb", "memcached", "redis", "redis_ext",
                 "redis_patched", "redis_ext_patched"]:
        b0_auc = safe_get(ablation, app, "B0", "median", "auc_mb_sec")
        b1_auc = safe_get(ablation, app, "B1", "median", "auc_mb_sec")
        if b0_auc and b1_auc:
            auc_reds.append(compute_auc_reduction(b0_auc, b1_auc))
    if auc_reds:
        defcmd("aucRedMin", fmt_pct(min(auc_reds), 0))
        defcmd("aucRedMax", fmt_pct(max(auc_reds), 0))

    for cfg_id in ABLATION_CONFIGS_ORDER:
        cfg_m = CONFIG_MACRO_NAMES[cfg_id]
        deltas = []
        for app in APP_MACRO_NAMES:
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

    return defined


def main():
    lines = []
    lines.append("% Auto-generated from benchmark results. Do not edit by hand.")
    lines.append(f"% Regenerate with: cd paper && python3 gen_macros.py")
    lines.append(f"% Primary platform: {PRIMARY_PLATFORM}")
    lines.append("")

    defined = set()

    # Generate platform-prefixed macros for each platform
    for plat_name, plat_info in PLATFORMS.items():
        plat_dir = RESULTS_DIR / plat_info["dir"]
        if not plat_dir.exists():
            continue
        plat_suffix = plat_info["suffix"]
        lines.append("")
        lines.append(f"% {'='*60}")
        lines.append(f"% Platform: {plat_name} (suffix: {plat_suffix})")
        lines.append(f"% {'='*60}")
        generate_platform_macros(plat_suffix, plat_dir, lines, defined)

    # Generate unprefixed aliases from primary platform
    primary_suffix = PLATFORMS[PRIMARY_PLATFORM]["suffix"]
    lines.append("")
    lines.append(f"% {'='*60}")
    lines.append(f"% Unprefixed aliases → {PRIMARY_PLATFORM} (*{primary_suffix})")
    lines.append(f"% {'='*60}")

    alias_count = 0
    for name in sorted(defined):
        if name.endswith(primary_suffix):
            unprefixed = name[:-len(primary_suffix)]
            if unprefixed not in defined:
                lines.append(f"\\newcommand{{\\{unprefixed}}}{{\\{name}}}")
                defined.add(unprefixed)
                alias_count += 1

    # ── Fallback stubs for known-missing data ──────────────────────────
    lines.append("")
    lines.append("% ── Fallback stubs (missing data → ---) ──")
    stubs = [
        "ablSqliteMeshServeRss",
        "ablSqliteMeshRssRed",
        "ablSqliteMeshRssRedInt",
        "ablSqliteMeshFillRss",
        "ablSqliteMeshMinRss",
        "ablSqliteMeshCoolRss",
        "ablSqliteMeshAuc",
        "ablSqliteMeshOps",
        "aucSqliteMeshRed",
    ]
    for stub in stubs:
        if stub not in defined:
            lines.append(f"\\newcommand{{\\{stub}}}{{---}}")
            defined.add(stub)

    lines.append("")
    OUTPUT.write_text("\n".join(lines) + "\n")
    print(f"Wrote {len(defined)} macros ({alias_count} aliases → {PRIMARY_PLATFORM}) to {OUTPUT}")


if __name__ == "__main__":
    main()
