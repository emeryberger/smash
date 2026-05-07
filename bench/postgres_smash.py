#!/usr/bin/env python3
"""Validate smash on PostgreSQL: a real workload that uses libc malloc
(via palloc / memory contexts), heavy fork pattern (postmaster + per-
connection backends), and a non-trivial heap (per-backend work memory
+ catalogs + planner state).

Caveat: shared_buffers is shm_open()/mmap MAP_SHARED — smash doesn't
track that (and shouldn't), so the headline RSS includes a chunk smash
can't touch. The compressible fraction is what each backend allocates
on its own (work_mem, sorts, hashes, intermediate result sets).

Usage:
    python3 bench/postgres_smash.py /path/to/libsmash.so
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from collections import defaultdict
from pathlib import Path


# ── tool discovery ──────────────────────────────────────────────────────────


def find_pg_tools() -> dict[str, str] | None:
    """Locate initdb / postgres / psql / pgbench. Prefer the apt-installed
    /usr/lib/postgresql/<ver>/bin/ over $PATH versions because the latter
    sometimes resolve to wrappers that exec further into other processes."""
    bindirs: list[Path] = []
    libdir = Path("/usr/lib/postgresql")
    if libdir.is_dir():
        for ver in sorted(libdir.iterdir(), key=lambda p: p.name, reverse=True):
            if (ver / "bin" / "postgres").is_file():
                bindirs.append(ver / "bin")
                break
    needed = ("initdb", "postgres", "psql", "pgbench", "pg_ctl")
    out: dict[str, str] = {}
    for name in needed:
        for d in bindirs:
            cand = d / name
            if cand.is_file():
                out[name] = str(cand)
                break
        else:
            found = shutil.which(name)
            if found:
                out[name] = found
    missing = [n for n in needed if n not in out]
    if missing:
        print(f"missing postgres tools: {missing}\n"
              f"on Ubuntu: sudo apt install postgresql postgresql-contrib",
              file=sys.stderr)
        return None
    return out


# ── postgres lifecycle ──────────────────────────────────────────────────────


def initdb(tools: dict[str, str], datadir: Path) -> None:
    print(f">>> initdb {datadir}")
    subprocess.run(
        [tools["initdb"], "-D", str(datadir),
         "--encoding=UTF8", "--locale=C", "--no-clean", "--auth=trust",
         "--username=smashuser"],
        check=True, capture_output=True)


def start_postgres(tools: dict[str, str], libsmash: str, datadir: Path,
                   port: int, log_path: Path,
                   work_mem_mb: int = 8) -> subprocess.Popen:
    env = os.environ.copy()
    env["LD_PRELOAD"] = os.path.abspath(libsmash)
    env["SMASH_BANNER"] = "1"
    env["SMASH_DEBUG"] = "1"
    env["SMASH_STATS"] = "1"
    env["SMASH_DEFER_PHASES_MS"] = "5000"
    env["SMASH_LARGE_ONLY"] = "0"
    # Place the Unix socket in our writable datadir; default
    # /var/run/postgresql is owned by the postgres user and unwritable.
    socket_dir = datadir.parent
    cmd = [
        tools["postgres"],
        "-D", str(datadir),
        "-p", str(port),
        "-c", f"unix_socket_directories={socket_dir}",
        "-c", "shared_buffers=64MB",   # Small — most heap should be per-backend
        "-c", f"work_mem={work_mem_mb}MB",
        "-c", "max_connections=20",
        "-c", "fsync=off",             # Speed up pgbench init
        "-c", "synchronous_commit=off",
        # Disable parallel workers so each query's memory shows up in
        # one backend rather than being split across worker processes.
        "-c", "max_parallel_workers_per_gather=0",
        "-c", "max_parallel_workers=0",
        "-c", "log_destination=stderr",
        "-c", "logging_collector=off",
        "-c", "log_min_messages=warning",
    ]
    print(f">>> launching postgres on port {port}, log → {log_path}")
    log = open(log_path, "wb")
    proc = subprocess.Popen(
        cmd, env=env, stdout=log, stderr=subprocess.STDOUT,
        preexec_fn=os.setsid)
    # Stash the log handle on the proc so we can close it later.
    proc._smash_log = log  # type: ignore[attr-defined]
    return proc


def wait_for_ready(tools: dict[str, str], port: int, sockdir: str,
                   timeout_s: int = 30) -> bool:
    psql = tools["psql"]
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        r = subprocess.run(
            [psql, "-h", sockdir, "-p", str(port),
             "-U", "smashuser", "-d", "postgres",
             "-tAc", "SELECT 1"],
            capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip() == "1":
            return True
        time.sleep(0.5)
    return False


def stop_postgres(proc: subprocess.Popen) -> None:
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception:
        pass
    log = getattr(proc, "_smash_log", None)
    if log:
        log.close()


# ── workload ────────────────────────────────────────────────────────────────


# An analytical pgbench script. Each "transaction" runs three queries
# that stress work_mem and create fresh MemoryContexts:
#   1. ORDER BY on the largest table — forces materialization + sort
#      of the entire pgbench_accounts table when work_mem is large
#      enough to avoid disk; otherwise hits external sort.
#   2. GROUP BY aggregation — hash agg materializes one entry per
#      branch, then aggregates over all accounts.
#   3. Hash join across two tables.
ANALYTICAL_SQL = """\
SELECT abalance, aid FROM pgbench_accounts ORDER BY abalance, aid LIMIT 1000;
SELECT bid, COUNT(*), AVG(abalance), MIN(abalance), MAX(abalance)
  FROM pgbench_accounts GROUP BY bid ORDER BY bid;
SELECT b.bid, COUNT(*) FILTER (WHERE a.abalance > 0) AS positive,
              SUM(a.abalance) AS total
  FROM pgbench_branches b JOIN pgbench_accounts a ON a.bid = b.bid
  GROUP BY b.bid;
"""


def run_pgbench(tools: dict[str, str], port: int, sockdir: str,
                scale: int, clients: int, dwell_sec: int,
                analytical: bool) -> None:
    pgbench = tools["pgbench"]
    psql = tools["psql"]
    # Create the test database.
    subprocess.run(
        [psql, "-h", sockdir, "-p", str(port),
         "-U", "smashuser", "-d", "postgres",
         "-c", "CREATE DATABASE smash_pgbench"],
        check=True, capture_output=True)
    print(f">>> pgbench -i -s {scale} (initialising schema)")
    subprocess.run(
        [pgbench, "-h", sockdir, "-p", str(port),
         "-U", "smashuser", "-i", "-s", str(scale), "smash_pgbench"],
        check=True, capture_output=True)
    if analytical:
        # Drop a custom script next to the datadir.
        script = Path(sockdir) / "analytical.sql"
        script.write_text(ANALYTICAL_SQL)
        mode_label = f"analytical (custom script: {script.name})"
        cmd = [pgbench, "-h", sockdir, "-p", str(port),
               "-U", "smashuser", "-n",  # -n: skip vacuum (analytical, not OLTP)
               "-c", str(clients), "-T", str(dwell_sec),
               "-f", str(script), "smash_pgbench"]
    else:
        mode_label = "default mixed (TPC-B-like OLTP)"
        cmd = [pgbench, "-h", sockdir, "-p", str(port),
               "-U", "smashuser", "-c", str(clients), "-T", str(dwell_sec),
               "smash_pgbench"]
    print(f">>> pgbench -c {clients} -T {dwell_sec}  ({mode_label})")
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode != 0:
        print(f"pgbench returned {r.returncode}: {r.stderr.strip()}",
              file=sys.stderr)


# ── parsing ────────────────────────────────────────────────────────────────


STATS_RE = re.compile(
    r"\[smash stats\](?:\s+\[?[0-9-]+ [0-9:]+\]?)?\s+pid=(\d+)\s+"
    r"committed=(\d+)\s+active=(\d+)\s+monitor=(\d+)\s+"
    r"compressing=(\d+)\s+compressed=(\d+)"
)
BANNER_RE = re.compile(r"\[smash\][^\n]*\bloaded\b\s+pid=(\d+)")


def analyse(log_path: Path) -> int:
    text = log_path.read_text(errors="replace") if log_path.exists() else ""
    banners = set(BANNER_RE.findall(text))
    by_pid: dict[int, dict[str, int]] = defaultdict(
        lambda: {"committed_max": 0, "compressed_max": 0, "samples": 0})
    for m in STATS_RE.finditer(text):
        pid = int(m.group(1))
        c = int(m.group(2))
        cz = int(m.group(6))
        d = by_pid[pid]
        d["committed_max"] = max(d["committed_max"], c)
        d["compressed_max"] = max(d["compressed_max"], cz)
        d["samples"] += 1

    print()
    print("=" * 78)
    print("RESULTS")
    print("=" * 78)
    print(f"distinct processes that loaded libsmash: {len(banners)}")
    print(f"processes that emitted [smash stats]:    {len(by_pid)}")
    if not by_pid:
        print()
        print("No stats — postgres processes may have exited too quickly,")
        print("or libsmash didn't load. Check the raw log.")
        return 1

    PAGE = 4096
    print()
    print(f"{'pid':>7}  {'samples':>7}  {'committed_max':>14}  "
          f"{'compressed_max':>15}  {'compressed_MB':>12}")
    sum_c = 0
    sum_cz = 0
    for pid in sorted(by_pid):
        d = by_pid[pid]
        c = d["committed_max"]
        cz = d["compressed_max"]
        sum_c += c
        sum_cz += cz
        print(f"{pid:>7}  {d['samples']:>7}  {c:>14}  {cz:>15}  "
              f"{cz * PAGE / (1024*1024):>11.1f}")

    sum_c_mb = sum_c * PAGE / (1024 * 1024)
    sum_cz_mb = sum_cz * PAGE / (1024 * 1024)
    print()
    print(f"aggregate (sum of per-process maxima):")
    print(f"  committed: {sum_c} pages ({sum_c_mb:.0f} MB)")
    print(f"  compressed: {sum_cz} pages ({sum_cz_mb:.0f} MB)")
    if sum_c > 0:
        print(f"  compressed/committed: {sum_cz/sum_c:.1%}")

    print()
    print("=" * 78)
    print("VERDICT")
    print("=" * 78)
    if sum_c < 256:
        print(f"⚠️  smash saw only {sum_c} pages — backends may have exited")
        print("    before the compressor could tick. Try a longer dwell or")
        print("    a more allocation-heavy workload (-S read-only is lighter;")
        print("    default mixed is heavier).")
        return 1
    if sum_cz / sum_c < 0.3:
        print(f"⚠️  smash saw {sum_c_mb:.0f} MB of postgres heap but only")
        print(f"    compressed {100*sum_cz/sum_c:.1f}%. Pages may be staying")
        print(f"    hot under continuous pgbench traffic. Try a cooling phase")
        print(f"    after pgbench (kill the clients, leave postgres idle for")
        print(f"    SMASH_COLD_TIMEOUT_SEC*3 seconds).")
        return 1
    print(f"✅ smash saw {sum_c_mb:.0f} MB of postgres heap and compressed")
    print(f"   {sum_cz_mb:.0f} MB ({100*sum_cz/sum_c:.1f}%). System-malloc")
    print(f"   workload validated.")
    return 0


# ── entrypoint ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("libsmash", help="Path to libsmash.so")
    ap.add_argument("--port", type=int, default=5499)
    ap.add_argument("--scale", type=int, default=10,
                    help="pgbench scale factor (10 ≈ 150 MB schema; "
                         "use 50+ for analytical mode)")
    ap.add_argument("--clients", type=int, default=4)
    ap.add_argument("--dwell", type=int, default=60,
                    help="pgbench duration in seconds")
    ap.add_argument("--analytical", action="store_true",
                    help="Use a sort/aggregate/join workload (stresses "
                         "work_mem and creates fresh MemoryContexts) "
                         "instead of pgbench's default OLTP. Recommended "
                         "for smash validation since OLTP barely allocates.")
    ap.add_argument("--work-mem-mb", type=int, default=None,
                    help="postgres work_mem in MB (default 8 mixed / 128 analytical)")
    args = ap.parse_args()
    work_mem_mb = args.work_mem_mb or (128 if args.analytical else 8)

    if sys.platform != "linux":
        print("Linux-only.", file=sys.stderr)
        return 2
    if not Path(args.libsmash).exists():
        print(f"libsmash not found: {args.libsmash}", file=sys.stderr)
        return 2
    tools = find_pg_tools()
    if tools is None:
        return 2

    print(f">>> libsmash : {args.libsmash}")
    for name, p in tools.items():
        print(f">>> {name:<10}: {p}")

    workdir = Path(tempfile.mkdtemp(prefix="smash-pg-"))
    datadir = workdir / "pgdata"
    log_path = workdir / "postgres-smash.log"

    proc: subprocess.Popen | None = None
    sockdir = str(workdir)  # postgres socket lives next to datadir
    try:
        initdb(tools, datadir)
        proc = start_postgres(tools, args.libsmash, datadir,
                              args.port, log_path,
                              work_mem_mb=work_mem_mb)
        if not wait_for_ready(tools, args.port, sockdir, timeout_s=30):
            print("postgres did not become ready in 30s; check the log",
                  file=sys.stderr)
            return 1
        run_pgbench(tools, args.port, sockdir,
                    args.scale, args.clients, args.dwell,
                    analytical=args.analytical)
    finally:
        if proc is not None:
            print(">>> stopping postgres")
            stop_postgres(proc)

    rc = analyse(log_path)
    print(f"\n>>> full log saved to {log_path}")
    print(f">>> data dir was {datadir} (delete with: rm -rf {workdir})")
    return rc


if __name__ == "__main__":
    sys.exit(main())
