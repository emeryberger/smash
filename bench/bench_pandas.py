#!/usr/bin/env python3
"""Python+pandas cold-columns benchmark for Smash.

Creates a large DataFrame with many columns, accesses a subset ("hot columns"),
lets the rest go cold, then measures RSS reduction. Finally accesses cold
columns to measure decompression overhead.

Key design: pandas/NumPy stores each column as a contiguous C array allocated
via malloc. Columns not accessed become cold pages that Smash can compress.
We use integer and low-cardinality string-like data (stored as int codes)
for good compressibility.

Usage:
    cd build
    python3 ../bench/bench_pandas.py [--quick]

Prerequisites:
    pip install pandas numpy
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def get_rss_mb_external(pid):
    """Get RSS from outside the process (for LD_PRELOAD wrapper)."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024.0
    except (FileNotFoundError, PermissionError):
        pass
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)], text=True
        )
        return float(out.strip()) / 1024.0
    except Exception:
        return 0.0


# ── The actual benchmark (runs as a subprocess with LD_PRELOAD) ──────────────

BENCH_SCRIPT = '''
import numpy as np
import pandas as pd
import time
import os
import sys
import ctypes
import ctypes.util

def _make_mach_rss_reader():
    """Build a fast RSS reader using mach task_info (macOS only)."""
    libc = ctypes.CDLL(ctypes.util.find_library("c"))

    MACH_TASK_BASIC_INFO = 20
    MACH_TASK_BASIC_INFO_COUNT = 12  # 12 natural_t fields

    class MachTaskBasicInfo(ctypes.Structure):
        _fields_ = [
            ("virtual_size", ctypes.c_uint64),
            ("resident_size", ctypes.c_uint64),
            ("resident_size_max", ctypes.c_uint64),
            ("user_time_sec", ctypes.c_uint32),
            ("user_time_usec", ctypes.c_uint32),
            ("system_time_sec", ctypes.c_uint32),
            ("system_time_usec", ctypes.c_uint32),
            ("policy", ctypes.c_int32),
            ("suspend_count", ctypes.c_int32),
        ]

    task_self = libc.mach_task_self
    task_self.restype = ctypes.c_uint32
    task_info = libc.task_info
    task_info.restype = ctypes.c_int32

    def read_rss_mb():
        info = MachTaskBasicInfo()
        count = ctypes.c_uint32(MACH_TASK_BASIC_INFO_COUNT)
        ret = task_info(
            task_self(),
            MACH_TASK_BASIC_INFO,
            ctypes.byref(info),
            ctypes.byref(count),
        )
        if ret == 0:
            return info.resident_size / (1024 * 1024)
        return 0.0

    return read_rss_mb

def _make_proc_rss_reader():
    """Build a fast RSS reader using /proc/self/status (Linux)."""
    def read_rss_mb():
        try:
            with open(f"/proc/{os.getpid()}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1]) / 1024.0
        except Exception:
            pass
        return 0.0
    return read_rss_mb

# Pick the right reader for this platform — no subprocess overhead
if sys.platform == "darwin":
    get_rss_mb = _make_mach_rss_reader()
else:
    get_rss_mb = _make_proc_rss_reader()

# Parameters from environment
num_rows = int(os.environ.get("BENCH_ROWS", "2000000"))
num_cols = int(os.environ.get("BENCH_COLS", "100"))
hot_cols = int(os.environ.get("BENCH_HOT_COLS", "10"))
cool_sec = int(os.environ.get("BENCH_COOL_SEC", "30"))
data_type = os.environ.get("BENCH_DATA_TYPE", "mixed")

np.random.seed(42)

print(f"Creating DataFrame: {num_rows} rows x {num_cols} columns ({data_type})")
rss_before = get_rss_mb()
print(f"METRIC pre_alloc_rss_mb {rss_before:.1f}")

# Create DataFrame with compressible data patterns
# Each column is a contiguous numpy array (malloc'd)
data = {}
for i in range(num_cols):
    if data_type == "int" or (data_type == "mixed" and i % 3 != 2):
        if i % 2 == 0:
            # Monotonically increasing with small deltas (very compressible)
            base = np.arange(num_rows, dtype=np.int64) + i * 1000
            noise = np.random.randint(0, 10, size=num_rows, dtype=np.int64)
            data[f"col_{i:03d}"] = base + noise
        else:
            # Low-cardinality integers (compressible: many repeated values)
            data[f"col_{i:03d}"] = np.random.randint(0, 100, size=num_rows, dtype=np.int64)
    else:
        # Float columns with structure (not purely random)
        data[f"col_{i:03d}"] = np.random.normal(i * 10.0, 1.0, size=num_rows)

df = pd.DataFrame(data)

rss_loaded = get_rss_mb()
data_size_mb = df.memory_usage(deep=True).sum() / (1024 * 1024)
print(f"METRIC data_size_mb {data_size_mb:.1f}")
print(f"METRIC loaded_rss_mb {rss_loaded:.1f}")
print(f"DataFrame: {data_size_mb:.0f} MB in memory, RSS={rss_loaded:.0f} MB")

# ── Phase 1: Work with hot columns only ──────────────────────────────────
hot_col_names = [f"col_{i:03d}" for i in range(hot_cols)]
cold_col_names = [f"col_{i:03d}" for i in range(hot_cols, num_cols)]

print(f"\\nPhase 1: Working with {hot_cols} hot columns...")
t_start = time.time()

# Do real work on hot columns to keep them warm
for _ in range(5):
    result = df[hot_col_names].sum()
    result = df[hot_col_names].mean()
    result = df[hot_col_names].std()
    # Sorting touches all rows in hot columns
    _ = df[hot_col_names].sort_values(by=hot_col_names[0])

t_hot = time.time() - t_start
rss_hot = get_rss_mb()
print(f"METRIC hot_phase_time_sec {t_hot:.3f}")
print(f"METRIC hot_phase_rss_mb {rss_hot:.1f}")
print(f"  Hot phase: {t_hot:.2f}s, RSS={rss_hot:.0f} MB")

# ── Phase 2: Cool-down — stop touching cold columns ─────────────────────
print(f"\\nPhase 2: Cooling for {cool_sec}s (cold columns untouched)...")
rss_timeline = []
for i in range(cool_sec):
    time.sleep(1)
    rss = get_rss_mb()
    rss_timeline.append(rss)
    # Keep hot columns warm by doing light work
    if i % 5 == 0:
        _ = df[hot_col_names[:3]].mean()
    if (i + 1) % 10 == 0:
        print(f"    t={i+1}s, RSS={rss:.0f} MB")

min_rss = min(rss_timeline) if rss_timeline else rss_hot
cool_rss = rss_timeline[-1] if rss_timeline else rss_hot
print(f"METRIC min_rss_mb {min_rss:.1f}")
print(f"METRIC cool_rss_mb {cool_rss:.1f}")
print(f"  After cooling: RSS={cool_rss:.0f} MB (min={min_rss:.0f} MB)")

# ── Phase 3: Access cold columns (forces decompression) ─────────────────
print(f"\\nPhase 3: Accessing {len(cold_col_names)} cold columns...")
t_start = time.time()

# Touch all cold columns — this forces page decompression
cold_sums = df[cold_col_names].sum()
cold_means = df[cold_col_names].mean()

t_cold = time.time() - t_start
rss_after = get_rss_mb()
print(f"METRIC cold_access_time_sec {t_cold:.3f}")
print(f"METRIC post_cold_rss_mb {rss_after:.1f}")
print(f"  Cold access: {t_cold:.3f}s, RSS={rss_after:.0f} MB")

# ── Phase 4: Baseline cold-column access time (no compression) ──────────
# Re-access cold columns immediately (warm) for comparison
t_start = time.time()
_ = df[cold_col_names].sum()
_ = df[cold_col_names].mean()
t_warm = time.time() - t_start
print(f"METRIC warm_access_time_sec {t_warm:.3f}")
print(f"  Warm re-access: {t_warm:.3f}s")
if t_warm > 0:
    overhead = (t_cold / t_warm - 1) * 100
    print(f"METRIC cold_access_overhead_pct {overhead:.1f}")
    print(f"  Cold access overhead: {overhead:.1f}%")

# ── Summary metrics ──────────────────────────────────────────────────────
peak_rss = max(rss_loaded, rss_hot)
reduction = (1 - min_rss / peak_rss) * 100 if peak_rss > 0 else 0
auc = sum(rss_timeline) if rss_timeline else 0

print(f"\\nMETRIC peak_rss_mb {peak_rss:.1f}")
print(f"METRIC rss_reduction_pct {reduction:.1f}")
print(f"METRIC auc_mb_sec {auc:.1f}")
print(f"METRIC num_rows {num_rows}")
print(f"METRIC num_cols {num_cols}")
print(f"METRIC hot_cols {hot_cols}")

print(f"\\n{'='*50}")
print(f"  Peak RSS:      {peak_rss:.0f} MB")
print(f"  Min RSS:       {min_rss:.0f} MB")
print(f"  Reduction:     {reduction:.1f}%")
print(f"  Cold access:   {t_cold:.3f}s (vs {t_warm:.3f}s warm)")
print(f"{'='*50}")
'''


def parse_metrics(text):
    """Parse METRIC lines from subprocess output."""
    metrics = {}
    for line in text.splitlines():
        if line.startswith("METRIC "):
            parts = line.split()
            if len(parts) == 3:
                try:
                    metrics[parts[1]] = float(parts[2])
                except ValueError:
                    pass
    return metrics


def run_pandas_benchmark(smash_lib=None, num_rows=2_000_000, num_cols=100,
                         hot_cols=10, cool_sec=30, data_type="mixed"):
    """Run the pandas benchmark as a subprocess (so LD_PRELOAD works)."""
    env = os.environ.copy()
    env["BENCH_ROWS"] = str(num_rows)
    env["BENCH_COLS"] = str(num_cols)
    env["BENCH_HOT_COLS"] = str(hot_cols)
    env["BENCH_COOL_SEC"] = str(cool_sec)
    env["BENCH_DATA_TYPE"] = data_type

    if smash_lib:
        if sys.platform == "darwin":
            env["DYLD_INSERT_LIBRARIES"] = str(smash_lib)
        else:
            env["LD_PRELOAD"] = str(smash_lib)
        # Large-only mode: let Python's internal allocator (mimalloc in 3.13+)
        # handle small objects; Smash only manages large allocations (NumPy arrays).
        env["SMASH_LARGE_ONLY"] = "1"
        env["SMASH_VERY_COLD_TICKS"] = "5"

    config_name = "smash" if smash_lib else "baseline"
    print(f"  Running pandas benchmark ({config_name})...")
    print(f"    {num_rows} rows x {num_cols} cols, {hot_cols} hot, cool={cool_sec}s")

    # Write the benchmark script to a temp file
    import tempfile
    script_file = tempfile.NamedTemporaryFile(
        mode="w", suffix=".py", prefix="bench_pandas_", delete=False
    )
    script_file.write(BENCH_SCRIPT)
    script_file.close()

    proc = None
    try:
        # Run as subprocess to pick up LD_PRELOAD
        proc = subprocess.Popen(
            [sys.executable, script_file.name],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

        # Also sample RSS externally (in case internal measurement misses compression)
        rss_timeline_ext = []
        while proc.poll() is None:
            rss = get_rss_mb_external(proc.pid)
            if rss > 0:
                rss_timeline_ext.append(rss)
            time.sleep(1)

        stdout, stderr = proc.communicate(timeout=30)

        if proc.returncode != 0:
            print(f"    ERROR: benchmark exited with code {proc.returncode}")
            print(f"    stderr: {stderr[:500]}")
            return None

        # Print the benchmark output
        for line in stdout.splitlines():
            if not line.startswith("METRIC"):
                print(f"    {line}")

        metrics = parse_metrics(stdout)

        # Add external RSS measurements (more accurate with Smash)
        if rss_timeline_ext:
            metrics["ext_peak_rss_mb"] = max(rss_timeline_ext)
            metrics["ext_min_rss_mb"] = min(rss_timeline_ext)
            ext_reduction = (1 - min(rss_timeline_ext) / max(rss_timeline_ext)) * 100
            metrics["ext_rss_reduction_pct"] = ext_reduction

        return metrics

    except subprocess.TimeoutExpired:
        if proc:
            proc.kill()
        print("    ERROR: benchmark timed out")
        return None
    finally:
        os.unlink(script_file.name)


def main():
    parser = argparse.ArgumentParser(
        description="Python+pandas cold-columns benchmark for Smash"
    )
    parser.add_argument("--quick", action="store_true",
                        help="Smaller dataset, shorter cool-down")
    parser.add_argument("--smash-lib", default=None,
                        help="Path to libsmash.so/.dylib")
    parser.add_argument("--rows", type=int, default=None,
                        help="Number of rows (default: 2M full, 500K quick)")
    parser.add_argument("--cols", type=int, default=100,
                        help="Total columns (default: 100)")
    parser.add_argument("--hot-cols", type=int, default=10,
                        help="Number of hot columns (default: 10)")
    parser.add_argument("--data-type", choices=["int", "mixed"], default="mixed",
                        help="Column data type pattern")
    parser.add_argument("--cool-sec", type=int, default=None,
                        help="Cooldown seconds (default: 15 quick, 30 full)")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--output-dir", default=None,
                        help="Save JSON results to this directory")
    args = parser.parse_args()

    # Check pandas is installed
    try:
        subprocess.check_call(
            [sys.executable, "-c", "import pandas, numpy"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        print("ERROR: pandas and numpy required. Install with:")
        print("  pip install pandas numpy")
        sys.exit(1)

    if args.rows:
        num_rows = args.rows
    elif args.quick:
        num_rows = 500_000
    else:
        num_rows = 2_000_000

    if args.cool_sec is not None:
        cool_sec = args.cool_sec
    elif args.quick:
        cool_sec = 15
    else:
        cool_sec = 30

    # Find libsmash
    build_dir = Path(".").resolve()
    lib_suffix = ".dylib" if sys.platform == "darwin" else ".so"
    if args.smash_lib:
        smash_lib = Path(args.smash_lib).resolve()
    else:
        smash_lib = build_dir / f"libsmash{lib_suffix}"
        if not smash_lib.exists():
            print(f"WARNING: {smash_lib} not found, will only run baseline")
            smash_lib = None

    all_results = {}
    configs = [
        ("baseline", None),
    ]  # type: list[tuple[str, str | None]]
    if smash_lib:
        configs.append(("smash", str(smash_lib)))

    for config_name, lib in configs:
        print(f"\n{'='*60}")
        print(f"  Pandas - {config_name}")
        print(f"{'='*60}")

        run_results = []
        for run_idx in range(args.runs):
            if args.runs > 1:
                print(f"\n  --- Run {run_idx+1}/{args.runs} ---")

            metrics = run_pandas_benchmark(
                smash_lib=lib, num_rows=num_rows, num_cols=args.cols,
                hot_cols=args.hot_cols, cool_sec=cool_sec,
                data_type=args.data_type,
            )
            if metrics:
                run_results.append(metrics)

        if run_results:
            median_metrics = {}
            for key in ["peak_rss_mb", "min_rss_mb", "cool_rss_mb",
                        "rss_reduction_pct", "auc_mb_sec",
                        "hot_phase_time_sec", "cold_access_time_sec",
                        "warm_access_time_sec", "cold_access_overhead_pct",
                        "ext_peak_rss_mb", "ext_min_rss_mb", "ext_rss_reduction_pct",
                        "data_size_mb"]:
                vals = sorted(r[key] for r in run_results if key in r)
                if vals:
                    median_metrics[key] = vals[len(vals) // 2]

            all_results[config_name] = {
                "median": median_metrics,
                "runs": run_results,
            }

    # Print summary
    print(f"\n{'='*60}")
    print("  PANDAS COLD-COLUMNS BENCHMARK RESULTS")
    print(f"{'='*60}")
    print(f"  Config: {num_rows} rows x {args.cols} cols, {args.hot_cols} hot")

    for config_name, data in all_results.items():
        m = data["median"]
        print(f"\n  {config_name}:")
        print(f"    Data size:     {m.get('data_size_mb', 0):.0f} MB")
        print(f"    Peak RSS:      {m.get('peak_rss_mb', 0):.0f} MB")
        print(f"    Min RSS:       {m.get('min_rss_mb', 0):.0f} MB")
        print(f"    Reduction:     {m.get('rss_reduction_pct', 0):.1f}%")
        print(f"    Cold access:   {m.get('cold_access_time_sec', 0):.3f}s "
              f"(warm: {m.get('warm_access_time_sec', 0):.3f}s)")
        if "ext_rss_reduction_pct" in m:
            print(f"    Ext reduction: {m['ext_rss_reduction_pct']:.1f}% "
                  f"(external RSS measurement)")

    if "baseline" in all_results and "smash" in all_results:
        b = all_results["baseline"]["median"]
        s = all_results["smash"]["median"]
        base_peak = b.get("peak_rss_mb", 0)
        smash_min = s.get("min_rss_mb", 0)
        if base_peak > 0 and smash_min > 0:
            vs_base = (1 - smash_min / base_peak) * 100
            print(f"\n  Smash min vs baseline peak: {vs_base:.1f}% RSS reduction")

        base_cold = b.get("cold_access_time_sec", 0)
        smash_cold = s.get("cold_access_time_sec", 0)
        if base_cold > 0:
            print(f"  Cold access overhead: {((smash_cold/base_cold)-1)*100:+.1f}%")

    # Save results only if --output-dir was explicitly given
    if args.output_dir:
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        out_file = output_dir / "pandas_results.json"
        with open(out_file, "w") as f:
            json.dump(all_results, f, indent=2)
        print(f"\n  Results saved to {out_file}")


if __name__ == "__main__":
    main()
