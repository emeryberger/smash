#!/usr/bin/env python3
"""Download PostgreSQL source, patch AllocSet to be a thin malloc/free
passthrough (à la "Reconsidering Custom Memory Allocation",
Berger/Zorn/McKinley OOPSLA 2002), build, and run pgbench under
LD_PRELOAD=libsmash.so.

The shim replaces palloc's chunk-pooling AllocSet with a per-call
malloc / free. Every palloc therefore reaches our LD_PRELOAD wrapper
and smash sees the real allocation traffic.

Usage:
    python3 bench/postgres_shim_build.py /path/to/libsmash.so

Builds into /tmp/smash-pg-shim/install/. Skips re-download/build if
already present (delete the dir to force rebuild).
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
import urllib.request
from collections import defaultdict
from pathlib import Path


PG_VERSION = "16.6"
PG_TARBALL = f"postgresql-{PG_VERSION}.tar.bz2"
PG_URL = f"https://ftp.postgresql.org/pub/source/v{PG_VERSION}/{PG_TARBALL}"

WORKDIR = Path("/tmp/smash-pg-shim")
SRCDIR = WORKDIR / f"postgresql-{PG_VERSION}"
INSTDIR = WORKDIR / "install"


# ── the shim patch ──────────────────────────────────────────────────────────
#
# AllocSetAlloc / AllocSetFree / AllocSetRealloc normally pool malloc'd
# chunks across allocations; we replace each with a direct malloc/free/
# realloc. We KEEP the chunk-header (so the type byte still routes
# free()s correctly) but skip the freelist + block-batching machinery.
#
# Diff strategy: insert an early-return at the top of each function that
# does the simple thing. The pooling code below stays in the file but
# is unreachable, which keeps the patch tiny + reversible.

PATCH_MARKER = "/* SMASH-SHIM */"

ASET_PATCHES = [
    # AllocSetAlloc: replace the pooling implementation with a passthrough.
    {
        "anchor": "AllocSetAlloc(MemoryContext context, Size size, int flags)\n{",
        "insert": """
\t/* SMASH-SHIM: skip chunk pooling; route every palloc to libc malloc
\t * so LD_PRELOAD'd allocators (smash, ASan, etc.) see every request.
\t * Header layout matches AllocSet's so AllocSetFree/Realloc still work. */
\t{
\t\tvoid *raw = malloc(size + ALLOC_CHUNKHDRSZ);
\t\tif (raw == NULL) {
\t\t\tif (flags & MCXT_ALLOC_NO_OOM) return NULL;
\t\t\telog(ERROR, "out of memory (smash-shim malloc failed for %zu)", (size_t) size);
\t\t}
\t\tMemoryChunk *chunk = (MemoryChunk *) raw;
\t\tMemoryChunkSetHdrMaskExternal(chunk, MCTX_ASET_ID);
\t\tvoid *user = MemoryChunkGetPointer(chunk);
\t\t((AllocSet) context)->header.mem_allocated += size + ALLOC_CHUNKHDRSZ;
\t\tif (flags & MCXT_ALLOC_ZERO) memset(user, 0, size);
\t\treturn user;
\t}
""",
    },
    # AllocSetFree: skip the freelist; just free.
    {
        "anchor": "AllocSetFree(void *pointer)\n{",
        "insert": """
\t/* SMASH-SHIM: skip freelist; release directly to libc free. */
\t{
\t\tMemoryChunk *chunk = PointerGetMemoryChunk(pointer);
\t\tfree(chunk);
\t\treturn;
\t}
""",
    },
    # AllocSetRealloc: replace with libc realloc.
    {
        "anchor": "AllocSetRealloc(void *pointer, Size size, int flags)\n{",
        "insert": """
\t/* SMASH-SHIM: skip the in-place / shrink fast paths; route to realloc. */
\t{
\t\tMemoryChunk *chunk = PointerGetMemoryChunk(pointer);
\t\tvoid *raw = realloc(chunk, size + ALLOC_CHUNKHDRSZ);
\t\tif (raw == NULL) {
\t\t\tif (flags & MCXT_ALLOC_NO_OOM) return NULL;
\t\t\telog(ERROR, "out of memory (smash-shim realloc failed for %zu)", (size_t) size);
\t\t}
\t\tMemoryChunk *newchunk = (MemoryChunk *) raw;
\t\tMemoryChunkSetHdrMaskExternal(newchunk, MCTX_ASET_ID);
\t\treturn MemoryChunkGetPointer(newchunk);
\t}
""",
    },
]


# ── shell helpers ───────────────────────────────────────────────────────────


def hr(label: str) -> None:
    print()
    print("=" * 78)
    print(label)
    print("=" * 78)


def sh(cmd: list[str], cwd: Path | None = None,
       env: dict[str, str] | None = None,
       quiet: bool = False) -> subprocess.CompletedProcess:
    if not quiet:
        print(f"$ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=cwd, env=env, check=True)


# ── stages ──────────────────────────────────────────────────────────────────


def stage_download() -> None:
    hr(f"download postgresql-{PG_VERSION} source")
    if SRCDIR.is_dir():
        print(f"already extracted at {SRCDIR}")
        return
    WORKDIR.mkdir(parents=True, exist_ok=True)
    tarball = WORKDIR / PG_TARBALL
    if not tarball.is_file():
        print(f"fetching {PG_URL}")
        urllib.request.urlretrieve(PG_URL, tarball)
    print(f"extracting {tarball.name}")
    sh(["tar", "-xjf", str(tarball)], cwd=WORKDIR)


def stage_patch() -> None:
    hr("apply SMASH-SHIM patch to aset.c")
    aset = SRCDIR / "src" / "backend" / "utils" / "mmgr" / "aset.c"
    text = aset.read_text()
    if PATCH_MARKER in text:
        print("aset.c already patched.")
        return
    new = text
    for p in ASET_PATCHES:
        if p["anchor"] not in new:
            print(f"!! anchor not found, skipping: {p['anchor'][:40]!r}",
                  file=sys.stderr)
            continue
        new = new.replace(p["anchor"], p["anchor"] + p["insert"], 1)
    if new == text:
        sys.exit("no patches applied — anchors didn't match. Aborting.")
    aset.write_text(new)
    print(f"patched {aset.relative_to(SRCDIR)}")


def clean_build_env() -> dict[str, str]:
    """Strip any inherited CFLAGS / LDFLAGS / LD_PRELOAD / etc. from the
    environment we hand to configure + make. The user's interactive shell
    can have leftovers from earlier smash testing (LD_PRELOAD pointing at
    libsmash, CFLAGS containing a stray Firefox temp dir, …) that confuse
    autoconf and gcc."""
    drop = {"CFLAGS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS", "LIBS",
            "LD_PRELOAD", "LD_LIBRARY_PATH",
            "DYLD_INSERT_LIBRARIES", "DYLD_FORCE_FLAT_NAMESPACE",
            "MallocNanoZone",
            "SMASH_BANNER", "SMASH_DEBUG", "SMASH_STATS",
            "SMASH_TRACK_EXTERNAL", "SMASH_LARGE_ONLY",
            "SMASH_DEFER_PHASES_MS", "SMASH_COLD_TIMEOUT_SEC"}
    return {k: v for k, v in os.environ.items() if k not in drop}


def stage_configure_build() -> None:
    hr("configure + make + make install (postgres-shim)")
    env = clean_build_env()
    if (INSTDIR / "bin" / "postgres").is_file():
        # Do a quick incremental rebuild in case aset.c changed since last time.
        print(f"installed binary already exists at {INSTDIR}/bin/postgres")
        # Force re-make of just the patched file to pick up edits
        sh(["make", "-C", "src/backend/utils/mmgr", "-j", str(os.cpu_count() or 4)],
           cwd=SRCDIR, env=env, quiet=True)
        sh(["make", "-j", str(os.cpu_count() or 4)], cwd=SRCDIR, env=env, quiet=True)
        sh(["make", "install"], cwd=SRCDIR, env=env, quiet=True)
        return
    INSTDIR.mkdir(parents=True, exist_ok=True)
    sh(["./configure",
        f"--prefix={INSTDIR}",
        "--without-readline", "--without-zlib", "--without-icu",
        "--without-llvm",
        "--enable-debug",
        "CFLAGS=-O2"],
       cwd=SRCDIR, env=env)
    sh(["make", "-j", str(os.cpu_count() or 4)], cwd=SRCDIR, env=env)
    sh(["make", "install"], cwd=SRCDIR, env=env)
    # contrib/pgbench
    sh(["make", "-C", "contrib/pgbench", "install"], cwd=SRCDIR, env=env)


def stage_run(libsmash: Path) -> int:
    """Initialise + start the patched postgres under smash, run pgbench."""
    hr("init + run pgbench under LD_PRELOAD=libsmash.so")
    bindir = INSTDIR / "bin"
    initdb = bindir / "initdb"
    postgres = bindir / "postgres"
    psql = bindir / "psql"
    pgbench = bindir / "pgbench"
    for b in (initdb, postgres, psql, pgbench):
        if not b.is_file():
            sys.exit(f"missing: {b}")

    workdir = Path(tempfile.mkdtemp(prefix="smash-pg-shim-run-"))
    datadir = workdir / "pgdata"
    log_path = workdir / "postgres.log"
    sockdir = workdir
    port = 5497

    print(f">>> initdb {datadir}")
    sh([str(initdb), "-D", str(datadir),
        "--encoding=UTF8", "--locale=C",
        "--auth=trust", "--username=smashuser"], quiet=True)

    env = os.environ.copy()
    env["LD_PRELOAD"] = str(libsmash.resolve())
    env["SMASH_BANNER"] = "1"
    env["SMASH_DEBUG"] = "1"
    env["SMASH_STATS"] = "1"
    env["SMASH_DEFER_PHASES_MS"] = "5000"
    env["SMASH_LARGE_ONLY"] = "0"

    cmd = [str(postgres), "-D", str(datadir), "-p", str(port),
           "-c", f"unix_socket_directories={sockdir}",
           "-c", "shared_buffers=64MB",
           "-c", "work_mem=128MB",
           "-c", "max_parallel_workers_per_gather=0",
           "-c", "max_parallel_workers=0",
           "-c", "fsync=off", "-c", "synchronous_commit=off",
           "-c", "log_min_messages=warning"]
    print(f">>> launching shimmed postgres (logs → {log_path})")
    log = open(log_path, "wb")
    proc = subprocess.Popen(
        cmd, env=env, stdout=log, stderr=subprocess.STDOUT,
        preexec_fn=os.setsid)

    # Wait for ready.
    deadline = time.time() + 30
    ready = False
    while time.time() < deadline:
        r = subprocess.run(
            [str(psql), "-h", str(sockdir), "-p", str(port),
             "-U", "smashuser", "-d", "postgres", "-tAc", "SELECT 1"],
            capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip() == "1":
            ready = True
            break
        time.sleep(0.5)
    if not ready:
        print("postgres did not start in 30s; check log:")
        print(log_path.read_text()[-2000:])
        try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception: pass
        return 1

    try:
        print(">>> pgbench setup")
        sh([str(psql), "-h", str(sockdir), "-p", str(port),
            "-U", "smashuser", "-d", "postgres",
            "-c", "CREATE DATABASE smash"], quiet=True)
        sh([str(pgbench), "-h", str(sockdir), "-p", str(port),
            "-U", "smashuser", "-i", "-s", "20", "smash"], quiet=True)
        # An analytical script: sort + group-by + hash join.
        analytical = workdir / "analytical.sql"
        analytical.write_text("""\
SELECT abalance, aid FROM pgbench_accounts ORDER BY abalance, aid LIMIT 1000;
SELECT bid, COUNT(*), AVG(abalance) FROM pgbench_accounts GROUP BY bid;
SELECT b.bid, COUNT(*), SUM(a.abalance) FROM pgbench_branches b
  JOIN pgbench_accounts a ON a.bid = b.bid GROUP BY b.bid;
""")
        print(">>> pgbench analytical workload, 90s")
        r = subprocess.run(
            [str(pgbench), "-h", str(sockdir), "-p", str(port),
             "-U", "smashuser", "-n", "-c", "2", "-T", "90",
             "-f", str(analytical), "smash"],
            capture_output=True, text=True)
        print(r.stdout.strip())
    finally:
        print(">>> stopping postgres")
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=15)
        except Exception:
            try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except Exception: pass
        log.close()

    return analyse(log_path)


# ── log analysis ────────────────────────────────────────────────────────────


STATS_RE = re.compile(
    r"\[smash stats\](?:\s+\[?[0-9-]+ [0-9:]+\]?)?\s+pid=(\d+)\s+"
    r"committed=(\d+)\s+active=\d+\s+monitor=\d+\s+"
    r"compressing=\d+\s+compressed=(\d+)"
)


def analyse(log_path: Path) -> int:
    text = log_path.read_text(errors="replace") if log_path.exists() else ""
    by_pid: dict[int, dict[str, int]] = defaultdict(
        lambda: {"c": 0, "z": 0, "n": 0})
    for m in STATS_RE.finditer(text):
        pid = int(m.group(1))
        by_pid[pid]["c"] = max(by_pid[pid]["c"], int(m.group(2)))
        by_pid[pid]["z"] = max(by_pid[pid]["z"], int(m.group(3)))
        by_pid[pid]["n"] += 1
    hr("RESULTS")
    if not by_pid:
        print("no stats captured; check the log.")
        return 1
    print(f"{'pid':>7}  {'samples':>7}  {'committed':>10}  {'compressed':>10}  {'compressed_MB':>12}")
    PAGE = 4096
    sum_c = sum_z = 0
    for pid in sorted(by_pid):
        d = by_pid[pid]
        sum_c += d["c"]; sum_z += d["z"]
        print(f"{pid:>7}  {d['n']:>7}  {d['c']:>10}  {d['z']:>10}  "
              f"{d['z']*PAGE/1024/1024:>11.1f}")
    print()
    sum_c_mb = sum_c * PAGE / 1024 / 1024
    sum_z_mb = sum_z * PAGE / 1024 / 1024
    print(f"aggregate: committed={sum_c} pages ({sum_c_mb:.0f} MB), "
          f"compressed={sum_z} pages ({sum_z_mb:.0f} MB)")
    if sum_c > 0:
        print(f"           compressed/committed = {sum_z/sum_c:.1%}")
    if sum_c < 256:
        print()
        print("⚠️  smash still saw very little. The shim may not have taken")
        print(f"    effect — verify aset.c has the SMASH-SHIM marker:")
        print(f"      grep -c SMASH-SHIM {SRCDIR}/src/backend/utils/mmgr/aset.c")
        return 1
    if sum_z / max(sum_c, 1) >= 0.3:
        print(f"\n✅ Shimmed postgres + smash: real compression activity.")
        return 0
    print(f"\nsmash saw the heap but compression ratio is low; might be a")
    print(f"hot-pages issue. Try a longer dwell with idle period at the end.")
    return 0


# ── entrypoint ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("libsmash", help="Path to libsmash.so")
    ap.add_argument("--clean", action="store_true",
                    help=f"Wipe {WORKDIR} before starting (forces re-download/build)")
    args = ap.parse_args()

    if sys.platform != "linux":
        print("Linux-only.", file=sys.stderr); return 2
    libsmash = Path(args.libsmash)
    if not libsmash.exists():
        print(f"libsmash not found: {libsmash}", file=sys.stderr); return 2

    if args.clean and WORKDIR.exists():
        print(f">>> rm -rf {WORKDIR}")
        shutil.rmtree(WORKDIR)

    stage_download()
    stage_patch()
    stage_configure_build()
    return stage_run(libsmash)


if __name__ == "__main__":
    sys.exit(main())
