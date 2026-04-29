#!/usr/bin/env python3
"""Firefox browsing benchmark for Smash.

Builds Firefox with --disable-jemalloc (so system malloc is used),
launches it headless with LD_PRELOAD=libsmash.so, drives browsing
via Selenium, and measures aggregate RSS across all Firefox processes.

Usage:
    cd build
    python3 ../bench/bench_firefox.py [--quick] [--skip-build]

Prerequisites (installed by setup_ec2_new_benchmarks.sh):
    - Firefox source tree at ~/firefox-source (or --firefox-dir)
    - geckodriver on PATH
    - Python packages: selenium
    - Xvfb (for headless X)
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


# ── RSS helpers ──────────────────────────────────────────────────────────────

def get_process_tree_rss_mb(root_pid):
    """Get total RSS of a process and all its descendants (MiB)."""
    try:
        # Get all PIDs in the process tree
        out = subprocess.check_output(
            ["ps", "--ppid", str(root_pid), "-o", "pid="],
            text=True, stderr=subprocess.DEVNULL
        ).strip()
        children = [int(p) for p in out.split() if p.strip()]
    except (subprocess.CalledProcessError, ValueError):
        children = []

    total_rss = _get_single_rss_kb(root_pid)
    for child in children:
        total_rss += _get_single_rss_kb(child)
        # Also get grandchildren
        total_rss += sum(_get_single_rss_kb(gc) for gc in _get_all_descendants(child))
    return total_rss / 1024.0


def _get_single_rss_kb(pid):
    """Get RSS of a single process in KiB."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        pass
    return 0


# Recursive version that sums the whole tree
def get_tree_rss_mb(root_pid):
    """Sum RSS of root_pid + all descendants via /proc."""
    pids = _get_all_descendants(root_pid)
    pids.append(root_pid)
    total_kb = sum(_get_single_rss_kb(p) for p in pids)
    return total_kb / 1024.0


def _get_all_descendants(pid):
    """Get all descendant PIDs recursively."""
    children = []
    try:
        out = subprocess.check_output(
            ["ps", "--ppid", str(pid), "-o", "pid="],
            text=True, stderr=subprocess.DEVNULL
        ).strip()
        for p in out.split():
            p = p.strip()
            if p:
                child_pid = int(p)
                children.append(child_pid)
                children.extend(_get_all_descendants(child_pid))
    except (subprocess.CalledProcessError, ValueError):
        pass
    return children


def count_firefox_processes(root_pid):
    """Count how many processes are in the Firefox tree."""
    return len(_get_all_descendants(root_pid)) + 1


# ── Firefox build ────────────────────────────────────────────────────────────

def build_firefox(source_dir, jobs=None):
    """Build Firefox with jemalloc disabled."""
    source_dir = Path(source_dir)
    mozconfig = source_dir / "mozconfig"

    # Write mozconfig that disables jemalloc
    mozconfig.write_text(
        "# Smash benchmark: disable jemalloc so LD_PRELOAD works\n"
        "ac_add_options --disable-jemalloc\n"
        "ac_add_options --enable-optimize\n"
        "ac_add_options --disable-debug\n"
        "ac_add_options --disable-tests\n"
        "ac_add_options --without-wasm-sandboxed-libraries\n"
    )

    j = jobs or os.cpu_count() or 4
    print(f"Building Firefox (--disable-jemalloc) with {j} jobs...")
    print(f"  Source: {source_dir}")

    r = subprocess.run(
        ["./mach", "build", f"-j{j}"],
        cwd=str(source_dir),
        timeout=7200,  # 2 hours max
    )
    if r.returncode != 0:
        print("ERROR: Firefox build failed")
        sys.exit(1)

    # Find the built firefox binary
    obj_dir = _find_obj_dir(source_dir)
    ff_bin = obj_dir / "dist" / "bin" / "firefox"
    if not ff_bin.exists():
        print(f"ERROR: firefox binary not found at {ff_bin}")
        sys.exit(1)

    print(f"  Built: {ff_bin}")
    return str(ff_bin)


def _find_obj_dir(source_dir):
    """Find the objdir from the Firefox build."""
    # mach stores it; also check common patterns
    for pattern in ["obj-*"]:
        matches = sorted(Path(source_dir).glob(pattern))
        if matches:
            return matches[-1]
    # Fallback
    return source_dir / "obj-x86_64-pc-linux-gnu"


def find_firefox_binary(source_dir):
    """Find an already-built firefox binary."""
    obj_dir = _find_obj_dir(Path(source_dir))
    ff_bin = obj_dir / "dist" / "bin" / "firefox"
    if ff_bin.exists():
        return str(ff_bin)
    return None


# ── URLs to browse ───────────────────────────────────────────────────────────

# Mix of content-heavy pages (large DOM, images, JS state)
BROWSE_URLS = [
    "https://en.wikipedia.org/wiki/Memory_management",
    "https://en.wikipedia.org/wiki/Garbage_collection_(computer_science)",
    "https://en.wikipedia.org/wiki/Virtual_memory",
    "https://en.wikipedia.org/wiki/Cache_(computing)",
    "https://en.wikipedia.org/wiki/Operating_system",
    "https://en.wikipedia.org/wiki/Linux_kernel",
    "https://en.wikipedia.org/wiki/Web_browser",
    "https://en.wikipedia.org/wiki/Firefox",
    "https://en.wikipedia.org/wiki/Compiler",
    "https://en.wikipedia.org/wiki/Database",
    "https://en.wikipedia.org/wiki/Distributed_computing",
    "https://en.wikipedia.org/wiki/Machine_learning",
    "https://en.wikipedia.org/wiki/Neural_network_(machine_learning)",
    "https://en.wikipedia.org/wiki/Cloud_computing",
    "https://en.wikipedia.org/wiki/Computer_architecture",
    "https://en.wikipedia.org/wiki/Instruction_set_architecture",
    "https://en.wikipedia.org/wiki/CPU_cache",
    "https://en.wikipedia.org/wiki/Solid-state_drive",
    "https://en.wikipedia.org/wiki/Programming_language",
    "https://en.wikipedia.org/wiki/Rust_(programming_language)",
]

QUICK_URLS = BROWSE_URLS[:8]


# ── Benchmark driver ─────────────────────────────────────────────────────────

def run_benchmark(firefox_bin, smash_lib=None, urls=None, cool_sec=30,
                  revisit_count=5, profile_dir=None):
    """Run one Firefox benchmark pass. Returns dict of metrics."""
    if urls is None:
        urls = BROWSE_URLS

    env = os.environ.copy()
    env["MOZ_HEADLESS"] = "1"
    env["DISPLAY"] = ":99"  # Xvfb

    if smash_lib:
        env["LD_PRELOAD"] = str(smash_lib)
        # Give Smash time-based config for quicker cold detection
        env["SMASH_VERY_COLD_TICKS"] = "5"

    # Create a temporary profile to avoid interference
    import tempfile
    if profile_dir is None:
        profile_tmp = tempfile.mkdtemp(prefix="ff_smash_")
        profile_dir = profile_tmp
    else:
        profile_tmp = None

    print(f"  Starting Firefox (smash={'yes' if smash_lib else 'no'})...")

    try:
        from selenium import webdriver
        from selenium.webdriver.firefox.options import Options
        from selenium.webdriver.firefox.service import Service
    except ImportError:
        print("    ERROR: selenium not installed (pip install selenium)")
        return None

    # If using Smash, create a wrapper script so LD_PRELOAD only applies to
    # Firefox, NOT to geckodriver (which would break Marionette IPC).
    wrapper_script = None
    actual_binary = firefox_bin
    if smash_lib:
        import tempfile
        wrapper_script = tempfile.NamedTemporaryFile(
            mode="w", suffix=".sh", prefix="ff_smash_", delete=False
        )
        wrapper_script.write(f"""#!/bin/bash
export LD_PRELOAD={smash_lib}
export SMASH_VERY_COLD_TICKS=${{SMASH_VERY_COLD_TICKS:-5}}
exec {firefox_bin} "$@"
""")
        wrapper_script.close()
        os.chmod(wrapper_script.name, 0o755)
        actual_binary = wrapper_script.name

    options = Options()
    options.binary_location = actual_binary
    options.add_argument("--headless")

    # Don't pass LD_PRELOAD to geckodriver — only Firefox gets it via the wrapper
    clean_env = {k: v for k, v in env.items() if k != "LD_PRELOAD"}
    service = Service(env=clean_env)
    driver = None
    try:
        driver = webdriver.Firefox(options=options, service=service)

        # Find the Firefox PID (Selenium's managed process)
        ff_pid = driver.service.process.pid
        # The actual browser is a child of geckodriver
        time.sleep(3)
        # Find the real firefox parent PID
        browser_pids = _find_firefox_pids()
        if not browser_pids:
            print("    WARNING: Could not find Firefox process tree")
            ff_pid = driver.service.process.pid
        else:
            ff_pid = browser_pids[0]  # parent firefox process

        print(f"    Firefox PID: {ff_pid} ({count_firefox_processes(ff_pid)} processes)")

        # ── Phase 1: Open tabs ───────────────────────────────────────
        print(f"    Opening {len(urls)} tabs...")
        rss_timeline = []

        # Open first URL in current tab
        driver.get(urls[0])
        time.sleep(2)

        # Open remaining URLs in new tabs
        for i, url in enumerate(urls[1:], 1):
            driver.execute_script(f"window.open('{url}', '_blank');")
            time.sleep(1.5)  # Let page load
            rss = get_tree_rss_mb(ff_pid)
            rss_timeline.append(rss)
            if (i + 1) % 5 == 0:
                nproc = count_firefox_processes(ff_pid)
                print(f"      {i+1}/{len(urls)} tabs open, RSS={rss:.0f} MB, {nproc} procs")

        # Let everything settle
        time.sleep(5)
        peak_rss = get_tree_rss_mb(ff_pid)
        nproc_peak = count_firefox_processes(ff_pid)
        rss_timeline.append(peak_rss)
        print(f"    Peak RSS after all tabs: {peak_rss:.0f} MB ({nproc_peak} processes)")

        # ── Phase 2: Cool-down (no interaction) ──────────────────────
        print(f"    Cooling for {cool_sec}s...")
        for i in range(cool_sec):
            time.sleep(1)
            rss = get_tree_rss_mb(ff_pid)
            rss_timeline.append(rss)
            if (i + 1) % 10 == 0:
                print(f"      t={i+1}s, RSS={rss:.0f} MB")

        min_rss = min(rss_timeline[-cool_sec:]) if cool_sec > 0 else peak_rss
        cool_rss = get_tree_rss_mb(ff_pid)
        print(f"    After cooling: RSS={cool_rss:.0f} MB (min={min_rss:.0f} MB)")

        # ── Phase 3: Revisit some tabs ───────────────────────────────
        handles = driver.window_handles
        revisit_n = min(revisit_count, len(handles))
        print(f"    Revisiting {revisit_n} tabs...")

        revisit_start = time.time()
        for i in range(revisit_n):
            driver.switch_to.window(handles[i])
            # Scroll to trigger decompression of layout/image data
            driver.execute_script("window.scrollTo(0, document.body.scrollHeight / 2);")
            time.sleep(0.5)
        revisit_elapsed = time.time() - revisit_start

        serve_rss = get_tree_rss_mb(ff_pid)
        rss_timeline.append(serve_rss)
        print(f"    After revisit: RSS={serve_rss:.0f} MB, took {revisit_elapsed:.1f}s")

        # ── Compute metrics ──────────────────────────────────────────
        reduction = (1 - min_rss / peak_rss) * 100 if peak_rss > 0 else 0
        auc = sum(rss_timeline)

        metrics = {
            "peak_rss_mb": peak_rss,
            "min_rss_mb": min_rss,
            "cool_rss_mb": cool_rss,
            "serve_rss_mb": serve_rss,
            "rss_reduction_pct": reduction,
            "rss_timeline": rss_timeline,
            "auc_mb_sec": auc,
            "num_tabs": len(urls),
            "num_processes": nproc_peak,
            "revisit_time_sec": revisit_elapsed,
            "cool_sec": cool_sec,
        }

        return metrics

    except Exception as e:
        print(f"    ERROR: {e}")
        import traceback
        traceback.print_exc()
        return None

    finally:
        if driver:
            try:
                driver.quit()
            except Exception:
                pass
        # Kill any remaining firefox processes
        _kill_all_firefox()
        # Clean up temp files
        if wrapper_script:
            try:
                os.unlink(wrapper_script.name)
            except OSError:
                pass
        if profile_tmp:
            subprocess.run(["rm", "-rf", profile_tmp], capture_output=True)


def _find_firefox_pids():
    """Find Firefox main process PIDs."""
    try:
        out = subprocess.check_output(
            ["pgrep", "-f", "firefox.*-contentproc", "--oldest"],
            text=True, stderr=subprocess.DEVNULL
        ).strip()
    except subprocess.CalledProcessError:
        pass

    # Broader search: find parent firefox process
    try:
        out = subprocess.check_output(
            ["pgrep", "-x", "firefox"],
            text=True, stderr=subprocess.DEVNULL
        ).strip()
        return [int(p) for p in out.split() if p.strip()]
    except (subprocess.CalledProcessError, ValueError):
        return []


def _kill_all_firefox():
    """Kill all Firefox processes."""
    subprocess.run(["pkill", "-9", "firefox"], capture_output=True)
    subprocess.run(["pkill", "-9", "geckodriver"], capture_output=True)
    time.sleep(1)


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Firefox browsing benchmark for Smash")
    parser.add_argument("--quick", action="store_true",
                        help="Fewer tabs, shorter cool-down")
    parser.add_argument("--skip-build", action="store_true",
                        help="Use already-built Firefox")
    parser.add_argument("--firefox-dir", default=os.path.expanduser("~/firefox-source"),
                        help="Firefox source directory")
    parser.add_argument("--firefox-bin", default=None,
                        help="Path to built firefox binary (overrides --firefox-dir)")
    parser.add_argument("--smash-lib", default=None,
                        help="Path to libsmash.so (default: auto-detect from build dir)")
    parser.add_argument("--runs", type=int, default=1,
                        help="Number of runs per config")
    parser.add_argument("--output-dir", default="paper_results",
                        help="Output directory")
    args = parser.parse_args()

    # Find/build Firefox
    if args.firefox_bin:
        ff_bin = args.firefox_bin
    elif args.skip_build:
        ff_bin = find_firefox_binary(args.firefox_dir)
        if not ff_bin:
            print(f"ERROR: No built Firefox found in {args.firefox_dir}")
            sys.exit(1)
    else:
        ff_bin = build_firefox(args.firefox_dir)

    print(f"Firefox binary: {ff_bin}")

    # Find libsmash.so
    build_dir = Path(".").resolve()
    if args.smash_lib:
        smash_lib = Path(args.smash_lib).resolve()
    else:
        smash_lib = build_dir / "libsmash.so"
        if not smash_lib.exists():
            print(f"WARNING: {smash_lib} not found, will only run baseline")
            smash_lib = None

    # Ensure Xvfb is running
    _ensure_xvfb()

    # Benchmark params
    if args.quick:
        urls = QUICK_URLS
        cool_sec = 15
        revisit_count = 3
    else:
        urls = BROWSE_URLS
        cool_sec = 45
        revisit_count = 5

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    all_results = {}

    configs = [
        ("baseline", None),
    ]  # type: list[tuple[str, str | None]]
    if smash_lib:
        configs.append(("smash", str(smash_lib)))

    for config_name, lib in configs:
        print(f"\n{'='*60}")
        print(f"  Firefox - {config_name}")
        print(f"{'='*60}")

        run_results = []
        for run_idx in range(args.runs):
            if args.runs > 1:
                print(f"\n  --- Run {run_idx+1}/{args.runs} ---")

            _kill_all_firefox()
            time.sleep(2)

            metrics = run_benchmark(
                ff_bin, smash_lib=lib, urls=urls,
                cool_sec=cool_sec, revisit_count=revisit_count,
            )
            if metrics:
                run_results.append(metrics)
                print(f"    RSS: {metrics['peak_rss_mb']:.0f} → {metrics['min_rss_mb']:.0f} MB "
                      f"({metrics['rss_reduction_pct']:.1f}% reduction)")

        if run_results:
            # Compute median of key metrics
            median_metrics = {}
            for key in ["peak_rss_mb", "min_rss_mb", "cool_rss_mb", "serve_rss_mb",
                        "rss_reduction_pct", "auc_mb_sec", "revisit_time_sec"]:
                vals = [r[key] for r in run_results if key in r]
                if vals:
                    vals.sort()
                    median_metrics[key] = vals[len(vals) // 2]

            all_results[config_name] = {
                "median": median_metrics,
                "runs": run_results,
            }

    # Print summary
    print(f"\n{'='*60}")
    print("  FIREFOX BENCHMARK RESULTS")
    print(f"{'='*60}")
    for config_name, data in all_results.items():
        m = data["median"]
        print(f"  {config_name:12s}: peak={m.get('peak_rss_mb',0):.0f} MB, "
              f"min={m.get('min_rss_mb',0):.0f} MB, "
              f"reduction={m.get('rss_reduction_pct',0):.1f}%, "
              f"revisit={m.get('revisit_time_sec',0):.1f}s")

    if "baseline" in all_results and "smash" in all_results:
        b = all_results["baseline"]["median"]
        s = all_results["smash"]["median"]
        base_peak = b.get("peak_rss_mb", 0)
        smash_min = s.get("min_rss_mb", 0)
        if base_peak > 0:
            vs_base = (1 - smash_min / base_peak) * 100
            print(f"\n  Smash min vs baseline peak: {vs_base:.1f}% reduction")

    # Save results
    out_file = output_dir / "firefox_results.json"
    with open(out_file, "w") as f:
        # Remove timeline arrays for cleaner JSON (they're large)
        save_results = {}
        for k, v in all_results.items():
            save_results[k] = {
                "median": v["median"],
                "runs": [{kk: vv for kk, vv in run.items() if kk != "rss_timeline"}
                         for run in v["runs"]],
            }
        json.dump(save_results, f, indent=2)
    print(f"\n  Results saved to {out_file}")

    # Print METRIC lines for integration with run_paper_experiments.py
    if "smash" in all_results:
        m = all_results["smash"]["median"]
    elif "baseline" in all_results:
        m = all_results["baseline"]["median"]
    else:
        m = {}

    for key, val in m.items():
        if isinstance(val, (int, float)):
            print(f"METRIC {key} {val}")


def _ensure_xvfb():
    """Start Xvfb if not already running."""
    try:
        subprocess.check_output(["pgrep", "Xvfb"], text=True, stderr=subprocess.DEVNULL)
        return  # Already running
    except subprocess.CalledProcessError:
        pass

    print("Starting Xvfb on :99...")
    subprocess.Popen(
        ["Xvfb", ":99", "-screen", "0", "1920x1080x24"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1)


if __name__ == "__main__":
    main()
