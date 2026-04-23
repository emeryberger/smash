#!/usr/bin/env python3
"""Run all paper experiments: ablation (all 5 apps) + compress-only.

Usage:
    cd build
    python3 ../bench/run_paper_experiments.py [--quick] [--ablation-only] [--compress-only-only]

Produces paper_results/ with:
  - ablation_results.json
  - compress_only_results.json
  - paper_tables.txt (ready to paste into evaluation.tex)
"""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from collections import OrderedDict
from pathlib import Path

# ── CMake ablation variables ─────────────────────────────────────────────────
ALL_ABLATION_VARS = [
    "SMASH_NUM_ARENAS", "SMASH_COLD_TICKS", "SMASH_VERY_COLD_TICKS",
    "SMASH_DICT_TRAIN_SAMPLES", "SMASH_PREFETCH_WINDOW",
    "SMASH_COMPRESSOR_WORKERS", "SMASH_COMPRESS_STORE_SHARDS",
    "SMASH_LARGE_ALLOC_VM_THRESHOLD",
    "SMASH_ABLATION_NO_ZERO_EAGER", "SMASH_ABLATION_NO_ZERO_DEFERRED",
    "SMASH_ABLATION_NO_SKIP_STATS", "SMASH_ABLATION_NO_CHUNK_BITMAP",
    "SMASH_ABLATION_NO_CALLSITE_ARENA",
    # T3 series: reference-behavior homogeneity experiments (Apr 2026)
    "SMASH_MAX_SLOTS_PER_PAGE", "SMASH_COLD_ARENA_FEEDBACK",
    "SMASH_COLD_ARENA_THRESHOLD", "SMASH_PAGE_LOCAL_BATCH",
    "SMASH_THREAD_ARENA_HASH",
    "SMASH_ADAPTIVE_CAP", "SMASH_ADAPTIVE_CAP_TARGET_PCT",
    "SMASH_ADAPTIVE_CAP_MIN", "SMASH_ADAPTIVE_CAP_MIN_SAMPLES",
    "SMASH_MEASURE_COHORTS",
]

# ── Ablation configs matching paper table ────────────────────────────────────
ABLATION_CONFIGS = OrderedDict([
    ("B1", {"name": "Default", "cmake_flags": {}, "use_smash": True}),
    ("B0", {"name": "System malloc", "cmake_flags": {}, "use_smash": False}),
    ("MESH", {"name": "Mesh", "cmake_flags": {}, "use_smash": False, "use_mesh": True}),
    ("DICT", {"name": "With dicts",
              "cmake_flags": {"SMASH_DICT_TRAIN_SAMPLES": "16"}, "use_smash": True}),
    ("T1a", {"name": "No arenas",
             "cmake_flags": {"SMASH_NUM_ARENAS": "1"}, "use_smash": True}),
    ("T1c", {"name": "Fast tier only",
             "cmake_flags": {"SMASH_VERY_COLD_TICKS": "9999"}, "use_smash": True}),
    ("T2a", {"name": "No zero-deferred",
             "cmake_flags": {"SMASH_ABLATION_NO_ZERO_DEFERRED": "ON"}, "use_smash": True}),
    ("T1e", {"name": "No prefetch",
             "cmake_flags": {"SMASH_PREFETCH_WINDOW": "0"}, "use_smash": True}),
    ("T1f", {"name": "Single worker",
             "cmake_flags": {"SMASH_COMPRESSOR_WORKERS": "1"}, "use_smash": True}),
    ("B2", {"name": "No compression",
            "cmake_flags": {"SMASH_COLD_TICKS": "9999"}, "use_smash": True}),
    # T3 series: reference-behavior homogeneity (design memo, Apr 2026)
    # Each measures one lever in isolation on top of the B1 default.
    # C1 cap target: 8 live objects per page → P(page cold) ≈ (1-q)^8.
    # At q=0.1 that's 43% (vs ~0.001% for the default 64+ objects/page).
    ("T3a", {"name": "Cap 8 slots/page (global)",
             "cmake_flags": {"SMASH_MAX_SLOTS_PER_PAGE": "8"}, "use_smash": True}),
    ("T3b", {"name": "Page-local batch",
             "cmake_flags": {"SMASH_PAGE_LOCAL_BATCH": "ON"}, "use_smash": True}),
    ("T3c", {"name": "Cold arenas + cap 8",
             "cmake_flags": {"SMASH_COLD_ARENA_FEEDBACK": "ON",
                             "SMASH_MAX_SLOTS_PER_PAGE": "8"}, "use_smash": True}),
    ("T3d", {"name": "All three (B1+T3a+T3b+T3c)",
             "cmake_flags": {"SMASH_COLD_ARENA_FEEDBACK": "ON",
                             "SMASH_MAX_SLOTS_PER_PAGE": "8",
                             "SMASH_PAGE_LOCAL_BATCH": "ON"}, "use_smash": True}),
    # A2-lite: thread identity in arena hash (cheap per-thread separation).
    ("T3e", {"name": "Thread arena hash",
             "cmake_flags": {"SMASH_THREAD_ARENA_HASH": "ON"}, "use_smash": True}),
    # C1b: adaptive cap driven by (compress, decompress) feedback.  q̂ =
    # decomp / (comp + decomp); N = floor(log(p_target)/log(1-q̂)) with
    # a floor of kAdaptiveCapMin.  Hot buckets (q̂ >= 0.30) disable cap.
    ("T3f", {"name": "Adaptive cap (feedback)",
             "cmake_flags": {"SMASH_ADAPTIVE_CAP": "ON"}, "use_smash": True}),
    # T4 series: isolation experiments (PLAN4.md, Apr 2026).
    # T4b: widen kNumArenas from 4 -> 16 to reduce call-site collisions on a
    # shared partial span.  Tests M2a (the cheapest isolation lever) on top
    # of the T3f adaptive cap.
    ("T4b", {"name": "Adaptive cap + 16 arenas",
             "cmake_flags": {"SMASH_ADAPTIVE_CAP": "ON",
                             "SMASH_NUM_ARENAS": "16"}, "use_smash": True}),
    # Combination experiments: explore pairwise/triple combos of winners
    ("T5a", {"name": "ThreadHash + PageLocal",
             "cmake_flags": {"SMASH_THREAD_ARENA_HASH": "ON",
                             "SMASH_PAGE_LOCAL_BATCH": "ON"}, "use_smash": True}),
    ("T5b", {"name": "ThreadHash + ColdArena",
             "cmake_flags": {"SMASH_THREAD_ARENA_HASH": "ON",
                             "SMASH_COLD_ARENA_FEEDBACK": "ON"}, "use_smash": True}),
    ("T5c", {"name": "ThreadHash + PageLocal + ColdArena",
             "cmake_flags": {"SMASH_THREAD_ARENA_HASH": "ON",
                             "SMASH_PAGE_LOCAL_BATCH": "ON",
                             "SMASH_COLD_ARENA_FEEDBACK": "ON"}, "use_smash": True}),
    ("T5d", {"name": "ThreadHash + 16 arenas",
             "cmake_flags": {"SMASH_THREAD_ARENA_HASH": "ON",
                             "SMASH_NUM_ARENAS": "16"}, "use_smash": True}),
    ("T5e", {"name": "ThreadHash + PageLocal + 16 arenas",
             "cmake_flags": {"SMASH_THREAD_ARENA_HASH": "ON",
                             "SMASH_PAGE_LOCAL_BATCH": "ON",
                             "SMASH_NUM_ARENAS": "16"}, "use_smash": True}),
    ("T5f", {"name": "PageLocal + ColdArena",
             "cmake_flags": {"SMASH_PAGE_LOCAL_BATCH": "ON",
                             "SMASH_COLD_ARENA_FEEDBACK": "ON"}, "use_smash": True}),
    ("T5g", {"name": "ColdArena only (no cap)",
             "cmake_flags": {"SMASH_COLD_ARENA_FEEDBACK": "ON"}, "use_smash": True}),
])

APPS = ["sqlite", "rocksdb", "duckdb", "memcached", "redis", "redis_ext",
        "redis_patched", "redis_ext_patched", "pandas"]

IS_DARWIN = platform.system() == "Darwin"
PRELOAD_VAR = "DYLD_INSERT_LIBRARIES" if IS_DARWIN else "LD_PRELOAD"
LIB_SUFFIX = ".dylib" if IS_DARWIN else ".so"
MESH_LIB = "/usr/local/lib/libmesh.dylib" if IS_DARWIN else "/usr/lib/libmesh.so"


# ── Provenance ───────────────────────────────────────────────────────────────
#
# Every results JSON records (a) who we are (host, OS, CPU, RAM, page size)
# and (b) what code produced each data point (git commit, uncommitted diff
# indicator, source tree hash, cmake flags).  This exists because a previous
# session had apparently-comparable JSON files that were actually taken
# under different Redis/bench conditions, and we spent time debugging a
# phantom regression.  Never again.

_SYSTEM_INFO_CACHE = None

def collect_system_info():
    """Capture host/OS/CPU/memory/tool-version info that won't change
    between runs within a session."""
    global _SYSTEM_INFO_CACHE
    if _SYSTEM_INFO_CACHE is not None:
        return _SYSTEM_INFO_CACHE

    def run(cmd):
        try:
            out = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT,
                                          timeout=5)
            return out.strip()
        except Exception:
            return None

    info = {
        "timestamp_utc": datetime.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "hostname": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "page_size_bytes": os.sysconf("SC_PAGESIZE") if hasattr(os, "sysconf") else None,
    }

    if IS_DARWIN:
        info["cpu"] = run(["sysctl", "-n", "machdep.cpu.brand_string"])
        info["cpu_cores"] = run(["sysctl", "-n", "hw.ncpu"])
        mem_raw = run(["sysctl", "-n", "hw.memsize"])
        if mem_raw:
            try:
                info["mem_gib"] = round(int(mem_raw) / (1024 ** 3), 1)
            except Exception:
                pass
    else:
        cpuinfo = run(["grep", "-m1", "model name", "/proc/cpuinfo"])
        if cpuinfo and ":" in cpuinfo:
            info["cpu"] = cpuinfo.split(":", 1)[1].strip()
        cores = run(["grep", "-c", "^processor", "/proc/cpuinfo"])
        if cores:
            info["cpu_cores"] = cores
        mem_kb = run(["grep", "MemTotal", "/proc/meminfo"])
        if mem_kb:
            try:
                kb = int(mem_kb.split()[1])
                info["mem_gib"] = round(kb / (1024 ** 2), 1)
            except Exception:
                pass

    # Tool versions: record whatever is on PATH so we know what we linked
    # against.  Redis/memcached binaries in the build tree override system
    # ones; the get_binary helper picks those where available.
    for tool in ["cmake", "gcc", "g++", "clang", "clang++"]:
        v = run([tool, "--version"])
        if v:
            info[f"{tool}_version"] = v.splitlines()[0]
    info["redis_server_version"] = run(["redis-server", "--version"]) or None
    info["memcached_version"] = run(["memcached", "--version"]) or None

    _SYSTEM_INFO_CACHE = info
    return info


def _file_hash(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()[:16]
    except Exception:
        return None


def collect_source_hash(source_dir):
    """Hash the Smash source tree (src/, include/, CMakeLists.txt).
    Stable across unrelated filesystem operations; detects uncommitted
    changes that a git commit hash alone would miss."""
    source_dir = Path(source_dir)
    files = []
    for sub in ("src", "include"):
        p = source_dir / sub
        if p.exists():
            files.extend(sorted(p.rglob("*.h")))
            files.extend(sorted(p.rglob("*.cpp")))
    top_cmake = source_dir / "CMakeLists.txt"
    if top_cmake.exists():
        files.append(top_cmake)

    combined = hashlib.sha256()
    for f in files:
        rel = f.relative_to(source_dir).as_posix()
        fh = _file_hash(f)
        if fh is None:
            continue
        combined.update(rel.encode("utf-8"))
        combined.update(fh.encode("ascii"))
    return combined.hexdigest()[:16]


def collect_git_info(source_dir):
    """Git HEAD hash + whether the tree has uncommitted changes."""
    try:
        head = subprocess.check_output(
            ["git", "-C", str(source_dir), "rev-parse", "HEAD"],
            text=True, stderr=subprocess.DEVNULL, timeout=5
        ).strip()
    except Exception:
        return {"git_head": None, "git_dirty": None}
    dirty = False
    try:
        st = subprocess.check_output(
            ["git", "-C", str(source_dir), "status", "--porcelain"],
            text=True, stderr=subprocess.DEVNULL, timeout=5
        )
        dirty = bool(st.strip())
    except Exception:
        dirty = None
    return {"git_head": head, "git_dirty": dirty}


def collect_smash_env():
    """Snapshot every SMASH_* environment variable at the point of call.
    Individual bench functions may set additional SMASH_* variables inside
    their own env copies; this captures the baseline visible to the runner."""
    return {k: v for k, v in os.environ.items() if k.startswith("SMASH_")}


def build_provenance(source_dir, cmake_flags, lib_path):
    """Per-build provenance.  Call after each rebuild; the result is stamped
    into every result entry produced with that build."""
    p = {
        "cmake_flags": dict(cmake_flags) if cmake_flags else {},
        "smash_env": collect_smash_env(),
        "source_hash": collect_source_hash(source_dir),
        "libsmash_sha256": _file_hash(lib_path) if lib_path and Path(lib_path).exists() else None,
        "libsmash_mtime": (
            datetime.datetime.utcfromtimestamp(
                Path(lib_path).stat().st_mtime).isoformat(timespec="seconds") + "Z"
            if lib_path and Path(lib_path).exists() else None
        ),
    }
    p.update(collect_git_info(source_dir))
    return p


# Per-app bench parameters as actually used by the run_* functions below.
# Change these in lockstep with the code or the measurements become lies.
# Included in every results JSON so callers know *what* was measured.
BENCH_PARAMS = {
    "sqlite": {
        "quick": {"rows": None, "note": "passes --quick to bench_sqlite"},
        "full":  {"rows": None, "note": "default bench_sqlite args"},
    },
    "rocksdb": {
        "quick": {"keys": 200000, "value_size": 256, "cool_sec": 5, "serve_sec": 10},
        "full":  {"keys": 200000, "value_size": 256, "cool_sec": 5, "serve_sec": 10},
    },
    "redis": {
        "quick": {"port": 16399, "num_ops": 50000, "num_clients": 1000,
                  "value_size": 1000, "keyspace": 100000, "cool_sec": 5},
        "full":  {"port": 16399, "num_ops": 200000,
                  "num_clients_linux": 50, "num_clients_darwin": 5000,
                  "value_size": 2000, "keyspace": 200000, "cool_sec": 20,
                  "server_flags": ["--hz", "1", "--dynamic-hz", "no",
                                   "--save", "", "--appendonly", "no"]},
    },
    "redis_ext": {"full": {"port": 16400, "note": "extended: SET + DELETE 50% + GET"}},
    "redis_patched":     {"full": {"uses": "redis-smash patched binary"}},
    "redis_ext_patched": {"full": {"uses": "redis-smash patched binary, ext workload"}},
    "memcached": {
        "full": {"num_items": 200000, "value_size": 1024, "cool_sec": 20},
    },
    "duckdb": {
        "full": {"note": "TPC-H queries, see run_duckdb_bench"},
    },
    "pandas": {
        "full": {"note": "see bench/bench_pandas.py"},
    },
}


# ── Build helpers ────────────────────────────────────────────────────────────

def rebuild(build_dir, cmake_flags, source_dir):
    """Reconfigure and rebuild libsmash with given flags.  Returns the
    per-build provenance record on success, or None on failure."""
    # Preserve compiler settings from the original build to avoid C++20 issues
    cache_file = Path(build_dir) / "CMakeCache.txt"
    compiler_flags = []
    if cache_file.exists():
        with open(cache_file) as f:
            for line in f:
                if line.startswith("CMAKE_C_COMPILER:"):
                    compiler_flags.append(f"-DCMAKE_C_COMPILER={line.split('=')[1].strip()}")
                elif line.startswith("CMAKE_CXX_COMPILER:"):
                    compiler_flags.append(f"-DCMAKE_CXX_COMPILER={line.split('=')[1].strip()}")

    cmd = ["cmake", str(source_dir), "-DSMASH_BUILD_BENCH=ON"] + compiler_flags
    for var in ALL_ABLATION_VARS:
        cmd.append(f"-U{var}")
    for key, val in cmake_flags.items():
        cmd.append(f"-D{key}={val}")

    r = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"    CMAKE FAILED: {r.stderr[-300:]}")
        return None

    # Build only the targets needed for experiments (avoid broken bench targets)
    targets = ["smash", "smash_noopt", "smash_compress_only",
               "bench_sqlite"]
    r = subprocess.run(["make"] + targets + ["-j"], cwd=build_dir,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"    MAKE FAILED: {r.stderr[-300:]}")
        return None

    lib_path = Path(build_dir) / f"libsmash{LIB_SUFFIX}"
    return build_provenance(source_dir, cmake_flags, lib_path)


# ── RSS measurement ──────────────────────────────────────────────────────────

def get_rss_mb(pid):
    """Get RSS of a process in MiB."""
    try:
        out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True)
        return float(out.strip()) / 1024
    except Exception:
        return 0.0


def wait_for_port(port, timeout=30):
    """Wait until a TCP port is accepting connections."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.2)
    return False


def wait_for_port_closed(port, timeout=10):
    """Wait until a TCP port is no longer accepting connections."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            time.sleep(0.2)
        except (ConnectionRefusedError, OSError):
            return True
    return False


def wait_for_timewait_drain(port, timeout=60):
    """Wait until TIME_WAIT sockets on a port are drained."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            # Try ss first (Linux), fall back to netstat
            if IS_DARWIN:
                out = subprocess.check_output(
                    ["netstat", "-an", "-p", "tcp"], text=True, timeout=5
                )
                tw_count = sum(1 for line in out.splitlines()
                              if f".{port} " in line and "TIME_WAIT" in line)
            else:
                out = subprocess.check_output(
                    ["ss", "-tan", "state", "time-wait"], text=True, timeout=5
                )
                tw_count = sum(1 for line in out.splitlines()
                              if f":{port} " in line or f":{port}" in line)
            if tw_count < 100:  # a few stragglers are fine
                return True
        except Exception:
            return True  # can't check → proceed anyway
        time.sleep(1)
    return False


def kill_redis(port, build_dir=None):
    """Forcefully kill any Redis on the given port and wait for it to die."""
    cli = "redis-cli"
    if build_dir:
        cli_path = get_binary("redis-cli", build_dir) or get_binary("redis-cli-smash", build_dir)
        if cli_path:
            cli = cli_path
    subprocess.run([cli, "-p", str(port), "SHUTDOWN", "NOSAVE"],
                   capture_output=True, timeout=5)
    if not wait_for_port_closed(port, timeout=5):
        # Force kill any process on this port
        try:
            if IS_DARWIN:
                out = subprocess.check_output(
                    ["lsof", "-ti", f"tcp:{port}"], text=True, timeout=5
                ).strip()
            else:
                # Linux: use ss or fuser
                try:
                    out = subprocess.check_output(
                        ["fuser", f"{port}/tcp"], text=True, stderr=subprocess.DEVNULL, timeout=5
                    ).strip()
                except (subprocess.CalledProcessError, FileNotFoundError):
                    out = ""
            for pid in out.split():
                try:
                    os.kill(int(pid), signal.SIGKILL)
                except (ProcessLookupError, ValueError):
                    pass
        except subprocess.CalledProcessError:
            pass
        wait_for_port_closed(port, timeout=5)
    # Wait for TIME_WAIT sockets to drain so the next run's clients
    # don't hit "Can't assign requested address" from ephemeral port exhaustion
    wait_for_timewait_drain(port)


# ── METRIC parsing ───────────────────────────────────────────────────────────

def parse_metrics(text):
    """Parse METRIC lines from text."""
    metrics = {}
    for line in text.splitlines():
        m = re.match(r"^METRIC (\S+) (\S+)$", line)
        if m:
            try:
                metrics[m.group(1)] = float(m.group(2))
            except ValueError:
                pass
    return metrics


# ── In-process benchmark runners ─────────────────────────────────────────────

def run_sqlite(build_dir, smash_lib, quick):
    """Run bench_sqlite with RSS timeline sampling."""
    exe = build_dir / "bench" / "bench_sqlite"
    if not exe.exists():
        return None
    args = ["--quick"] if quick else []
    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"
    try:
        # Run as background process to sample RSS during execution
        proc = subprocess.Popen([str(exe)] + args, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env=env)

        # Sample RSS every second while process runs
        rss_timeline = []
        while proc.poll() is None:
            rss = get_rss_mb(proc.pid)
            if rss > 0:
                rss_timeline.append(rss)
            time.sleep(1)

        stdout, _ = proc.communicate(timeout=10)
        metrics = parse_metrics(stdout)

        if metrics and rss_timeline:
            metrics["rss_timeline"] = rss_timeline
            metrics["auc_mb_sec"] = sum(rss_timeline)
            metrics["peak_rss_mb"] = max(rss_timeline) if rss_timeline else metrics.get("peak_rss_mb", 0)
            metrics["min_rss_mb"] = min(rss_timeline) if rss_timeline else metrics.get("min_rss_mb", 0)

        return metrics
    except Exception as e:
        print(f"    sqlite error: {e}")
        return None


def run_rocksdb(build_dir, smash_lib, quick):
    """Run bench_rocksdb C++ binary with RSS timeline sampling."""
    exe = build_dir / "bench" / "bench_rocksdb"
    if exe.exists():
        args = ["--keys", "200000", "--value-size", "256",
                "--cool", "5", "--serve", "10"] if quick else []
        env = os.environ.copy()
        if smash_lib:
            env[PRELOAD_VAR] = str(smash_lib)
            env["SMASH_VERY_COLD_TICKS"] = "5"
        try:
            # Run as background process to sample RSS during execution
            proc = subprocess.Popen([str(exe)] + args, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, env=env)

            # Sample RSS every second while process runs
            rss_timeline = []
            while proc.poll() is None:
                rss = get_rss_mb(proc.pid)
                if rss > 0:
                    rss_timeline.append(rss)
                time.sleep(1)

            stdout, _ = proc.communicate(timeout=10)
            metrics = parse_metrics(stdout)

            if metrics:
                if rss_timeline:
                    metrics["rss_timeline"] = rss_timeline
                    metrics["auc_mb_sec"] = sum(rss_timeline)
                    if "peak_rss_mb" not in metrics:
                        metrics["peak_rss_mb"] = max(rss_timeline)
                    if "min_rss_mb" not in metrics:
                        metrics["min_rss_mb"] = min(rss_timeline)

                if "rss_reduction_pct" not in metrics:
                    peak = metrics.get("peak_rss_mb", 0)
                    min_rss = metrics.get("min_rss_mb", peak)
                    if peak > 0 and min_rss > 0:
                        metrics["rss_reduction_pct"] = (1 - min_rss / peak) * 100
                        metrics["steady_rss_mb"] = metrics.get("serve_rss_mb", min_rss)
                        metrics["post_cool_rss_mb"] = metrics.get("cool_rss_mb", min_rss)
            return metrics
        except Exception as e:
            print(f"    rocksdb error: {e}")
            return None
    # Fallback to shell script
    script = build_dir / "bench" / "bench_rocksdb.sh"
    if script.exists():
        return run_shell_benchmark(script, smash_lib, quick, "rocksdb")
    return None


# ── External-process benchmark runners ───────────────────────────────────────

def run_redis_bench(build_dir, smash_lib, quick):
    """Run Redis benchmark using redis-benchmark (LLAMA-style config)."""
    redis_server = get_binary("redis-server", build_dir)
    redis_cli = get_binary("redis-cli", build_dir)
    redis_benchmark = get_binary("redis-benchmark", build_dir)
    for cmd in ("redis-server", "redis-cli", "redis-benchmark"):
        if not check_binary(cmd, build_dir):
            return None

    port = 16399
    num_ops = 50000 if quick else 200000
    num_clients = 1000 if quick else (50 if not IS_DARWIN else 5000)
    value_size = 1000 if quick else 2000
    keyspace = 100000 if quick else 200000
    cool_sec = 5 if quick else 20

    kill_redis(port, build_dir)

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"

    proc = subprocess.Popen(
        [redis_server, "--port", str(port), "--save", "",
         "--appendonly", "no", "--daemonize", "no", "--loglevel", "warning",
         "--hz", "1", "--dynamic-hz", "no"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        if not wait_for_port(port, timeout=10):
            print("    redis: failed to start")
            return None

        # SET phase via redis-benchmark
        try:
            set_result = subprocess.run(
                [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                 "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                 "-t", "set", "-q"],
                capture_output=True, text=True, timeout=1200
            )
            set_rps = _parse_redis_benchmark_rps(set_result.stdout, "SET")
        except subprocess.TimeoutExpired:
            print("    redis SET timed out, returning partial results")
            set_rps = 0

        # Verify Redis is actually filled
        try:
            dbsize = subprocess.check_output(
                [redis_cli, "-p", str(port), "DBSIZE"],
                text=True, timeout=5
            ).strip()
        except Exception:
            dbsize = "?"

        fill_rss = get_rss_mb(proc.pid)
        if fill_rss < 10:
            print(f"    redis: fill_rss={fill_rss:.1f}MB (too low, DBSIZE={dbsize})")
            return None

        # Cool-down: let compression run, sample RSS timeline
        rss_timeline = [fill_rss]
        min_rss = fill_rss
        for i in range(cool_sec):
            time.sleep(1)
            rss = get_rss_mb(proc.pid)
            if rss > 0:
                rss_timeline.append(rss)
                min_rss = min(min_rss, rss)
            else:
                rss_timeline.append(rss_timeline[-1])

        cool_rss = get_rss_mb(proc.pid)

        # GET phase via redis-benchmark (skip if SET failed)
        get_rps = 0
        if set_rps > 0:
            try:
                get_result = subprocess.run(
                    [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                     "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                     "-t", "get", "-q"],
                    capture_output=True, text=True, timeout=1200
                )
                get_rps = _parse_redis_benchmark_rps(get_result.stdout, "GET")
            except subprocess.TimeoutExpired:
                print("    redis GET timed out, returning partial results")
                get_rps = 0

        # Validate measurements - min_rss=0 means measurement failed (process died)
        if min_rss <= 0 or fill_rss <= 0:
            print(f"    redis: invalid RSS measurement (fill={fill_rss}, min={min_rss})")
            return None
        reduction = (1 - min_rss / fill_rss) * 100
        # AUC = area under curve (MB-seconds) - integral of RSS over cool phase
        auc_mb_sec = sum(rss_timeline)  # Each sample is 1 second apart

        return {
            "peak_rss_mb": fill_rss,
            "steady_rss_mb": cool_rss,
            "min_rss_mb": min_rss,
            "rss_reduction_pct": reduction,
            "rss_timeline": rss_timeline,
            "auc_mb_sec": auc_mb_sec,
            "set_rps": set_rps,
            "get_rps": get_rps,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        kill_redis(port, build_dir)


def _parse_redis_benchmark_rps(output, cmd_name):
    """Parse requests/sec from redis-benchmark -q output."""
    for line in output.splitlines():
        if cmd_name in line and "requests per second" in line:
            m = re.search(r'([\d.]+)\s+requests per second', line)
            if m:
                return float(m.group(1))
    return 0.0


def run_redis_extended_bench(build_dir, smash_lib, quick):
    """Run Redis extended benchmark: SET → DELETE 50% → cool → GET."""
    redis_server = get_binary("redis-server", build_dir)
    redis_cli = get_binary("redis-cli", build_dir)
    redis_benchmark = get_binary("redis-benchmark", build_dir)
    for cmd in ("redis-server", "redis-cli", "redis-benchmark"):
        if not check_binary(cmd, build_dir):
            return None

    port = 16400  # different port from run_redis_bench to avoid conflicts
    num_ops = 50000 if quick else 200000
    num_clients = 1000 if quick else (50 if not IS_DARWIN else 5000)
    value_size = 1000 if quick else 2000
    keyspace = 100000 if quick else 200000
    cool_sec = 5 if quick else 20

    kill_redis(port, build_dir)

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"

    proc = subprocess.Popen(
        [redis_server, "--port", str(port), "--save", "",
         "--appendonly", "no", "--daemonize", "no", "--loglevel", "warning",
         "--hz", "1", "--dynamic-hz", "no"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        if not wait_for_port(port, timeout=10):
            print("    redis_ext: failed to start")
            return None

        # SET phase via redis-benchmark
        try:
            set_result = subprocess.run(
                [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                 "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                 "-t", "set", "-q"],
                capture_output=True, text=True, timeout=1200
            )
            set_rps = _parse_redis_benchmark_rps(set_result.stdout, "SET")
        except subprocess.TimeoutExpired:
            print("    redis_ext SET timed out, returning partial results")
            set_rps = 0
        peak_rss = get_rss_mb(proc.pid)

        # Check how many keys we have
        try:
            dbsize_out = subprocess.check_output(
                [redis_cli, "-p", str(port), "DBSIZE"],
                text=True, timeout=5
            ).strip()
        except Exception:
            dbsize_out = "?"

        # DELETE ~50% of keys using RANDOMKEY
        # redis-benchmark -r uses random keys; SCAN works but RANDOMKEY is simpler
        try:
            total_keys = int(dbsize_out.split(":")[-1].strip().rstrip("\r"))
        except (ValueError, IndexError):
            total_keys = 0
        target_delete = total_keys // 2
        deleted = 0
        if target_delete > 0:
            # Pipeline DEL commands for speed
            batch_size = 100
            for _ in range(0, target_delete, batch_size):
                n = min(batch_size, target_delete - deleted)
                # Get random keys and delete them
                pipe_cmds = ""
                for _ in range(n):
                    pipe_cmds += "RANDOMKEY\n"
                try:
                    key_result = subprocess.run(
                        [redis_cli, "-p", str(port), "--pipe-mode"],
                        input=pipe_cmds, capture_output=True, text=True, timeout=10
                    )
                except Exception:
                    # Fallback: delete one at a time
                    pass
                # Simpler approach: use redis-cli eval
                try:
                    del_result = subprocess.run(
                        [redis_cli, "-p", str(port), "EVAL",
                         f"local d=0; for i=1,{n} do local k=redis.call('RANDOMKEY'); "
                         f"if k then redis.call('DEL',k); d=d+1 end end; return d",
                         "0"],
                        capture_output=True, text=True, timeout=30
                    )
                    try:
                        deleted += int(del_result.stdout.strip().split(")")[-1].strip()
                                      if ")" in del_result.stdout
                                      else del_result.stdout.strip())
                    except (ValueError, IndexError):
                        deleted += n  # assume it worked
                except Exception:
                    break

        post_del_rss = get_rss_mb(proc.pid)
        print(f"    deleted {deleted}/{total_keys} keys", flush=True)

        # Cool-down: let compression run, sample RSS timeline
        rss_timeline = [post_del_rss]
        min_rss = post_del_rss
        for i in range(cool_sec):
            time.sleep(1)
            rss = get_rss_mb(proc.pid)
            if rss > 0:
                rss_timeline.append(rss)
                min_rss = min(min_rss, rss)
            else:
                rss_timeline.append(rss_timeline[-1])

        cool_rss = get_rss_mb(proc.pid)

        # GET phase via redis-benchmark
        get_rps = 0
        if set_rps > 0:
            try:
                get_result = subprocess.run(
                    [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                     "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                     "-t", "get", "-q"],
                    capture_output=True, text=True, timeout=1200
                )
                get_rps = _parse_redis_benchmark_rps(get_result.stdout, "GET")
            except subprocess.TimeoutExpired:
                print("    redis-ext GET timed out, returning partial results")
                get_rps = 0

        # Validate measurements - cool_rss=0 means measurement failed (process died)
        if cool_rss <= 0 or post_del_rss <= 0:
            print(f"    redis-ext: invalid RSS measurement (post_del={post_del_rss}, cool={cool_rss})")
            return None
        reduction = (1 - cool_rss / post_del_rss) * 100
        # AUC = area under curve (MB-seconds) - integral of RSS over cool phase
        auc_mb_sec = sum(rss_timeline)

        return {
            "peak_rss_mb": peak_rss,
            "post_del_rss_mb": post_del_rss,
            "steady_rss_mb": cool_rss,
            "min_rss_mb": min_rss,
            "rss_reduction_pct": reduction,
            "rss_timeline": rss_timeline,
            "auc_mb_sec": auc_mb_sec,
            "set_rps": set_rps,
            "get_rps": get_rps,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        kill_redis(port, build_dir)


def _run_redis_bench_impl(build_dir, smash_lib, quick, patched, extended):
    """Shared implementation for Redis benchmarks (stock and patched, standard and extended)."""
    if patched:
        redis_server = get_binary("redis-server-smash", build_dir)
        redis_cli = get_binary("redis-cli-smash", build_dir)
        for cmd in ("redis-server-smash", "redis-cli-smash", "redis-benchmark"):
            if not check_binary(cmd, build_dir):
                return None
    else:
        redis_server = get_binary("redis-server", build_dir)
        redis_cli = get_binary("redis-cli", build_dir)
        for cmd in ("redis-server", "redis-cli", "redis-benchmark"):
            if not check_binary(cmd, build_dir):
                return None
    redis_benchmark = get_binary("redis-benchmark", build_dir)

    label = ("redis_ext_patched" if extended else "redis_patched") if patched \
        else ("redis_ext" if extended else "redis")
    # Use different ports to avoid conflicts
    port = 16399 + (1 if extended else 0) + (2 if patched else 0)
    num_ops = 50000 if quick else 200000
    num_clients = 1000 if quick else (50 if not IS_DARWIN else 5000)
    value_size = 1000 if quick else 2000
    keyspace = 100000 if quick else 200000
    cool_sec = 5 if quick else 20

    kill_redis(port, build_dir)

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"

    server_args = [redis_server, "--port", str(port), "--save", "",
                   "--appendonly", "no", "--daemonize", "no", "--loglevel", "warning",
                   "--hz", "1", "--dynamic-hz", "no"]
    if patched:
        server_args += ["--idle-mode", "yes"]

    proc = subprocess.Popen(
        server_args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        if not wait_for_port(port, timeout=10):
            print(f"    {label}: failed to start")
            return None

        # SET phase
        try:
            set_result = subprocess.run(
                [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                 "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                 "-t", "set", "-q"],
                capture_output=True, text=True, timeout=1200
            )
            set_rps = _parse_redis_benchmark_rps(set_result.stdout, "SET")
        except subprocess.TimeoutExpired:
            print(f"    {label} SET timed out")
            set_rps = 0

        # Verify fill
        try:
            dbsize = subprocess.check_output(
                [redis_cli, "-p", str(port), "DBSIZE"],
                text=True, timeout=5
            ).strip()
        except Exception:
            dbsize = "?"

        fill_rss = get_rss_mb(proc.pid)
        # Parse DBSIZE (format: "(integer) N")
        num_keys = 0
        try:
            num_keys = int(dbsize.split(":")[-1].strip().rstrip("\r"))
        except (ValueError, IndexError):
            pass
        if fill_rss < 10 or num_keys < keyspace // 4:
            print(f"    {label}: fill_rss={fill_rss:.1f}MB keys={num_keys} (underfill, DBSIZE={dbsize})")
            return None

        # DELETE phase (extended workload only)
        if extended:
            n = num_ops // 2
            try:
                subprocess.run(
                    [redis_cli, "-p", str(port), "EVAL",
                     f"local d=0; for i=1,{n} do local k=redis.call('RANDOMKEY'); "
                     f"if k then redis.call('DEL',k); d=d+1 end end; return d",
                     "0"],
                    capture_output=True, text=True, timeout=300
                )
            except subprocess.TimeoutExpired:
                print(f"    {label} DELETE timed out")

        # Cool phase
        rss_timeline = []
        min_rss = fill_rss
        for _ in range(cool_sec):
            time.sleep(1)
            r = get_rss_mb(proc.pid)
            rss_timeline.append(r)
            if r > 0 and r < min_rss:
                min_rss = r

        steady_rss = get_rss_mb(proc.pid)

        # GET phase
        get_rps = 0
        if set_rps > 0:
            try:
                get_result = subprocess.run(
                    [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                     "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                     "-t", "get", "-q"],
                    capture_output=True, text=True, timeout=1200
                )
                get_rps = _parse_redis_benchmark_rps(get_result.stdout, "GET")
            except subprocess.TimeoutExpired:
                print(f"    {label} GET timed out")

        if fill_rss <= 0 or min_rss <= 0:
            print(f"    {label}: invalid RSS measurement")
            return None

        rss_reduction = (1.0 - min_rss / fill_rss) * 100

        auc = sum(rss_timeline)
        result = {
            "peak_rss_mb": fill_rss,
            "min_rss_mb": min_rss,
            "steady_rss_mb": steady_rss,
            "rss_reduction_pct": rss_reduction,
            "rss_timeline": rss_timeline,
            "auc_mb_sec": auc,
        }
        if set_rps: result["ops_per_sec"] = set_rps
        return result

    finally:
        try:
            subprocess.run([redis_cli, "-p", str(port), "SHUTDOWN", "NOSAVE"],
                          capture_output=True, timeout=5)
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        kill_redis(port, build_dir)


def run_redis_patched_bench(build_dir, smash_lib, quick):
    """Run patched Redis (redis-smash with idle-mode) standard benchmark."""
    return _run_redis_bench_impl(build_dir, smash_lib, quick, patched=True, extended=False)


def run_redis_ext_patched_bench(build_dir, smash_lib, quick):
    """Run patched Redis (redis-smash with idle-mode) extended benchmark (with DELETE)."""
    return _run_redis_bench_impl(build_dir, smash_lib, quick, patched=True, extended=True)


def run_memcached_bench(build_dir, smash_lib, quick):
    """Run Memcached benchmark.

    Memcached's slab allocator creates ~1MB slab pages. For effective compression:
    - Use enough data to fill multiple slabs (~200MB+)
    - Use compressible value patterns (repeated strings)
    - Allow sufficient cool time for compression to kick in
    - Access only a small "hot set" during serve phase
    """
    memcached_bin = get_binary("memcached", build_dir)
    if not check_binary("memcached", build_dir):
        return None

    port = 11299
    # Use larger dataset for meaningful compression
    num_keys = 200000 if quick else 500000
    value_size = 500  # Larger values = better slab utilization
    cool_sec = 15 if quick else 30  # Longer cool for compression
    serve_sec = 10 if quick else 20

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        # Don't override SMASH_VERY_COLD_TICKS - use defaults

    # Start memcached with larger memory limit
    proc = subprocess.Popen(
        [memcached_bin, "-p", str(port), "-m", "1024", "-l", "127.0.0.1"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        if not wait_for_port(port, timeout=10):
            print("    memcached: failed to start")
            return None

        # Populate with compressible values
        _populate_memcached(port, num_keys, value_size)

        # Verify process is still running after populate
        if proc.poll() is not None:
            print(f"    memcached: process died during populate (exit={proc.returncode})")
            return None

        fill_rss = get_rss_mb(proc.pid)

        if fill_rss < 50:
            print(f"    memcached: fill_rss too low ({fill_rss:.1f}MB) - populate may have failed")

        # Cool phase - track RSS timeline and minimum
        rss_timeline = [fill_rss]
        min_rss = fill_rss
        for _ in range(cool_sec):
            time.sleep(1)
            if proc.poll() is not None:
                print(f"    memcached: process died during cool phase (exit={proc.returncode})")
                return None
            rss = get_rss_mb(proc.pid)
            if rss > 0:
                rss_timeline.append(rss)
                min_rss = min(min_rss, rss)
            else:
                rss_timeline.append(rss_timeline[-1])
        cool_rss = get_rss_mb(proc.pid)

        # Serve phase: access hot 5%
        hot_keys = max(1, num_keys // 20)
        _access_memcached(port, hot_keys, serve_sec)

        # Verify process survived serve phase
        if proc.poll() is not None:
            print(f"    memcached: process died during serve phase (exit={proc.returncode})")
            return None
        serve_rss = get_rss_mb(proc.pid)

        # Validate measurements - min_rss=0 means measurement failed (process died)
        if min_rss <= 0 or fill_rss <= 0:
            print(f"    memcached: invalid RSS measurement (fill={fill_rss}, min={min_rss})")
            return None
        reduction = (1 - min_rss / fill_rss) * 100
        # AUC = area under curve (MB-seconds) - integral of RSS over cool phase
        auc_mb_sec = sum(rss_timeline)

        return {
            "peak_rss_mb": fill_rss,
            "cool_rss_mb": cool_rss,
            "steady_rss_mb": serve_rss,
            "min_rss_mb": min_rss,
            "rss_reduction_pct": reduction,
            "rss_timeline": rss_timeline,
            "auc_mb_sec": auc_mb_sec,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def _populate_memcached(port, num_keys, value_size=500):
    """Populate memcached with compressible data.

    Uses repeated patterns that compress well with LZ4/zstd:
    - Each value has a unique prefix (key identifier)
    - Followed by repeated compressible content
    """
    s = socket.create_connection(("127.0.0.1", port), timeout=120)
    s.settimeout(120)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        batch = []
        batch_size = 50  # Smaller batches for reliability
        for i in range(num_keys):
            key = f"key:{i:07d}"
            # Create compressible value: prefix + repeated pattern
            prefix = f"entry_{i:07d}_"
            pattern = "abcdefghij" * 10  # 100 char pattern
            padding_needed = max(0, value_size - len(prefix))
            val = prefix + (pattern * (padding_needed // len(pattern) + 1))[:padding_needed]
            cmd = f"set {key} 0 0 {len(val)}\r\n{val}\r\n"
            batch.append(cmd)
            if len(batch) >= batch_size:
                s.sendall("".join(batch).encode())
                # Drain responses
                try:
                    while True:
                        data = s.recv(65536)
                        if len(data) < 1000:
                            break
                except socket.timeout:
                    pass
                batch = []
                # Print progress for large populations
                if i > 0 and i % 50000 == 0:
                    print(f"    memcached populate: {i}/{num_keys}", flush=True)
        if batch:
            s.sendall("".join(batch).encode())
            time.sleep(0.5)
            try:
                s.recv(65536)
            except socket.timeout:
                pass
    finally:
        s.close()


def _access_memcached(port, hot_keys, duration_sec):
    """Access hot keys in memcached for a duration."""
    deadline = time.time() + duration_sec
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(1.0)
    try:
        while time.time() < deadline:
            batch = []
            for _ in range(100):
                key = f"key:{hash(time.time()) % hot_keys:07d}"
                batch.append(f"get {key}\r\n")
            try:
                s.sendall("".join(batch).encode())
                s.recv(65536)
            except (socket.timeout, BrokenPipeError, ConnectionResetError):
                # Connection issues during serve phase - reconnect
                try:
                    s.close()
                except Exception:
                    pass
                try:
                    s = socket.create_connection(("127.0.0.1", port), timeout=5)
                    s.settimeout(1.0)
                except Exception:
                    break
            time.sleep(0.01)
    finally:
        try:
            s.close()
        except Exception:
            pass


_DUCKDB_BASELINE_CACHE = {}

def _duckdb_baseline_rss(build_dir, quick):
    """Get DuckDB's cool-phase RSS without Smash — cached.

    Measures at the same workload point as the main benchmark:
    1. Fill TPC-H data to file-based database
    2. CHECKPOINT to flush to disk
    3. Start new process to load data
    4. Sleep for cool_sec
    5. Measure RSS

    This gives an apples-to-apples comparison with Smash runs.
    """
    global _DUCKDB_BASELINE_CACHE
    key = "quick" if quick else "full"
    if key in _DUCKDB_BASELINE_CACHE:
        return _DUCKDB_BASELINE_CACHE[key]

    duckdb_bin = get_binary("duckdb", build_dir)
    sf = 0.5 if quick else 2
    cool_sec = 15 if quick else 60

    with tempfile.TemporaryDirectory() as tmpdir:
        db_file = os.path.join(tmpdir, "baseline.duckdb")
        marker = os.path.join(tmpdir, "marker.csv")

        # Phase 1: Fill (no Smash) - same as main benchmark
        proc = subprocess.Popen(
            [duckdb_bin, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        fill_sql = (f"INSTALL tpch;\nLOAD tpch;\nCALL dbgen(sf={sf});\n"
                    f"COPY (SELECT 'fill_done') TO '{marker}' (HEADER false);\n"
                    f"CHECKPOINT;\n")
        try:
            proc.stdin.write(fill_sql.encode())
            proc.stdin.flush()
        except (BrokenPipeError, OSError):
            proc.kill()
            return 0.0

        if not _wait_file(marker, "fill_done", timeout=600):
            proc.terminate()
            return 0.0

        try:
            proc.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

        # Phase 2: Cool - new process loads data (same as main benchmark)
        if os.path.exists(marker):
            os.remove(marker)

        proc = subprocess.Popen(
            [duckdb_bin, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        hold_sql = (f"SELECT count(*) FROM lineitem;\n"
                    f"COPY (SELECT 'loaded') TO '{marker}' (HEADER false);\n")
        try:
            proc.stdin.write(hold_sql.encode())
            proc.stdin.flush()
        except (BrokenPipeError, OSError):
            proc.kill()
            return 0.0

        if not _wait_file(marker, "loaded", timeout=60):
            proc.terminate()
            return 0.0

        # Cool down and measure RSS at same point as main benchmark
        time.sleep(cool_sec)
        rss = get_rss_mb(proc.pid)

        try:
            proc.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except:
            proc.kill()

    _DUCKDB_BASELINE_CACHE[key] = rss
    return rss


def run_duckdb_bench(build_dir, smash_lib, quick, no_compression=False):
    """Run DuckDB benchmark with TPC-H data in :memory: mode.

    Single-process approach: Smash is loaded from the start so it can track
    all malloc'd pages.  DuckDB with a persistent file only reads data lazily
    via vectorized scans, keeping RSS low — :memory: forces all data into RAM.

    DuckDB needs a longer cool period (~45s) because its many small columnar
    allocations take multiple compressor ticks to fully compress.
    """
    duckdb_bin = get_binary("duckdb", build_dir)
    if not check_binary("duckdb", build_dir):
        return None

    sf = 0.5 if quick else 2
    cool_sec = 15 if quick else 60
    serve_iters = 3 if quick else 10

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)

    compression_pragma = "SET force_compression='Uncompressed';\n" if no_compression else ""

    with tempfile.TemporaryDirectory() as tmpdir:
        marker = os.path.join(tmpdir, "marker.csv")

        proc = subprocess.Popen(
            [duckdb_bin, ":memory:"],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=env,
        )

        # Phase 1: Fill — generate TPC-H data in memory
        fill_sql = (f"{compression_pragma}"
                    f"INSTALL tpch;\nLOAD tpch;\nCALL dbgen(sf={sf});\n"
                    f"COPY (SELECT 'fill_done') TO '{marker}' (HEADER false);\n")

        try:
            proc.stdin.write(fill_sql.encode())
            proc.stdin.flush()
        except (BrokenPipeError, OSError):
            print("    duckdb: process exited during fill")
            return None

        fill_timeout = 300 if quick else 600
        if not _wait_file(marker, "fill_done", timeout=fill_timeout):
            print(f"    duckdb: fill timed out after {fill_timeout}s")
            proc.terminate()
            return None

        peak_rss = get_rss_mb(proc.pid)
        if peak_rss <= 0:
            print("    duckdb: could not measure fill RSS")
            proc.terminate()
            return None

        # Phase 2: Cool — let Smash compress idle pages
        rss_timeline = [peak_rss]
        min_rss = peak_rss
        for _ in range(cool_sec):
            time.sleep(1)
            rss = get_rss_mb(proc.pid)
            if rss > 0:
                rss_timeline.append(rss)
                min_rss = min(min_rss, rss)
            else:
                rss_timeline.append(rss_timeline[-1])

        cool_rss = get_rss_mb(proc.pid)
        if cool_rss <= 0:
            cool_rss = min_rss

        # Phase 3: Serve — narrow date-range queries (hot subset)
        if os.path.exists(marker):
            os.remove(marker)

        serve_query = ("SELECT l_returnflag, l_linestatus, count(*), sum(l_quantity) "
                       "FROM lineitem WHERE l_shipdate BETWEEN '1998-11-01' AND '1998-12-01' "
                       "GROUP BY l_returnflag, l_linestatus;\n")
        serve_sql = serve_query * serve_iters
        serve_sql += f"COPY (SELECT 'serve_done') TO '{marker}' (HEADER false);\n"

        try:
            proc.stdin.write(serve_sql.encode())
            proc.stdin.flush()
        except (BrokenPipeError, OSError):
            print("    duckdb: serve write failed")
            serve_rss = cool_rss
        else:
            if _wait_file(marker, "serve_done", timeout=60):
                serve_rss = get_rss_mb(proc.pid)
                if serve_rss <= 0:
                    serve_rss = cool_rss
            else:
                serve_rss = cool_rss

        # Clean up
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

        if cool_rss <= 0 or peak_rss <= 0:
            print(f"    duckdb: invalid RSS (peak={peak_rss}, cool={cool_rss})")
            return None

        reduction = (1 - min_rss / peak_rss) * 100
        auc_mb_sec = sum(rss_timeline)

        return {
            "peak_rss_mb": peak_rss,
            "post_cool_rss_mb": cool_rss,
            "steady_rss_mb": serve_rss,
            "min_rss_mb": min_rss,
            "rss_reduction_pct": reduction,
            "rss_timeline": rss_timeline,
            "auc_mb_sec": auc_mb_sec,
        }


def _wait_file(path, content, timeout=60):
    """Wait for a file to appear with expected content."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                with open(path) as f:
                    if content in f.read():
                        return True
            except Exception:
                pass
        time.sleep(0.5)
    return False


def _check_cmd(name):
    """Check if a command exists in PATH."""
    try:
        subprocess.run(["which", name], capture_output=True, check=True)
        return True
    except subprocess.CalledProcessError:
        return False


# ── Built binary path resolution ─────────────────────────────────────────────
# Prefer binaries built as part of SMASH_BUILD_BENCH_DEPS over system ones.

_BENCH_DEPS_BIN = None

def _get_bench_deps_bin(build_dir):
    """Get the bench/deps/bin directory path."""
    global _BENCH_DEPS_BIN
    if _BENCH_DEPS_BIN is None:
        _BENCH_DEPS_BIN = Path(build_dir) / "bench" / "deps" / "bin"
    return _BENCH_DEPS_BIN


def get_binary(name, build_dir):
    """Get the path to a binary, preferring built versions over system ones.

    Returns the full path to a built binary if available, otherwise returns
    the command name to use via PATH lookup.
    """
    deps_bin = _get_bench_deps_bin(build_dir)

    # Map command names to built binary names
    binary_map = {
        "memcached": "memcached",
        "duckdb": "duckdb",
        "redis-server": "redis-server-libc",
        "redis-server-smash": "redis-server-smash",
        "redis-cli": "redis-cli",
        "redis-cli-smash": "redis-cli-smash",
        "redis-benchmark": "redis-benchmark",
    }

    built_name = binary_map.get(name, name)
    built_path = deps_bin / built_name

    if built_path.exists():
        return str(built_path)

    # Fall back to system command
    return name


def check_binary(name, build_dir):
    """Check if a binary is available (either built or in PATH)."""
    deps_bin = _get_bench_deps_bin(build_dir)
    binary_map = {
        "memcached": "memcached",
        "duckdb": "duckdb",
        "redis-server": "redis-server-libc",
        "redis-server-smash": "redis-server-smash",
        "redis-cli": "redis-cli",
        "redis-cli-smash": "redis-cli-smash",
        "redis-benchmark": "redis-benchmark",
    }
    built_name = binary_map.get(name, name)
    built_path = deps_bin / built_name

    if built_path.exists():
        return True
    return _check_cmd(name)


# ── Run shell-script-based benchmark ─────────────────────────────────────────

def run_shell_benchmark(script, smash_lib, quick, name):
    """Run a shell script benchmark and parse output for smash metrics."""
    args = ["bash", str(script)]
    if quick:
        args.append("--quick")

    # The shell script handles SMASH_LIB internally via BUILD_DIR
    # Just make sure libsmash.dylib is the right one (rebuilt)
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=600)
        stdout = r.stdout

        # Parse metrics: look for [smash] ... RSS: X MB lines
        metrics = {}
        for line in stdout.splitlines():
            m = re.match(
                r"\s*\[smash\]\s+(Fill|Cool|Serve|Cold re-access)\s+done.*RSS:\s+([\d.]+)\s+MB",
                line)
            if m:
                phase = m.group(1).lower().replace(" ", "_").replace("-", "")
                metrics[f"{phase}_rss_mb"] = float(m.group(2))

        # Also parse METRIC lines
        for line in stdout.splitlines():
            m = re.match(r"^METRIC smash_(\S+) ([\d.]+)$", line)
            if m:
                metrics[m.group(1)] = float(m.group(2))

        return metrics
    except Exception as e:
        print(f"    {name} error: {e}")
        return None


# ── Pandas cold-columns benchmark ────────────────────────────────────────────

def run_pandas_bench(build_dir, smash_lib, quick):
    """Run Python+pandas cold-columns benchmark as a subprocess.

    Creates a large DataFrame with many columns, works a subset ("hot"),
    lets the rest go cold, then measures RSS reduction and decompression cost.

    bench_pandas.py launches its own inner subprocess with LD_PRELOAD, so we
    DON'T set LD_PRELOAD here — we pass --smash-lib instead.
    """
    script = build_dir.parent / "bench" / "bench_pandas.py"
    if not script.exists():
        print(f"    pandas script not found: {script}")
        return None

    # Check pandas is available
    try:
        subprocess.check_call(
            [sys.executable, "-c", "import pandas, numpy"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        print("    pandas/numpy not installed, skipping")
        return None

    args = [sys.executable, str(script), "--runs", "1"]
    if quick:
        args.append("--quick")
    else:
        args.extend(["--rows", "2000000", "--cool-sec", "60"])
    if smash_lib:
        args.extend(["--smash-lib", str(smash_lib)])

    try:
        proc = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

        # bench_pandas.py runs the actual work in an inner subprocess.
        # We can't sample RSS of the inner process from here, so rely on
        # the internal METRIC lines from the benchmark.
        stdout, stderr = proc.communicate(timeout=600)

        if proc.returncode != 0:
            print(f"    pandas error: exit code {proc.returncode}")
            if stderr:
                print(f"    stderr: {stderr[:300]}")
            return None

        metrics = parse_metrics(stdout)

        if "rss_reduction_pct" not in metrics:
            peak = metrics.get("peak_rss_mb", 0)
            min_rss = metrics.get("min_rss_mb", peak)
            if peak > 0 and min_rss > 0:
                metrics["rss_reduction_pct"] = (1 - min_rss / peak) * 100

        return metrics
    except Exception as e:
        print(f"    pandas error: {e}")
        return None


# ── App runner dispatch ──────────────────────────────────────────────────────

def run_app(app, build_dir, smash_lib, quick):
    """Run a benchmark for a given app, return metrics dict."""
    if app == "sqlite":
        return run_sqlite(build_dir, smash_lib, quick)
    elif app == "rocksdb":
        return run_rocksdb(build_dir, smash_lib, quick)
    elif app == "memcached":
        return run_memcached_bench(build_dir, smash_lib, quick)
    elif app == "redis":
        return run_redis_bench(build_dir, smash_lib, quick)
    elif app == "redis_ext":
        return run_redis_extended_bench(build_dir, smash_lib, quick)
    elif app == "redis_patched":
        return run_redis_patched_bench(build_dir, smash_lib, quick)
    elif app == "redis_ext_patched":
        return run_redis_ext_patched_bench(build_dir, smash_lib, quick)
    elif app == "duckdb":
        return run_duckdb_bench(build_dir, smash_lib, quick)
    elif app == "pandas":
        return run_pandas_bench(build_dir, smash_lib, quick)
    return None


# ── Ablation experiment ──────────────────────────────────────────────────────

def run_ablation(build_dir, source_dir, apps, quick, output_dir, runs=1):
    """Run full ablation study across all apps and configs."""
    results_path = output_dir / "ablation_results.json"

    smash_lib = build_dir / f"libsmash{LIB_SUFFIX}"

    # Load existing results.  Preserve any existing provenance/system-info
    # at the top level; we extend, not overwrite, so partial re-runs keep
    # the original host record while newly-stamped entries carry the
    # current build.
    all_results = {}
    if results_path.exists():
        all_results = json.loads(results_path.read_text())

    # Ensure top-level provenance.  If the file already has one, keep it
    # (entries embedded earlier were stamped under it), but always append
    # the current session so we can see when this was re-opened.
    sessions = all_results.setdefault("_sessions", [])
    sessions.append({
        "timestamp_utc": datetime.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "runs_requested": runs,
        "quick": quick,
        "apps": list(apps),
        "system_info": collect_system_info(),
        "smash_env_at_start": collect_smash_env(),
        "bench_params": BENCH_PARAMS,
    })
    results_path.write_text(json.dumps(all_results, indent=2))

    last_flags = None
    current_provenance = None
    total = len(ABLATION_CONFIGS)
    t0 = time.time()

    for idx, (cid, cfg) in enumerate(ABLATION_CONFIGS.items(), 1):
        elapsed = time.time() - t0
        print(f"\n[{idx}/{total}] {cfg['name']} ({cid})  [{elapsed:.0f}s elapsed]")

        # Rebuild if flags changed
        flags = cfg["cmake_flags"]
        if flags != last_flags and cfg["use_smash"]:
            print(f"  Rebuilding libsmash...")
            current_provenance = rebuild(build_dir, flags, source_dir)
            if current_provenance is None:
                print(f"  SKIPPING {cid}: build failed")
                continue
            last_flags = flags

        for app in apps:
            existing = all_results.get(app, {}).get(cid)
            if existing and len(existing.get("runs", [])) >= runs:
                print(f"  {app}: cached ({len(existing['runs'])} runs)")
                continue

            if cfg["use_smash"]:
                lib = smash_lib
            elif cfg.get("use_mesh"):
                lib = MESH_LIB
            else:
                lib = None
            run_results = []

            max_attempts = runs + 2  # allow a couple retries for transient failures
            attempt = 0
            for run_num in range(1, runs + 1):
                while attempt < max_attempts:
                    attempt += 1
                    print(f"  {app} run {run_num}...", end="", flush=True)
                    metrics = run_app(app, build_dir, lib, quick)
                    if metrics:
                        rss = metrics.get("rss_reduction_pct", "N/A")
                        print(f" rss_reduction={rss}")
                        run_results.append(metrics)
                        break
                    elif app.startswith("redis"):
                        print(f" RETRY (transient failure)")
                        time.sleep(2)
                    else:
                        print(f" SKIP (not available)")
                        break
                else:
                    print(f"  {app}: too many failures, skipping remaining runs")
                    break

            if run_results:
                med = _median_metrics(run_results)
                if app not in all_results:
                    all_results[app] = {}
                all_results[app][cid] = {
                    "name": cfg["name"],
                    "runs": run_results,
                    "median": med,
                    "provenance": current_provenance,
                }

        # Save incrementally
        results_path.write_text(json.dumps(all_results, indent=2))

    elapsed = time.time() - t0
    print(f"\nAblation complete in {elapsed:.0f}s. Results: {results_path}")
    return all_results


# ── Compress-only experiment ─────────────────────────────────────────────────

def run_compress_only(build_dir, source_dir, apps, quick, output_dir, runs=1):
    """Run compress-only experiment for all apps."""
    results_path = output_dir / "compress_only_results.json"

    smash_lib = build_dir / f"libsmash{LIB_SUFFIX}"
    co_lib = build_dir / f"libsmash_compress_only{LIB_SUFFIX}"

    if not co_lib.exists():
        print(f"ERROR: {co_lib} not found. Build with -DSMASH_BUILD_BENCH=ON")
        return {}

    # Ensure default build for full smash and capture provenance.
    current_provenance = rebuild(build_dir, {}, source_dir)
    if current_provenance is None:
        print("ERROR: rebuild failed")
        return {}

    all_results = {}
    if results_path.exists():
        all_results = json.loads(results_path.read_text())

    sessions = all_results.setdefault("_sessions", [])
    sessions.append({
        "timestamp_utc": datetime.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "runs_requested": runs,
        "quick": quick,
        "apps": list(apps),
        "system_info": collect_system_info(),
        "smash_env_at_start": collect_smash_env(),
        "bench_params": BENCH_PARAMS,
    })
    results_path.write_text(json.dumps(all_results, indent=2))

    configs = [
        ("baseline", None),
        ("compress_only", co_lib),
        ("full_smash", smash_lib),
    ]

    t0 = time.time()
    for app in apps:
        print(f"\n{'='*60}")
        print(f"  Compress-only: {app}")
        print(f"{'='*60}")

        for config_name, lib in configs:
            key = f"{app}_{config_name}"
            existing = all_results.get(key)
            if existing and len(existing.get("runs", [])) >= runs:
                print(f"  {config_name}: cached ({len(existing['runs'])} runs)")
                continue

            run_results = []
            max_attempts = runs + 2
            attempt = 0
            for run_num in range(1, runs + 1):
                while attempt < max_attempts:
                    attempt += 1
                    print(f"  {config_name} run {run_num}...", end="", flush=True)
                    metrics = run_app(app, build_dir, lib, quick)
                    if metrics:
                        rss = metrics.get("rss_reduction_pct", 0)
                        steady = metrics.get("steady_rss_mb", 0)
                        print(f" rss={steady:.1f}MB reduction={rss:.1f}%")
                        run_results.append(metrics)
                        break
                    elif app.startswith("redis"):
                        print(f" RETRY (transient failure)")
                        time.sleep(2)
                    else:
                        print(f" SKIP")
                        break

            if run_results:
                all_results[key] = {
                    "runs": run_results,
                    "median": _median_metrics(run_results),
                    "provenance": current_provenance,
                }

        results_path.write_text(json.dumps(all_results, indent=2))

    elapsed = time.time() - t0
    print(f"\nCompress-only complete in {elapsed:.0f}s. Results: {results_path}")
    return all_results


# ── DuckDB compression experiment ─────────────────────────────────────────────

def run_duckdb_compression_experiment(build_dir, source_dir, quick, output_dir, runs=1):
    """Compare DuckDB's internal compression vs Smash compression.

    Four configurations:
      1. baseline:       system malloc, DuckDB compression ON
      2. duckdb-uncomp:  system malloc, DuckDB compression OFF
      3. smash:          Smash, DuckDB compression ON
      4. smash-uncomp:   Smash, DuckDB compression OFF

    This isolates the contribution of each compression layer.
    """
    results_path = output_dir / "duckdb_compression_results.json"
    smash_lib = build_dir / f"libsmash{LIB_SUFFIX}"

    # Ensure default build
    rebuild(build_dir, {}, source_dir)

    all_results = {}
    if results_path.exists():
        all_results = json.loads(results_path.read_text())

    configs = [
        ("baseline",      None,      False),  # (name, smash_lib, no_compression)
        ("duckdb-uncomp", None,      True),
        ("smash",         smash_lib, False),
        ("smash-uncomp",  smash_lib, True),
    ]

    t0 = time.time()
    for config_name, lib, no_compress in configs:
        existing = all_results.get(config_name)
        if existing and len(existing.get("runs", [])) >= runs:
            print(f"  {config_name}: cached ({len(existing['runs'])} runs)")
            continue

        run_results = []
        for run_num in range(1, runs + 1):
            print(f"  {config_name} run {run_num}...", end="", flush=True)
            metrics = run_duckdb_bench(build_dir, lib, quick,
                                       no_compression=no_compress)
            if metrics:
                rss = metrics.get("steady_rss_mb", 0)
                red = metrics.get("rss_reduction_pct", 0)
                print(f" rss={rss:.1f}MB reduction={red:.1f}%")
                run_results.append(metrics)
            else:
                print(" SKIP")
                break

        if run_results:
            all_results[config_name] = {
                "runs": run_results,
                "median": _median_metrics(run_results),
            }

        results_path.write_text(json.dumps(all_results, indent=2))

    # Print summary table
    print(f"\n  {'Config':<20s} {'Cool RSS':>10s} {'Serve RSS':>10s} {'Reduction':>10s}")
    print("  " + "-" * 52)
    for config_name, _, _ in configs:
        if config_name not in all_results:
            continue
        med = all_results[config_name].get("median", {})
        cool = med.get("cool_rss_mb", med.get("post_cool_rss_mb", 0))
        serve = med.get("steady_rss_mb", 0)
        red = med.get("rss_reduction_pct", 0)
        print(f"  {config_name:<20s} {cool:>9.0f}MB {serve:>9.0f}MB {red:>9.1f}%")

    elapsed = time.time() - t0
    print(f"\nDuckDB compression experiment complete in {elapsed:.0f}s. Results: {results_path}")
    return all_results


# ── Helpers ──────────────────────────────────────────────────────────────────

def _median_metrics(runs):
    """Compute median of each metric across runs."""
    if not runs:
        return {}

    # Find the median run by rss_reduction_pct (for selecting list values)
    reductions = [(i, r.get("rss_reduction_pct", 0)) for i, r in enumerate(runs) if r.get("rss_reduction_pct") is not None]
    if reductions:
        reductions.sort(key=lambda x: x[1])
        median_run_idx = reductions[len(reductions) // 2][0]
    else:
        median_run_idx = 0

    keys = set()
    for r in runs:
        keys.update(r.keys())
    result = {}
    for k in keys:
        vals = [r[k] for r in runs if k in r and r[k] is not None]
        if not vals:
            continue
        # For list values (like rss_timeline), use the value from the median run
        if isinstance(vals[0], list):
            if k in runs[median_run_idx]:
                result[k] = runs[median_run_idx][k]
        # For scalar values, compute median
        elif isinstance(vals[0], (int, float)):
            result[k] = statistics.median(vals)
    return result


# ── Paper table generation ───────────────────────────────────────────────────

def generate_ablation_table(results, apps):
    """Generate LaTeX ablation table from results."""
    lines = []
    lines.append("% Auto-generated ablation table")
    lines.append("\\begin{tabular}{ll" + "r" * len(apps) + "r}")
    lines.append("\\toprule")
    headers = " & ".join([f"\\textbf{{{a.capitalize()}}}" for a in apps])
    lines.append(f"ID & Configuration & {headers} & Avg.\\ $\\Delta$ \\\\")
    lines.append("\\midrule")

    # Get baseline (B1) values
    b1 = {}
    for app in apps:
        b1[app] = results.get(app, {}).get("B1", {}).get("median", {}).get("rss_reduction_pct")

    for cid, cfg in ABLATION_CONFIGS.items():
        vals = []
        deltas = []
        for app in apps:
            v = results.get(app, {}).get(cid, {}).get("median", {}).get("rss_reduction_pct")
            if v is not None and b1.get(app) is not None:
                delta = v - b1[app]
                if cid == "B1":
                    vals.append(f"\\textbf{{{v:.1f}\\%}}")
                else:
                    sign = "+" if delta >= 0 else "$-$"
                    absv = abs(delta)
                    vals.append(f"{v:.1f}\\% ({sign}{absv:.1f})")
                    deltas.append(delta)
            else:
                vals.append("---")

        avg_delta = f"{statistics.mean(deltas):+.1f}" if deltas else "---"
        val_str = " & ".join(vals)

        if cid == "B1":
            lines.append(f"    & \\textbf{{{cfg['name']}}} & {val_str} & --- \\\\")
        elif cid in ("B0", "B2"):
            lines.append(f"{cid}  & {cfg['name']} & {val_str} & {avg_delta} \\\\")
        else:
            lines.append(f"    & {cfg['name']} & {val_str} & {avg_delta} \\\\")

        if cid == "B0":
            lines.append("\\midrule")

    lines.append("\\bottomrule")
    lines.append("\\end{tabular}")
    return "\n".join(lines)


def generate_compress_only_table(results, apps):
    """Generate LaTeX compress-only table from results."""
    lines = []
    lines.append("% Auto-generated compress-only table")
    lines.append("\\begin{tabular}{llrrr}")
    lines.append("\\toprule")
    lines.append("Benchmark & Config & Cool RSS & Reduction & Ops/s \\\\")
    lines.append("\\midrule")

    for app in apps:
        baseline = results.get(f"{app}_baseline", {}).get("median", {})
        co = results.get(f"{app}_compress_only", {}).get("median", {})
        full = results.get(f"{app}_full_smash", {}).get("median", {})

        base_rss = baseline.get("steady_rss_mb", baseline.get("peak_rss_mb", 0))
        co_rss = co.get("steady_rss_mb", co.get("min_rss_mb", 0))
        full_rss = full.get("steady_rss_mb", full.get("min_rss_mb", 0))

        co_red = (1 - co_rss / base_rss) * 100 if base_rss > 0 and co_rss > 0 else 0
        full_red = (1 - full_rss / base_rss) * 100 if base_rss > 0 and full_rss > 0 else 0

        lines.append(f"\\multirow{{3}}{{*}}{{{app.capitalize()}}}")
        lines.append(f"  & baseline       & {base_rss:.0f}  & ---    & --- \\\\")
        lines.append(f"  & compress-only  & {co_rss:.0f}    & {co_red:.1f}\\% & --- \\\\")
        lines.append(f"  & full \\sys      & \\textbf{{{full_rss:.0f}}}   & "
                     f"\\textbf{{{full_red:.1f}\\%}} & --- \\\\")
        lines.append("\\midrule")

    # Remove last midrule
    if lines[-1] == "\\midrule":
        lines[-1] = "\\bottomrule"

    lines.append("\\end{tabular}")
    return "\n".join(lines)


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Run all paper experiments")
    parser.add_argument("--quick", action="store_true", help="Use --quick for faster runs")
    parser.add_argument("--ablation-only", action="store_true")
    parser.add_argument("--compress-only-only", action="store_true")
    parser.add_argument("--duckdb-compression-only", action="store_true",
                        help="Run only the DuckDB compression comparison experiment")
    parser.add_argument("--build-dir", default=".", help="Build directory (default: .)")
    parser.add_argument("--output-dir", default=None,
                        help="Output directory for results (default: paper_results/<platform>)")
    parser.add_argument("--runs", type=int, default=1, help="Runs per config (default: 1)")
    parser.add_argument("--apps", default=None,
                        help="Comma-separated app list (default: all available)")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    source_dir = Path(__file__).resolve().parent.parent
    if args.output_dir:
        output_dir = Path(args.output_dir).resolve()
    else:
        plat_subdir = "macos" if IS_DARWIN else "linux"
        output_dir = source_dir / "paper_results" / plat_subdir
    output_dir.mkdir(parents=True, exist_ok=True)

    # Determine available apps
    if args.apps:
        apps = [a.strip() for a in args.apps.split(",")]
    else:
        apps = []
        for app in APPS:
            if app == "sqlite" and (build_dir / "bench" / "bench_sqlite").exists():
                apps.append(app)
            elif app == "rocksdb":
                if ((build_dir / "bench" / "bench_rocksdb").exists() or
                        (build_dir / "bench" / "bench_rocksdb.sh").exists()):
                    apps.append(app)
            elif app == "duckdb" and check_binary("duckdb", build_dir):
                apps.append(app)
            elif app == "memcached" and check_binary("memcached", build_dir):
                apps.append(app)
            elif app == "redis" and check_binary("redis-server", build_dir):
                apps.append(app)
            elif app == "redis_ext" and check_binary("redis-server", build_dir):
                apps.append(app)
            elif app == "redis_patched" and check_binary("redis-server-smash", build_dir):
                apps.append(app)
            elif app == "redis_ext_patched" and check_binary("redis-server-smash", build_dir):
                apps.append(app)

    print(f"Apps: {', '.join(apps)}")
    print(f"Build dir: {build_dir}")
    print(f"Output: {output_dir}")
    print(f"Quick: {args.quick}")
    print()

    ablation_results = {}
    co_results = {}

    if not args.compress_only_only and not args.duckdb_compression_only:
        print("=" * 70)
        print("  ABLATION STUDY")
        print("=" * 70)
        ablation_results = run_ablation(build_dir, source_dir, apps, args.quick,
                                        output_dir, runs=args.runs)

    if not args.ablation_only and not args.duckdb_compression_only:
        print("\n" + "=" * 70)
        print("  COMPRESS-ONLY EXPERIMENT")
        print("=" * 70)
        co_results = run_compress_only(build_dir, source_dir, apps, args.quick,
                                       output_dir, runs=args.runs)

    # DuckDB compression comparison (if duckdb is in app list)
    if "duckdb" in apps and not args.ablation_only and not args.compress_only_only:
        print("\n" + "=" * 70)
        print("  DUCKDB COMPRESSION EXPERIMENT")
        print("=" * 70)
        run_duckdb_compression_experiment(build_dir, source_dir, args.quick,
                                          output_dir, runs=args.runs)

    # Generate tables
    tables_path = output_dir / "paper_tables.txt"
    with open(tables_path, "w") as f:
        if ablation_results:
            used_apps = [a for a in apps if a in ablation_results]
            f.write("ABLATION TABLE\n")
            f.write("=" * 70 + "\n")
            f.write(generate_ablation_table(ablation_results, used_apps))
            f.write("\n\n")

        if co_results:
            f.write("COMPRESS-ONLY TABLE\n")
            f.write("=" * 70 + "\n")
            f.write(generate_compress_only_table(co_results, apps))
            f.write("\n")

    print(f"\nPaper tables: {tables_path}")
    print("Done!")


if __name__ == "__main__":
    main()
