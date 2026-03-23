#!/usr/bin/env python3
"""Unified benchmark suite for Smash paper experiments.

Runs all benchmarks needed for the paper:
1. Ablation study (9 configurations x 6 apps)
2. Compress-only experiment (3 configs x 6 apps)
3. Algorithm comparison (LZ4/zstd/WKdm across data types)
4. Allocator substrate comparison (system/mimalloc/jemalloc/tcmalloc/hoard)

Usage:
    cd build
    python3 ../bench/run_all_benchmarks.py [--quick] [--full] [--runs N]

    --quick: Fast smoke test (smaller datasets, 1 run)
    --full:  Paper-quality results (large datasets, 3 runs)
    --runs:  Override number of runs per configuration

Output:
    paper_results/
        ablation_results.json
        compress_only_results.json
        algo_compare_results.json
        allocator_compare_results.json
        paper_tables.txt
        summary.txt

Works on both Linux and macOS.
"""

import argparse
import json
import os
import platform
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from collections import OrderedDict
from datetime import datetime
from pathlib import Path

# ══════════════════════════════════════════════════════════════════════════════
#  CONFIGURATION
# ══════════════════════════════════════════════════════════════════════════════

IS_DARWIN = platform.system() == "Darwin"
IS_LINUX = platform.system() == "Linux"
PRELOAD_VAR = "DYLD_INSERT_LIBRARIES" if IS_DARWIN else "LD_PRELOAD"
LIB_SUFFIX = ".dylib" if IS_DARWIN else ".so"

# CMake ablation variables
ALL_ABLATION_VARS = [
    "SMASH_NUM_ARENAS", "SMASH_COLD_TICKS", "SMASH_VERY_COLD_TICKS",
    "SMASH_DICT_TRAIN_SAMPLES", "SMASH_PREFETCH_WINDOW",
    "SMASH_COMPRESSOR_WORKERS", "SMASH_COMPRESS_STORE_SHARDS",
    "SMASH_LARGE_ALLOC_VM_THRESHOLD",
    "SMASH_ABLATION_NO_ZERO_EAGER", "SMASH_ABLATION_NO_ZERO_DEFERRED",
    "SMASH_ABLATION_NO_SKIP_STATS", "SMASH_ABLATION_NO_CHUNK_BITMAP",
    "SMASH_ABLATION_NO_CALLSITE_ARENA",
]

# Ablation configs matching paper table
ABLATION_CONFIGS = OrderedDict([
    ("B1", {"name": "Default", "cmake_flags": {}, "use_smash": True}),
    ("B0", {"name": "System malloc", "cmake_flags": {}, "use_smash": False}),
    ("DICT", {"name": "With dicts",
              "cmake_flags": {"SMASH_DICT_TRAIN_SAMPLES": "16"}, "use_smash": True}),
    ("T1a", {"name": "No arenas",
             "cmake_flags": {"SMASH_NUM_ARENAS": "1"}, "use_smash": True}),
    ("T1c", {"name": "LZ4 only",
             "cmake_flags": {"SMASH_VERY_COLD_TICKS": "9999"}, "use_smash": True}),
    ("T2a", {"name": "No zero-deferred",
             "cmake_flags": {"SMASH_ABLATION_NO_ZERO_DEFERRED": "ON"}, "use_smash": True}),
    ("T1e", {"name": "No prefetch",
             "cmake_flags": {"SMASH_PREFETCH_WINDOW": "0"}, "use_smash": True}),
    ("T1f", {"name": "Single worker",
             "cmake_flags": {"SMASH_COMPRESSOR_WORKERS": "1"}, "use_smash": True}),
    ("B2", {"name": "No compression",
            "cmake_flags": {"SMASH_COLD_TICKS": "9999"}, "use_smash": True}),
])

APPS = ["sqlite", "rocksdb", "duckdb", "memcached", "redis", "redis_ext"]

# ══════════════════════════════════════════════════════════════════════════════
#  UTILITY FUNCTIONS
# ══════════════════════════════════════════════════════════════════════════════

def log(msg, level="INFO"):
    """Print timestamped log message."""
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {level}: {msg}", flush=True)


def get_rss_mb(pid):
    """Get RSS of a process in MiB."""
    try:
        if IS_DARWIN:
            out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True)
        else:
            with open(f"/proc/{pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1]) / 1024  # KB to MB
            return 0.0
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


def kill_process_on_port(port):
    """Kill any process listening on the given port."""
    try:
        if IS_DARWIN:
            out = subprocess.check_output(
                ["lsof", "-ti", f"tcp:{port}"], text=True, timeout=5
            ).strip()
        else:
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


def median_metrics(runs):
    """Compute median of each metric across runs."""
    if not runs:
        return {}
    keys = set()
    for r in runs:
        keys.update(r.keys())
    result = {}
    for k in keys:
        vals = [r[k] for r in runs if k in r]
        if vals:
            result[k] = statistics.median(vals)
    return result


def check_binary(name, build_dir):
    """Check if a binary is available."""
    deps_bin = Path(build_dir) / "bench" / "deps" / "bin"
    binary_map = {
        "memcached": "memcached",
        "duckdb": "duckdb",
        "redis-server": "redis-server-libc",
        "redis-cli": "redis-cli",
        "redis-benchmark": "redis-benchmark",
    }
    built_name = binary_map.get(name, name)
    if (deps_bin / built_name).exists():
        return True
    try:
        subprocess.run(["which", name], capture_output=True, check=True)
        return True
    except subprocess.CalledProcessError:
        return False


def get_binary(name, build_dir):
    """Get path to binary, preferring built versions."""
    deps_bin = Path(build_dir) / "bench" / "deps" / "bin"
    binary_map = {
        "memcached": "memcached",
        "duckdb": "duckdb",
        "redis-server": "redis-server-libc",
        "redis-cli": "redis-cli",
        "redis-benchmark": "redis-benchmark",
    }
    built_name = binary_map.get(name, name)
    built_path = deps_bin / built_name
    if built_path.exists():
        return str(built_path)
    return name


# ══════════════════════════════════════════════════════════════════════════════
#  BUILD HELPERS
# ══════════════════════════════════════════════════════════════════════════════

def rebuild(build_dir, cmake_flags, source_dir):
    """Reconfigure and rebuild libsmash with given flags."""
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
        log(f"CMAKE FAILED: {r.stderr[-500:]}", "ERROR")
        return False

    targets = ["smash", "smash_noopt", "smash_compress_only", "bench_sqlite"]
    r = subprocess.run(["make"] + targets + ["-j"], cwd=build_dir,
                       capture_output=True, text=True)
    if r.returncode != 0:
        log(f"MAKE FAILED: {r.stderr[-500:]}", "ERROR")
        return False
    return True


# ══════════════════════════════════════════════════════════════════════════════
#  APP BENCHMARK RUNNERS
# ══════════════════════════════════════════════════════════════════════════════

def run_sqlite(build_dir, smash_lib, quick):
    """Run bench_sqlite in-process."""
    exe = Path(build_dir) / "bench" / "bench_sqlite"
    if not exe.exists():
        return None
    args = ["--quick"] if quick else []
    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"
    try:
        r = subprocess.run([str(exe)] + args, capture_output=True, text=True,
                           env=env, timeout=300)
        return parse_metrics(r.stdout)
    except Exception as e:
        log(f"sqlite error: {e}", "ERROR")
        return None


def run_rocksdb(build_dir, smash_lib, quick):
    """Run bench_rocksdb C++ binary."""
    exe = Path(build_dir) / "bench" / "bench_rocksdb"
    if not exe.exists():
        return None
    args = ["--keys", "200000", "--value-size", "256",
            "--cool", "5", "--serve", "10"] if quick else []
    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"
    try:
        r = subprocess.run([str(exe)] + args, capture_output=True, text=True,
                           env=env, timeout=600)
        metrics = parse_metrics(r.stdout)
        if metrics and "rss_reduction_pct" not in metrics:
            peak = metrics.get("peak_rss_mb", 0)
            min_rss = metrics.get("min_rss_mb", peak)
            if peak > 0 and min_rss > 0:
                metrics["rss_reduction_pct"] = (1 - min_rss / peak) * 100
        return metrics
    except Exception as e:
        log(f"rocksdb error: {e}", "ERROR")
        return None


def run_memcached(build_dir, smash_lib, quick):
    """Run Memcached benchmark using TPC-H JSON records (paper methodology).

    Memcached's slab allocator creates ~1MB slab pages. For effective compression:
    - Use TPC-H JSON records (realistic, compressible data)
    - Use enough data to fill multiple slabs (~200MB+)
    - Allow sufficient cool time for compression to kick in
    - Access only a small "hot set" during serve phase
    """
    if not check_binary("memcached", build_dir):
        return None
    if not check_binary("duckdb", build_dir):
        log("memcached: duckdb required for TPC-H corpus generation", "WARN")

    memcached_bin = get_binary("memcached", build_dir)
    port = 11299
    num_keys = 200000 if quick else 500000
    cool_sec = 15 if quick else 30
    serve_sec = 10 if quick else 20

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)

    kill_process_on_port(port)

    # Generate TPC-H corpus
    corpus_file = None
    with tempfile.TemporaryDirectory() as tmpdir:
        corpus_path = os.path.join(tmpdir, "tpch_corpus.jsonl")
        if check_binary("duckdb", build_dir):
            log("Generating TPC-H JSON corpus...")
            if _generate_tpch_corpus(build_dir, num_keys, corpus_path):
                corpus_file = corpus_path
                log(f"Corpus generated: {os.path.getsize(corpus_path) // 1024}KB")

        # Start memcached with larger memory limit
        proc = subprocess.Popen(
            [memcached_bin, "-p", str(port), "-m", "1024", "-l", "127.0.0.1"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )

        try:
            if not wait_for_port(port, timeout=10):
                log("memcached: failed to start", "ERROR")
                return None

            # Populate with TPC-H JSON data
            log(f"Populating {num_keys} keys...")
            _populate_memcached(port, num_keys, corpus_file=corpus_file)

            # Verify process survived populate
            if proc.poll() is not None:
                log(f"memcached: process died during populate (exit={proc.returncode})", "ERROR")
                return None
            fill_rss = get_rss_mb(proc.pid)

            if fill_rss < 50:
                log(f"memcached: fill_rss={fill_rss:.1f}MB (low) - populate may have failed", "WARN")

            # Cool phase - track minimum RSS during compression
            log(f"Cooling for {cool_sec}s...")
            min_rss = fill_rss
            for _ in range(cool_sec):
                time.sleep(1)
                if proc.poll() is not None:
                    log(f"memcached: process died during cool phase (exit={proc.returncode})", "ERROR")
                    return None
                rss = get_rss_mb(proc.pid)
                if rss > 0:
                    min_rss = min(min_rss, rss)
            cool_rss = get_rss_mb(proc.pid)

            # Serve phase: access only hot 5% of keys
            hot_keys = max(1, num_keys // 20)
            log(f"Serving hot {hot_keys} keys for {serve_sec}s...")
            _access_memcached(port, hot_keys, serve_sec)

            # Verify process survived serve phase
            if proc.poll() is not None:
                log(f"memcached: process died during serve phase (exit={proc.returncode})", "ERROR")
                return None
            serve_rss = get_rss_mb(proc.pid)

            # Validate measurements - min_rss=0 means measurement failed
            if min_rss <= 0 or fill_rss <= 0:
                log(f"memcached: invalid RSS measurement (fill={fill_rss}, min={min_rss})", "ERROR")
                return None

            reduction = (1 - min_rss / fill_rss) * 100

            return {
                "peak_rss_mb": fill_rss,
                "cool_rss_mb": cool_rss,
                "steady_rss_mb": serve_rss,
                "min_rss_mb": min_rss,
                "rss_reduction_pct": reduction,
            }
        except Exception as e:
            log(f"memcached error: {e}", "ERROR")
            return None
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


def _generate_tpch_corpus(build_dir, num_records, output_file):
    """Generate TPC-H JSON corpus using DuckDB.

    This produces realistic, compressible JSON records matching the paper's methodology.
    """
    duckdb_bin = get_binary("duckdb", build_dir)
    sf = 0.1 if num_records < 200000 else 0.5  # Scale factor based on records needed

    sql = f"""
INSTALL tpch;
LOAD tpch;
CALL dbgen(sf={sf});
COPY (
    SELECT o_orderkey as id, c_name as customer, c_address as addr,
           c_phone as phone, c_mktsegment as segment,
           o_orderstatus as status, o_totalprice as total,
           o_orderdate as date, o_orderpriority as priority,
           o_comment as order_note, c_comment as customer_note
    FROM orders JOIN customer ON o_custkey = c_custkey
    ORDER BY o_orderkey
    LIMIT {num_records}
) TO '{output_file}' (FORMAT JSON, ARRAY false);
"""
    try:
        subprocess.run([duckdb_bin, "-c", sql], capture_output=True, timeout=120)
        return os.path.exists(output_file)
    except Exception as e:
        log(f"Failed to generate TPC-H corpus: {e}", "ERROR")
        return False


def _populate_memcached(port, num_keys, value_size=500, corpus_file=None):
    """Populate memcached with data.

    If corpus_file is provided, uses TPC-H JSON records (matching paper methodology).
    Otherwise falls back to synthetic compressible data.
    """
    s = socket.create_connection(("127.0.0.1", port), timeout=120)
    s.settimeout(120)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    try:
        batch = []
        batch_size = 50

        if corpus_file and os.path.exists(corpus_file):
            # Use TPC-H JSON records
            with open(corpus_file, 'r') as f:
                for i, line in enumerate(f):
                    if i >= num_keys:
                        break
                    key = f"key:{i:07d}"
                    val = line.strip()
                    cmd = f"set {key} 0 0 {len(val)}\r\n{val}\r\n"
                    batch.append(cmd)
                    if len(batch) >= batch_size:
                        s.sendall("".join(batch).encode())
                        try:
                            while True:
                                data = s.recv(65536)
                                if len(data) < 1000:
                                    break
                        except socket.timeout:
                            pass
                        batch = []
                        if i > 0 and i % 50000 == 0:
                            log(f"memcached populate: {i}/{num_keys}")
        else:
            # Fallback: synthetic compressible data
            for i in range(num_keys):
                key = f"key:{i:07d}"
                prefix = f"entry_{i:07d}_"
                pattern = "abcdefghij" * 10
                padding_needed = max(0, value_size - len(prefix))
                val = prefix + (pattern * (padding_needed // len(pattern) + 1))[:padding_needed]
                cmd = f"set {key} 0 0 {len(val)}\r\n{val}\r\n"
                batch.append(cmd)
                if len(batch) >= batch_size:
                    s.sendall("".join(batch).encode())
                    try:
                        while True:
                            data = s.recv(65536)
                            if len(data) < 1000:
                                break
                    except socket.timeout:
                        pass
                    batch = []
                    if i > 0 and i % 50000 == 0:
                        log(f"memcached populate: {i}/{num_keys}")

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
    """Access hot keys in memcached."""
    deadline = time.time() + duration_sec
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.settimeout(1.0)
        while time.time() < deadline:
            for _ in range(100):
                key = f"key:{hash(time.time()) % hot_keys:07d}"
                s.sendall(f"get {key}\r\n".encode())
            try:
                s.recv(65536)
            except socket.timeout:
                pass
            time.sleep(0.01)
        s.close()
    except Exception:
        pass


def run_redis(build_dir, smash_lib, quick):
    """Run Redis benchmark."""
    for cmd in ("redis-server", "redis-cli", "redis-benchmark"):
        if not check_binary(cmd, build_dir):
            return None

    redis_server = get_binary("redis-server", build_dir)
    redis_cli = get_binary("redis-cli", build_dir)
    redis_benchmark = get_binary("redis-benchmark", build_dir)

    port = 16399
    num_ops = 50000 if quick else 100000
    num_clients = 500 if quick else 2000
    value_size = 1000
    keyspace = 100000
    cool_sec = 5 if quick else 15

    kill_process_on_port(port)

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"

    proc = subprocess.Popen(
        [redis_server, "--port", str(port), "--save", "",
         "--appendonly", "no", "--daemonize", "no", "--loglevel", "warning"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        if not wait_for_port(port, timeout=10):
            log("redis: failed to start", "ERROR")
            return None

        # SET phase
        try:
            set_result = subprocess.run(
                [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                 "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                 "-t", "set", "-q"],
                capture_output=True, text=True, timeout=300
            )
            set_rps = _parse_redis_rps(set_result.stdout, "SET")
        except subprocess.TimeoutExpired:
            set_rps = 0

        fill_rss = get_rss_mb(proc.pid)

        # Cool-down
        min_rss = fill_rss
        for _ in range(cool_sec):
            time.sleep(1)
            rss = get_rss_mb(proc.pid)
            if rss > 0:
                min_rss = min(min_rss, rss)

        cool_rss = get_rss_mb(proc.pid)

        # GET phase
        get_rps = 0
        if set_rps > 0:
            try:
                get_result = subprocess.run(
                    [redis_benchmark, "-p", str(port), "-c", str(num_clients),
                     "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
                     "-t", "get", "-q"],
                    capture_output=True, text=True, timeout=300
                )
                get_rps = _parse_redis_rps(get_result.stdout, "GET")
            except subprocess.TimeoutExpired:
                pass

        # Validate - min_rss=0 means measurement failed
        if min_rss <= 0 or fill_rss <= 0:
            return None
        reduction = (1 - min_rss / fill_rss) * 100

        return {
            "peak_rss_mb": fill_rss,
            "steady_rss_mb": cool_rss,
            "min_rss_mb": min_rss,
            "rss_reduction_pct": reduction,
            "set_rps": set_rps,
            "get_rps": get_rps,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        kill_process_on_port(port)


def _parse_redis_rps(output, cmd_name):
    """Parse requests/sec from redis-benchmark output."""
    for line in output.splitlines():
        if cmd_name in line and "requests per second" in line:
            m = re.search(r'([\d.]+)\s+requests per second', line)
            if m:
                return float(m.group(1))
    return 0.0


def run_redis_ext(build_dir, smash_lib, quick):
    """Run Redis extended benchmark with DELETE phase."""
    # Similar to run_redis but with DELETE 50% between SET and cool
    for cmd in ("redis-server", "redis-cli", "redis-benchmark"):
        if not check_binary(cmd, build_dir):
            return None

    redis_server = get_binary("redis-server", build_dir)
    redis_cli = get_binary("redis-cli", build_dir)
    redis_benchmark = get_binary("redis-benchmark", build_dir)

    port = 16400
    num_ops = 50000 if quick else 100000
    num_clients = 500 if quick else 2000
    value_size = 1000
    keyspace = 100000
    cool_sec = 5 if quick else 15

    kill_process_on_port(port)

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"

    proc = subprocess.Popen(
        [redis_server, "--port", str(port), "--save", "",
         "--appendonly", "no", "--daemonize", "no", "--loglevel", "warning"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        if not wait_for_port(port, timeout=10):
            return None

        # SET phase
        subprocess.run(
            [redis_benchmark, "-p", str(port), "-c", str(num_clients),
             "-n", str(num_ops), "-d", str(value_size), "-r", str(keyspace),
             "-t", "set", "-q"],
            capture_output=True, timeout=300
        )
        peak_rss = get_rss_mb(proc.pid)

        # DELETE ~50% using EVAL
        try:
            subprocess.run(
                [redis_cli, "-p", str(port), "EVAL",
                 "local d=0; for i=1,50000 do local k=redis.call('RANDOMKEY'); "
                 "if k then redis.call('DEL',k); d=d+1 end end; return d", "0"],
                capture_output=True, timeout=60
            )
        except Exception:
            pass

        post_del_rss = get_rss_mb(proc.pid)

        # Cool-down
        min_rss = post_del_rss
        for _ in range(cool_sec):
            time.sleep(1)
            rss = get_rss_mb(proc.pid)
            if rss > 0:
                min_rss = min(min_rss, rss)

        cool_rss = get_rss_mb(proc.pid)

        # Validate measurements - cool_rss=0 means measurement failed (process died)
        if cool_rss <= 0 or post_del_rss <= 0:
            log(f"redis-ext: invalid RSS measurement (post_del={post_del_rss}, cool={cool_rss})", "ERROR")
            return None
        reduction = (1 - cool_rss / post_del_rss) * 100

        return {
            "peak_rss_mb": peak_rss,
            "post_del_rss_mb": post_del_rss,
            "steady_rss_mb": cool_rss,
            "min_rss_mb": min_rss,
            "rss_reduction_pct": reduction,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        kill_process_on_port(port)


def run_duckdb(build_dir, smash_lib, quick):
    """Run DuckDB benchmark with TPC-H data."""
    if not check_binary("duckdb", build_dir):
        return None

    duckdb_bin = get_binary("duckdb", build_dir)
    sf = 0.1 if quick else 0.5
    cool_sec = 5 if quick else 10

    env = os.environ.copy()
    if smash_lib:
        env[PRELOAD_VAR] = str(smash_lib)
        env["SMASH_VERY_COLD_TICKS"] = "5"

    with tempfile.TemporaryDirectory() as tmpdir:
        db_file = os.path.join(tmpdir, "bench.duckdb")
        marker = os.path.join(tmpdir, "marker.csv")

        # Fill phase
        fill_sql = (f"INSTALL tpch;\nLOAD tpch;\nCALL dbgen(sf={sf});\n"
                    f"COPY (SELECT 'fill_done') TO '{marker}' (HEADER false);\n"
                    f"CHECKPOINT;\n")

        proc = subprocess.Popen(
            [duckdb_bin, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=env
        )

        try:
            proc.stdin.write(fill_sql.encode())
            proc.stdin.flush()
        except (BrokenPipeError, OSError):
            return None

        if not _wait_file(marker, "fill_done", timeout=300):
            proc.terminate()
            return None

        fill_rss = get_rss_mb(proc.pid)
        try:
            proc.stdin.close()
        except:
            pass
        proc.wait(timeout=30)

        # Cool phase
        if os.path.exists(marker):
            os.remove(marker)

        proc = subprocess.Popen(
            [duckdb_bin, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=env
        )

        try:
            proc.stdin.write(f"SELECT count(*) FROM lineitem;\nCOPY (SELECT 'loaded') TO '{marker}' (HEADER false);\n".encode())
            proc.stdin.flush()
        except:
            return None

        if not _wait_file(marker, "loaded", timeout=60):
            proc.terminate()
            return None

        time.sleep(cool_sec)
        cool_rss = get_rss_mb(proc.pid)

        try:
            proc.stdin.close()
            proc.terminate()
            proc.wait(timeout=5)
        except:
            proc.kill()

        # Validate measurements
        if cool_rss <= 0 or fill_rss <= 0:
            return None
        reduction = (1 - cool_rss / fill_rss) * 100

        return {
            "peak_rss_mb": fill_rss,
            "cool_rss_mb": cool_rss,
            "steady_rss_mb": cool_rss,
            "min_rss_mb": min(fill_rss, cool_rss),
            "rss_reduction_pct": reduction,
        }


def _wait_file(path, content, timeout=60):
    """Wait for file to appear with expected content."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                with open(path) as f:
                    if content in f.read():
                        return True
            except:
                pass
        time.sleep(0.5)
    return False


# ══════════════════════════════════════════════════════════════════════════════
#  APP RUNNER DISPATCH
# ══════════════════════════════════════════════════════════════════════════════

def run_app(app, build_dir, smash_lib, quick):
    """Run benchmark for given app."""
    runners = {
        "sqlite": run_sqlite,
        "rocksdb": run_rocksdb,
        "duckdb": run_duckdb,
        "memcached": run_memcached,
        "redis": run_redis,
        "redis_ext": run_redis_ext,
    }
    runner = runners.get(app)
    if runner:
        return runner(build_dir, smash_lib, quick)
    return None


# ══════════════════════════════════════════════════════════════════════════════
#  ABLATION STUDY
# ══════════════════════════════════════════════════════════════════════════════

def run_ablation(build_dir, source_dir, apps, quick, output_dir, runs=1):
    """Run full ablation study."""
    results_path = output_dir / "ablation_results.json"
    smash_lib = build_dir / f"libsmash{LIB_SUFFIX}"

    all_results = {}
    if results_path.exists():
        all_results = json.loads(results_path.read_text())

    last_flags = None
    total = len(ABLATION_CONFIGS)
    t0 = time.time()

    for idx, (cid, cfg) in enumerate(ABLATION_CONFIGS.items(), 1):
        elapsed = time.time() - t0
        log(f"[{idx}/{total}] {cfg['name']} ({cid})  [{elapsed:.0f}s elapsed]")

        flags = cfg["cmake_flags"]
        if flags != last_flags and cfg["use_smash"]:
            log("Rebuilding libsmash...")
            if not rebuild(build_dir, flags, source_dir):
                log(f"SKIPPING {cid}: build failed", "ERROR")
                continue
            last_flags = flags

        for app in apps:
            if app in all_results and cid in all_results.get(app, {}):
                print(f"  {app}: cached")
                continue

            lib = smash_lib if cfg["use_smash"] else None
            run_results = []

            for run_num in range(1, runs + 1):
                print(f"  {app} run {run_num}...", end="", flush=True)
                try:
                    metrics = run_app(app, build_dir, lib, quick)
                    if metrics:
                        rss = metrics.get("rss_reduction_pct", "N/A")
                        print(f" rss_reduction={rss}")
                        run_results.append(metrics)
                    else:
                        print(" SKIP")
                        break
                except Exception as e:
                    print(f" ERROR: {e}")
                    break

            if run_results:
                if app not in all_results:
                    all_results[app] = {}
                all_results[app][cid] = {
                    "name": cfg["name"],
                    "runs": run_results,
                    "median": median_metrics(run_results),
                }

        results_path.write_text(json.dumps(all_results, indent=2))

    log(f"Ablation complete in {time.time() - t0:.0f}s")
    return all_results


# ══════════════════════════════════════════════════════════════════════════════
#  COMPRESS-ONLY EXPERIMENT
# ══════════════════════════════════════════════════════════════════════════════

def run_compress_only(build_dir, source_dir, apps, quick, output_dir, runs=1):
    """Run compress-only experiment."""
    results_path = output_dir / "compress_only_results.json"
    smash_lib = build_dir / f"libsmash{LIB_SUFFIX}"
    co_lib = build_dir / f"libsmash_compress_only{LIB_SUFFIX}"

    if not co_lib.exists():
        log(f"libsmash_compress_only not found, skipping compress-only experiment", "WARN")
        return {}

    rebuild(build_dir, {}, source_dir)

    all_results = {}
    if results_path.exists():
        all_results = json.loads(results_path.read_text())

    configs = [
        ("baseline", None),
        ("compress_only", co_lib),
        ("full_smash", smash_lib),
    ]

    t0 = time.time()
    for app in apps:
        log(f"Compress-only: {app}")

        for config_name, lib in configs:
            key = f"{app}_{config_name}"
            if key in all_results:
                print(f"  {config_name}: cached")
                continue

            run_results = []
            for run_num in range(1, runs + 1):
                print(f"  {config_name} run {run_num}...", end="", flush=True)
                try:
                    metrics = run_app(app, build_dir, lib, quick)
                    if metrics:
                        rss = metrics.get("rss_reduction_pct", 0)
                        print(f" reduction={rss:.1f}%")
                        run_results.append(metrics)
                    else:
                        print(" SKIP")
                        break
                except Exception as e:
                    print(f" ERROR: {e}")
                    break

            if run_results:
                all_results[key] = {
                    "runs": run_results,
                    "median": median_metrics(run_results),
                }

        results_path.write_text(json.dumps(all_results, indent=2))

    log(f"Compress-only complete in {time.time() - t0:.0f}s")
    return all_results


# ══════════════════════════════════════════════════════════════════════════════
#  ALGORITHM COMPARISON
# ══════════════════════════════════════════════════════════════════════════════

def run_algo_compare(build_dir, output_dir):
    """Run algorithm comparison benchmark."""
    exe = build_dir / "bench" / "bench_algo_compare"
    if not exe.exists():
        log("bench_algo_compare not found, skipping", "WARN")
        return None

    log("Running algorithm comparison...")
    try:
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        results_path = output_dir / "algo_compare_results.txt"
        results_path.write_text(r.stdout)
        log(f"Algorithm comparison saved to {results_path}")
        return r.stdout
    except Exception as e:
        log(f"Algorithm comparison error: {e}", "ERROR")
        return None


# ══════════════════════════════════════════════════════════════════════════════
#  ALLOCATOR COMPARISON
# ══════════════════════════════════════════════════════════════════════════════

def run_allocator_compare(build_dir, output_dir, quick):
    """Run allocator substrate comparison."""
    script = build_dir / "bench" / "bench_allocator_compare.py"
    if not script.exists():
        log("bench_allocator_compare.py not found, skipping", "WARN")
        return None

    log("Running allocator comparison...")
    args = ["python3", str(script),
            "--output", str(output_dir),
            "--runs", "1" if quick else "3",
            "--resume"]

    if quick:
        args.extend(["--count", "50000", "--sizes", "64,256,1024"])

    try:
        r = subprocess.run(args, cwd=build_dir, capture_output=True, text=True, timeout=1800)
        if r.returncode == 0:
            log("Allocator comparison complete")
        else:
            log(f"Allocator comparison had errors: {r.stderr[-500:]}", "WARN")
        return r.stdout
    except Exception as e:
        log(f"Allocator comparison error: {e}", "ERROR")
        return None


# ══════════════════════════════════════════════════════════════════════════════
#  TABLE GENERATION
# ══════════════════════════════════════════════════════════════════════════════

def generate_tables(output_dir, ablation_results, co_results, apps):
    """Generate paper tables."""
    tables_path = output_dir / "paper_tables.txt"

    with open(tables_path, "w") as f:
        f.write("=" * 70 + "\n")
        f.write("SMASH BENCHMARK RESULTS\n")
        f.write(f"Generated: {datetime.now().isoformat()}\n")
        f.write(f"Platform: {platform.system()} {platform.machine()}\n")
        f.write("=" * 70 + "\n\n")

        # Ablation summary
        if ablation_results:
            f.write("ABLATION STUDY - RSS Reduction (%)\n")
            f.write("-" * 70 + "\n")
            header = f"{'Config':<20}"
            for app in apps:
                header += f" {app:>10}"
            f.write(header + "\n")
            f.write("-" * 70 + "\n")

            for cid, cfg in ABLATION_CONFIGS.items():
                line = f"{cfg['name']:<20}"
                for app in apps:
                    v = ablation_results.get(app, {}).get(cid, {}).get("median", {}).get("rss_reduction_pct")
                    if v is not None:
                        line += f" {v:>10.1f}"
                    else:
                        line += f" {'---':>10}"
                f.write(line + "\n")
            f.write("\n")

        # Compress-only summary
        if co_results:
            f.write("COMPRESS-ONLY EXPERIMENT - RSS (MB)\n")
            f.write("-" * 70 + "\n")
            for app in apps:
                baseline = co_results.get(f"{app}_baseline", {}).get("median", {})
                co = co_results.get(f"{app}_compress_only", {}).get("median", {})
                full = co_results.get(f"{app}_full_smash", {}).get("median", {})

                base_rss = baseline.get("steady_rss_mb", baseline.get("peak_rss_mb", 0))
                co_rss = co.get("steady_rss_mb", 0)
                full_rss = full.get("steady_rss_mb", 0)

                f.write(f"{app}:\n")
                f.write(f"  baseline:      {base_rss:>8.1f} MB\n")
                f.write(f"  compress-only: {co_rss:>8.1f} MB\n")
                f.write(f"  full smash:    {full_rss:>8.1f} MB\n")
            f.write("\n")

    log(f"Tables saved to {tables_path}")


# ══════════════════════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Unified benchmark suite for Smash paper experiments",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("--quick", action="store_true",
                        help="Fast smoke test (smaller datasets, 1 run)")
    parser.add_argument("--full", action="store_true",
                        help="Paper-quality results (3 runs)")
    parser.add_argument("--runs", type=int, default=None,
                        help="Override number of runs per config")
    parser.add_argument("--build-dir", default=".",
                        help="Build directory (default: .)")
    parser.add_argument("--apps", default=None,
                        help="Comma-separated app list (default: auto-detect)")
    parser.add_argument("--skip-ablation", action="store_true")
    parser.add_argument("--skip-compress-only", action="store_true")
    parser.add_argument("--skip-algo", action="store_true")
    parser.add_argument("--skip-allocator", action="store_true")
    parser.add_argument("--fresh", action="store_true",
                        help="Clear cached results and start fresh")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    source_dir = Path(__file__).resolve().parent.parent
    output_dir = source_dir / "paper_results"
    output_dir.mkdir(exist_ok=True)

    # Determine runs
    if args.runs:
        runs = args.runs
    elif args.full:
        runs = 3
    else:
        runs = 1

    quick = args.quick and not args.full

    # Fresh start
    if args.fresh:
        for f in output_dir.glob("*.json"):
            f.unlink()
        log("Cleared cached results")

    # Detect available apps
    if args.apps:
        apps = [a.strip() for a in args.apps.split(",")]
    else:
        apps = []
        for app in APPS:
            if app == "sqlite" and (build_dir / "bench" / "bench_sqlite").exists():
                apps.append(app)
            elif app == "rocksdb" and (build_dir / "bench" / "bench_rocksdb").exists():
                apps.append(app)
            elif app == "duckdb" and check_binary("duckdb", build_dir):
                apps.append(app)
            elif app == "memcached" and check_binary("memcached", build_dir):
                apps.append(app)
            elif app in ("redis", "redis_ext") and check_binary("redis-server", build_dir):
                apps.append(app)

    log(f"Platform: {platform.system()} {platform.machine()}")
    log(f"Apps: {', '.join(apps)}")
    log(f"Build dir: {build_dir}")
    log(f"Output: {output_dir}")
    log(f"Mode: {'quick' if quick else 'full'} ({runs} runs)")
    print()

    ablation_results = {}
    co_results = {}

    # Run benchmarks
    if not args.skip_ablation:
        log("=" * 60)
        log("ABLATION STUDY")
        log("=" * 60)
        ablation_results = run_ablation(build_dir, source_dir, apps, quick, output_dir, runs)

    if not args.skip_compress_only:
        log("=" * 60)
        log("COMPRESS-ONLY EXPERIMENT")
        log("=" * 60)
        co_results = run_compress_only(build_dir, source_dir, apps, quick, output_dir, runs)

    if not args.skip_algo:
        log("=" * 60)
        log("ALGORITHM COMPARISON")
        log("=" * 60)
        run_algo_compare(build_dir, output_dir)

    if not args.skip_allocator:
        log("=" * 60)
        log("ALLOCATOR COMPARISON")
        log("=" * 60)
        run_allocator_compare(build_dir, output_dir, quick)

    # Generate summary tables
    generate_tables(output_dir, ablation_results, co_results, apps)

    log("=" * 60)
    log("ALL BENCHMARKS COMPLETE")
    log(f"Results in: {output_dir}")
    log("=" * 60)


if __name__ == "__main__":
    main()
