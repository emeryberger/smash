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
SRCDIR = WORKDIR / f"postgresql-{PG_VERSION}"          # shim source (patched)
INSTDIR = WORKDIR / "install"                          # shim binaries
SRCDIR_STOCK = WORKDIR / f"postgresql-{PG_VERSION}-stock"  # unpatched source
INSTDIR_STOCK = WORKDIR / "install-stock"              # unpatched binaries


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

# PG 16's AllocSetAlloc/Free/Realloc take 2 args (no `int flags`); flag-handling
# lives in the wrapper MemoryContextAllocExtended above. Note the patcher's
# idempotency check looks for this exact marker — every inserted block must
# contain it verbatim, otherwise re-runs will re-patch the file.
#
# The shim mimics AllocSet's existing "external chunk" code path (used today
# for >allocChunkLimit allocations): one malloc per request, with the layout
#   [AllocBlockData header][MemoryChunk hdr][user bytes]
# This matters because AllocSetGetChunkContext / AllocSetRealloc / AllocSetFree
# walk back from the user pointer to the block via ExternalChunkGetBlock and
# read block->aset. A bare malloc(size+chunkhdr) crashes those paths.
PATCH_MARKER = "/* SMASH-SHIM:"

ASET_PATCHES = [
    # AllocSetAlloc: route every palloc to libc malloc as a one-chunk block.
    {
        "anchor": "AllocSetAlloc(MemoryContext context, Size size)\n{",
        "insert": """
\t/* SMASH-SHIM: skip chunk pooling; allocate a one-chunk external block via
\t * libc malloc so LD_PRELOAD'd allocators (smash, ASan, etc.) see every
\t * request. Layout matches AllocSetAlloc's existing large-chunk path:
\t * [AllocBlockData][MemoryChunk hdr][user bytes]. */
\t{
\t\tAllocSet _set = (AllocSet) context;
\t\tSize _chunk_size = MAXALIGN(size);
\t\tSize _blksize = _chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
\t\tAllocBlock _block = (AllocBlock) malloc(_blksize);
\t\tif (_block == NULL)
\t\t\treturn NULL;
\t\tcontext->mem_allocated += _blksize;
\t\t_block->aset = _set;
\t\t_block->freeptr = _block->endptr = ((char *) _block) + _blksize;
\t\tMemoryChunk *_chunk = (MemoryChunk *) (((char *) _block) + ALLOC_BLOCKHDRSZ);
\t\tMemoryChunkSetHdrMaskExternal(_chunk, MCTX_ASET_ID);
\t\t/* Link under set->blocks so AllocSetReset/Delete free us. */
\t\tif (_set->blocks != NULL) {
\t\t\t_block->prev = _set->blocks;
\t\t\t_block->next = _set->blocks->next;
\t\t\tif (_block->next) _block->next->prev = _block;
\t\t\t_set->blocks->next = _block;
\t\t} else {
\t\t\t_block->prev = NULL;
\t\t\t_block->next = NULL;
\t\t\t_set->blocks = _block;
\t\t}
\t\treturn MemoryChunkGetPointer(_chunk);
\t}
""",
    },
    # AllocSetFree: unlink block, libc free.
    {
        "anchor": "AllocSetFree(void *pointer)\n{",
        "insert": """
\t/* SMASH-SHIM: external-chunk free: unlink block, libc free. */
\t{
\t\tMemoryChunk *_chunk = PointerGetMemoryChunk(pointer);
\t\tAllocBlock _block = ExternalChunkGetBlock(_chunk);
\t\tAllocSet _set = _block->aset;
\t\t_set->header.mem_allocated -= _block->endptr - ((char *) _block);
\t\tif (_block->prev)
\t\t\t_block->prev->next = _block->next;
\t\telse
\t\t\t_set->blocks = _block->next;
\t\tif (_block->next)
\t\t\t_block->next->prev = _block->prev;
\t\tfree(_block);
\t\treturn;
\t}
""",
    },
    # AllocSetRealloc: libc realloc, fix up block + neighbour links.
    {
        "anchor": "AllocSetRealloc(void *pointer, Size size)\n{",
        "insert": """
\t/* SMASH-SHIM: external-chunk realloc. */
\t{
\t\tMemoryChunk *_oldchunk = PointerGetMemoryChunk(pointer);
\t\tAllocBlock _oldblock = ExternalChunkGetBlock(_oldchunk);
\t\tAllocSet _set = _oldblock->aset;
\t\tSize _oldblksize = _oldblock->endptr - ((char *) _oldblock);
\t\tSize _chunk_size = MAXALIGN(size);
\t\tSize _newblksize = _chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
\t\tAllocBlock _newblock = (AllocBlock) realloc(_oldblock, _newblksize);
\t\tif (_newblock == NULL)
\t\t\treturn NULL;
\t\t_set->header.mem_allocated += _newblksize - _oldblksize;
\t\t_newblock->freeptr = _newblock->endptr = ((char *) _newblock) + _newblksize;
\t\t/* If the block moved, fix the neighbour pointers so list stays sane. */
\t\tif (_newblock != _oldblock) {
\t\t\tif (_newblock->prev)
\t\t\t\t_newblock->prev->next = _newblock;
\t\t\telse
\t\t\t\t_set->blocks = _newblock;
\t\t\tif (_newblock->next)
\t\t\t\t_newblock->next->prev = _newblock;
\t\t}
\t\tMemoryChunk *_newchunk = (MemoryChunk *) (((char *) _newblock) + ALLOC_BLOCKHDRSZ);
\t\tMemoryChunkSetHdrMaskExternal(_newchunk, MCTX_ASET_ID);
\t\treturn MemoryChunkGetPointer(_newchunk);
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


def stage_download(srcdir: Path = SRCDIR) -> None:
    hr(f"download postgresql-{PG_VERSION} source ({srcdir.name})")
    if srcdir.is_dir():
        print(f"already extracted at {srcdir}")
        return
    WORKDIR.mkdir(parents=True, exist_ok=True)
    tarball = WORKDIR / PG_TARBALL
    if not tarball.is_file():
        print(f"fetching {PG_URL}")
        urllib.request.urlretrieve(PG_URL, tarball)
    print(f"extracting {tarball.name} → {srcdir.name}")
    # tar -xjf creates "postgresql-X.Y" by default; for the stock build we
    # extract into a temp dir then rename to the desired srcdir name.
    if srcdir.name == f"postgresql-{PG_VERSION}":
        sh(["tar", "-xjf", str(tarball)], cwd=WORKDIR)
    else:
        tmp = Path(tempfile.mkdtemp(dir=WORKDIR, prefix="extract-"))
        sh(["tar", "-xjf", str(tarball), "-C", str(tmp)])
        (tmp / f"postgresql-{PG_VERSION}").rename(srcdir)
        shutil.rmtree(tmp)


def stage_patch(srcdir: Path = SRCDIR) -> None:
    hr(f"apply SMASH-SHIM patch to aset.c ({srcdir.name})")
    aset = srcdir / "src" / "backend" / "utils" / "mmgr" / "aset.c"
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
    print(f"patched {aset.relative_to(srcdir)}")


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
            "PROFILE",  # PG Makefile appends $(PROFILE) to CFLAGS/LDFLAGS
            "SMASH_BANNER", "SMASH_DEBUG", "SMASH_STATS",
            "SMASH_TRACK_EXTERNAL", "SMASH_LARGE_ONLY",
            "SMASH_DEFER_PHASES_MS", "SMASH_COLD_TIMEOUT_SEC"}
    return {k: v for k, v in os.environ.items() if k not in drop}


def stage_configure_build(srcdir: Path = SRCDIR, instdir: Path = INSTDIR,
                          label: str = "shim") -> None:
    hr(f"configure + make + make install (postgres-{label})")
    env = clean_build_env()
    if (instdir / "bin" / "postgres").is_file():
        # Do a quick incremental rebuild in case aset.c changed since last time.
        print(f"installed binary already exists at {instdir}/bin/postgres")
        # Force re-make of just the patched file to pick up edits
        sh(["make", "-C", "src/backend/utils/mmgr", "-j", str(os.cpu_count() or 4)],
           cwd=srcdir, env=env, quiet=True)
        sh(["make", "-j", str(os.cpu_count() or 4)], cwd=srcdir, env=env, quiet=True)
        sh(["make", "install"], cwd=srcdir, env=env, quiet=True)
        return
    instdir.mkdir(parents=True, exist_ok=True)
    sh([f"{srcdir}/configure",
        f"--prefix={instdir}",
        "--without-readline", "--without-zlib", "--without-icu",
        "--without-llvm",
        "--enable-debug",
        "CFLAGS=-O2"],
       cwd=srcdir, env=env)
    sh(["make", "-j", str(os.cpu_count() or 4)], cwd=srcdir, env=env)
    sh(["make", "install"], cwd=srcdir, env=env)
    # pgbench moved out of contrib/ in PG 16; `make install` covers src/bin/pgbench.


def build_zipfian_sql(queries: int, zipf_s: float, scale: int,
                      cool_sec: int, with_full_join: bool = True) -> str:
    """Generate the workload script.
    cool_sec=0          → no in-connection sleep
    with_full_join=True → append one full-table JOIN at the end (heavy; useful
                          for the cool workload's plan-cache fill, harmful for
                          the perf workload where each transaction repeats it)
    """
    naccounts = scale * 100_000
    zipfian_block = (
        f"\\set aid random_zipfian(1, {naccounts}, {zipf_s})\n"
        "SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
        "SELECT bid, COUNT(*), AVG(abalance) FROM pgbench_accounts "
        "WHERE aid BETWEEN :aid AND :aid + 1000 GROUP BY bid;\n"
    )
    sql = (
        f"-- Zipfian queries, parameter={zipf_s}, repetitions={queries}\n"
        + zipfian_block * queries
    )
    if with_full_join:
        sql += ("\n-- one full-table aggregate for plan-cache fill\n"
                "SELECT b.bid, COUNT(*), SUM(a.abalance) FROM pgbench_branches b "
                "JOIN pgbench_accounts a ON a.bid = b.bid GROUP BY b.bid;\n")
    if cool_sec > 0:
        sql += (f"\n-- cool phase: backend idles {cool_sec}s in same connection\n"
                f"SELECT pg_sleep({cool_sec});\n")
    return sql


# Parsed pgbench output.
PGBENCH_TPS_RE = re.compile(r"tps\s*=\s*([\d.]+)")
PGBENCH_LATENCY_RE = re.compile(r"latency average\s*=\s*([\d.]+)\s*ms")
PGBENCH_LATENCY_STDDEV_RE = re.compile(r"latency stddev\s*=\s*([\d.]+)\s*ms")


def stage_run(instdir: Path,
              libsmash: Path | None = None,
              preload: Path | None = None,
              preload_label: str = "",
              workload: str = "cool",
              clients: int = 4,
              queries: int = 200,
              cool_sec: int = 60,
              zipf_s: float = 1.2,
              scale: int = 20,
              perf_duration_sec: int = 60,
              quiet_pgbench: bool = False) -> dict:
    """Initialise + start postgres, run pgbench, return parsed stats.

    Returns a dict with: mode (label), tps, latency_avg_ms, latency_stddev_ms,
    smash_committed_pages, smash_compressed_pages, log_path.

    If `libsmash` is set, that's the smash LD_PRELOAD path AND smash-specific
    env vars (SMASH_BANNER, SMASH_DEBUG, ...) are set. If `preload` is set
    instead, it's a generic LD_PRELOAD (e.g. jemalloc) with no smash env. At
    most one of `libsmash` / `preload` should be non-None.
    """
    label_mode = ("shim" if instdir == INSTDIR else "stock")
    if libsmash:
        label_alloc = "smash"
    elif preload:
        label_alloc = preload_label or preload.stem
    else:
        label_alloc = "libc"
    label = f"{label_mode}/{label_alloc}/{workload}"
    hr(f"run {label}")

    bindir = instdir / "bin"
    initdb = bindir / "initdb"
    postgres = bindir / "postgres"
    psql = bindir / "psql"
    pgbench = bindir / "pgbench"
    for b in (initdb, postgres, psql, pgbench):
        if not b.is_file():
            sys.exit(f"missing: {b}")

    workdir = Path(tempfile.mkdtemp(prefix=f"smash-pg-{label_mode}-run-"))
    datadir = workdir / "pgdata"
    log_path = workdir / "postgres.log"
    sockdir = workdir
    port = 5497

    print(f">>> initdb {datadir}")
    sh([str(initdb), "-D", str(datadir),
        "--encoding=UTF8", "--locale=C",
        "--auth=trust", "--username=smashuser"], quiet=True)

    env = os.environ.copy()
    # Always strip stale smash env from the user's shell so a cross-config
    # lifetime can't accidentally re-enable it.
    for k in list(env):
        if k.startswith("SMASH_"):
            del env[k]
    if libsmash:
        env["LD_PRELOAD"] = str(libsmash.resolve())
        env["SMASH_BANNER"] = "1"
        env["SMASH_DEBUG"] = "1"
        env["SMASH_STATS"] = "1"
        env["SMASH_DEFER_PHASES_MS"] = "5000"
        env["SMASH_LARGE_ONLY"] = "0"
    elif preload:
        env["LD_PRELOAD"] = str(preload.resolve())
    else:
        env.pop("LD_PRELOAD", None)

    cmd = [str(postgres), "-D", str(datadir), "-p", str(port),
           "-c", f"unix_socket_directories={sockdir}",
           # Unix socket only — TCP collides with leftover postmasters from
           # earlier runs (pgbench connects via -h <socket-dir>).
           "-c", "listen_addresses=",
           "-c", "shared_buffers=64MB",
           "-c", "work_mem=128MB",
           "-c", "max_parallel_workers_per_gather=0",
           "-c", "max_parallel_workers=0",
           # Autovacuum + logical-replication launcher fork short-lived
           # workers periodically. With smash's atfork compressor restart in
           # play, those forks race against the child's exit and a small
           # fraction of them SIGSEGV under load. Disable them so the
           # per-backend numbers reflect the workload, not housekeeping noise.
           # Also keep stock/shim runs symmetric — same server config across
           # all comparison points.
           "-c", "autovacuum=off",
           "-c", "max_wal_senders=0",
           "-c", "fsync=off", "-c", "synchronous_commit=off",
           "-c", "log_min_messages=warning"]
    print(f">>> launching {label_mode} postgres (logs → {log_path})")
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
        return {"mode": label, "ok": False}

    pgbench_stdout = ""
    timed_out = False
    try:
        if not quiet_pgbench:
            print(">>> pgbench setup")
        sh([str(psql), "-h", str(sockdir), "-p", str(port),
            "-U", "smashuser", "-d", "postgres",
            "-c", "CREATE DATABASE smash"], quiet=True)
        sh([str(pgbench), "-h", str(sockdir), "-p", str(port),
            "-U", "smashuser", "-i", "-s", str(scale), "smash"], quiet=True)

        if workload == "cool":
            # One transaction per client: heavy work + 60 s in-connection
            # sleep. Stays in one connection so the per-backend MemoryContext
            # ages out — that's what smash's compressor needs.
            sql = build_zipfian_sql(queries, zipf_s, scale, cool_sec)
            script = workdir / "workload_cool.sql"
            script.write_text(sql)
            label_sub = (f"Zipfian (s={zipf_s}, {queries} q/client) "
                         f"+ {cool_sec}s cool, clients={clients}")
            args = ["-c", str(clients), "-j", str(min(clients, 4)),
                    "-t", "1", "-f", str(script), "smash"]
        elif workload == "perf":
            # Throughput measurement: same Zipfian shape, no full-table join,
            # no sleep tail. Use `-T` so we get a stable TPS over a fixed
            # wall-clock interval. Each transaction is one zipfian point
            # lookup + one 1000-row range aggregate; pgbench repeats this for
            # `perf_duration_sec` and reports tps + latency.
            sql = build_zipfian_sql(queries=1, zipf_s=zipf_s, scale=scale,
                                    cool_sec=0, with_full_join=False)
            script = workdir / "workload_perf.sql"
            script.write_text(sql)
            label_sub = (f"Zipfian perf (s={zipf_s}, "
                         f"-T={perf_duration_sec}s, clients={clients})")
            args = ["-c", str(clients), "-j", str(min(clients, 4)),
                    "-T", str(perf_duration_sec),
                    "-f", str(script), "smash"]
        else:
            sys.exit(f"unknown workload: {workload}")

        if not quiet_pgbench:
            print(f">>> pgbench {label_sub}")
        # `-r` enables per-command latency reporting AND ensures the trailing
        # "latency stddev = X ms" line appears in pgbench's summary even when
        # the session is short.
        # Hard timeout = 4 × the requested -T (or 4 × the cool tail), so a
        # pathological shim+smash run that gets stuck in a fault-on-every-page
        # loop can't hang the whole comparison sweep. The result is recorded
        # as ok=False / tps=NaN; the per-config table just shows it skipped.
        budget = (perf_duration_sec if workload == "perf"
                  else cool_sec + queries // 50 + 30)
        timeout_sec = max(60, 4 * budget)
        pgbench_stdout = ""
        timed_out = False
        try:
            r = subprocess.run(
                [str(pgbench), "-h", str(sockdir), "-p", str(port),
                 "-U", "smashuser", "-n", "-r", *args],
                capture_output=True, text=True, timeout=timeout_sec)
            pgbench_stdout = r.stdout.strip()
            if not quiet_pgbench:
                print(pgbench_stdout)
                if r.stderr.strip():
                    print(r.stderr.strip())
        except subprocess.TimeoutExpired as e:
            pgbench_stdout = (e.stdout or b"").decode(errors="replace").strip()
            timed_out = True
            print(f"[timeout] pgbench did not finish in {timeout_sec}s; "
                  f"killing this config and moving on.")
    finally:
        if not quiet_pgbench:
            print(">>> stopping postgres")
        # Cleanup is harder than it looks. Two complications:
        # 1. Postgres backends call setsid() per connection, leaving the
        #    postmaster's process group — killpg(postmaster_pgid) misses them.
        # 2. Backends rewrite their cmdline via setproctitle to
        #    "postgres: smashuser smash [local] SELECT", so `pkill -f` against
        #    the install path doesn't catch them either.
        # Match on "smashuser" (our unique DB role) plus the install paths;
        # one of those will always hit each leaked process.
        patterns = [str(pgbench), str(postgres), "smashuser"]
        if timed_out:
            sig = signal.SIGKILL
            wait_after = 0
        else:
            sig = signal.SIGTERM
            wait_after = 8
        for pat in patterns:
            subprocess.run(["pkill", f"-{sig.value}", "-f", pat],
                           capture_output=True)
        # Give postgres a moment to drain (only when we sent SIGTERM).
        if wait_after:
            try: proc.wait(timeout=wait_after)
            except Exception: pass
        # Belt-and-suspenders: SIGKILL anything left.
        for pat in patterns:
            subprocess.run(["pkill", "-9", "-f", pat], capture_output=True)
        try: proc.wait(timeout=2)
        except Exception: pass
        log.close()

    # Parse pgbench output.
    tps = float(PGBENCH_TPS_RE.search(pgbench_stdout).group(1)) \
        if PGBENCH_TPS_RE.search(pgbench_stdout) else float("nan")
    lat_avg = float(PGBENCH_LATENCY_RE.search(pgbench_stdout).group(1)) \
        if PGBENCH_LATENCY_RE.search(pgbench_stdout) else float("nan")
    lat_std_m = PGBENCH_LATENCY_STDDEV_RE.search(pgbench_stdout)
    lat_std = float(lat_std_m.group(1)) if lat_std_m else float("nan")

    # Smash stats (only meaningful when libsmash != None).
    sum_committed = sum_compressed = 0
    if libsmash and log_path.exists():
        text = log_path.read_text(errors="replace")
        by_pid: dict[int, dict[str, int]] = defaultdict(
            lambda: {"c": 0, "z": 0, "n": 0})
        for m in STATS_RE.finditer(text):
            pid = int(m.group(1))
            by_pid[pid]["c"] = max(by_pid[pid]["c"], int(m.group(2)))
            by_pid[pid]["z"] = max(by_pid[pid]["z"], int(m.group(3)))
        sum_committed = sum(d["c"] for d in by_pid.values())
        sum_compressed = sum(d["z"] for d in by_pid.values())

    # Each run leaves a ~240 MB pgdata directory in /tmp. With 5 configs × 5
    # runs that's 6 GB and fills small VM /tmp partitions, causing later runs
    # to fail with "No space left on device" mid-pgbench-init. Drop pgdata
    # but keep the postgres.log we already parsed in case anyone wants to
    # inspect smash stats afterwards.
    try:
        if datadir.is_dir():
            shutil.rmtree(datadir)
    except Exception as exc:
        print(f"[warn] could not remove {datadir}: {exc}")

    return {
        "mode": label,
        "ok": True,
        "tps": tps,
        "latency_avg_ms": lat_avg,
        "latency_stddev_ms": lat_std,
        "smash_committed_pages": sum_committed,
        "smash_compressed_pages": sum_compressed,
        "log_path": str(log_path),
    }


def stage_run_legacy(libsmash: Path, **kw) -> int:
    """Backward-compat wrapper for the old single-run-with-stats path."""
    res = stage_run(INSTDIR, libsmash=libsmash, workload="cool", **kw)
    if not res.get("ok"):
        return 1
    return analyse(Path(res["log_path"]))


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
    # "Long-lived" = lived through several compressor ticks (samples > 5).
    # That excludes pgbench-init's loader and other one-shot helpers that
    # exit before the compressor's first scan and so always show
    # compressed=0 — they would otherwise drag down the headline ratio
    # without being representative of what smash can do on warm-then-cold
    # heap memory.
    long_c = long_z = 0
    for pid in sorted(by_pid):
        d = by_pid[pid]
        sum_c += d["c"]; sum_z += d["z"]
        if d["n"] > 5:
            long_c += d["c"]; long_z += d["z"]
        print(f"{pid:>7}  {d['n']:>7}  {d['c']:>10}  {d['z']:>10}  "
              f"{d['z']*PAGE/1024/1024:>11.1f}")
    print()
    sum_c_mb = sum_c * PAGE / 1024 / 1024
    sum_z_mb = sum_z * PAGE / 1024 / 1024
    print(f"aggregate (all pids):    committed={sum_c} pages ({sum_c_mb:.0f} MB), "
          f"compressed={sum_z} pages ({sum_z_mb:.0f} MB), "
          f"ratio={sum_z/max(sum_c,1):.1%}")
    long_c_mb = long_c * PAGE / 1024 / 1024
    long_z_mb = long_z * PAGE / 1024 / 1024
    print(f"long-lived (samples>5):  committed={long_c} pages ({long_c_mb:.0f} MB), "
          f"compressed={long_z} pages ({long_z_mb:.0f} MB), "
          f"ratio={long_z/max(long_c,1):.1%}")
    if sum_c < 256:
        print()
        print("⚠️  smash still saw very little. The shim may not have taken")
        print(f"    effect — verify aset.c has the SMASH-SHIM marker:")
        print(f"      grep -c SMASH-SHIM {SRCDIR}/src/backend/utils/mmgr/aset.c")
        return 1
    if long_z / max(long_c, 1) >= 0.5:
        print(f"\n✅ Shimmed postgres + smash: long-lived backends "
              f"compress {long_z/max(long_c,1):.0%} of their heap.")
        return 0
    print(f"\nsmash saw the heap. long-lived ratio is the meaningful number "
          f"({long_z/max(long_c,1):.0%}); the all-pids aggregate also counts "
          f"transient backends that exit before the compressor ticks. Try "
          f"--cool=120 to let the compressor sweep more thoroughly, or --zipf=1.5 "
          f"for stronger access skew.")
    return 0


# ── entrypoint ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("libsmash", help="Path to libsmash.so")
    ap.add_argument("--clean", action="store_true",
                    help=f"Wipe {WORKDIR} before starting (forces re-download/build)")
    ap.add_argument("--clients", type=int, default=4,
                    help="pgbench clients (default 4)")
    ap.add_argument("--queries", type=int, default=200,
                    help="Zipfian-skewed queries per client before the cool tail (default 200)")
    ap.add_argument("--cool", type=int, default=60, dest="cool_sec",
                    help="seconds each backend idles after the work (default 60). The "
                         "smash compressor needs at least kColdTicks * tick_interval (~20 s) "
                         "of idle to start compressing; longer = more pages reach the COMPRESSED state")
    ap.add_argument("--zipf", type=float, default=1.2, dest="zipf_s",
                    help="random_zipfian parameter (>0, !=1; default 1.2). Higher = more skew, so "
                         "more rows are touched only rarely and stay cold")
    ap.add_argument("--scale", type=int, default=20,
                    help="pgbench --scale; affects pgbench_accounts size (default 20 → 2M rows)")
    ap.add_argument("--mode",
                    choices=["shim-smash", "shim", "stock", "compare"],
                    default="shim-smash",
                    help="What to run. shim-smash (default): patched postgres + LD_PRELOAD libsmash, "
                         "cool workload — measures compression. shim: patched postgres only, no smash, "
                         "perf workload — measures cost of the malloc-passthrough. stock: unpatched "
                         "postgres, no smash, perf workload — true baseline. compare: run all three "
                         "with the perf workload and print a side-by-side table.")
    ap.add_argument("--perf-duration", type=int, default=60,
                    help="seconds for the perf workload (pgbench -T). Ignored for cool workload.")
    ap.add_argument("--runs", type=int, default=1,
                    help="repetitions per mode for --mode=compare (median is reported)")
    args = ap.parse_args()

    if sys.platform != "linux":
        print("Linux-only.", file=sys.stderr); return 2
    libsmash = Path(args.libsmash)
    if not libsmash.exists():
        print(f"libsmash not found: {libsmash}", file=sys.stderr); return 2

    if args.clean and WORKDIR.exists():
        print(f">>> rm -rf {WORKDIR}")
        shutil.rmtree(WORKDIR)

    # Always make the shim build available — it's needed by every mode except
    # `stock`. `compare` needs both.
    needs_shim = args.mode in ("shim-smash", "shim", "compare")
    needs_stock = args.mode in ("stock", "compare")

    if needs_shim:
        stage_download(SRCDIR)
        stage_patch(SRCDIR)
        stage_configure_build(SRCDIR, INSTDIR, label="shim")
    if needs_stock:
        stage_download(SRCDIR_STOCK)
        # No stage_patch — that's the whole point.
        stage_configure_build(SRCDIR_STOCK, INSTDIR_STOCK, label="stock")

    common = dict(clients=args.clients, queries=args.queries,
                  cool_sec=args.cool_sec, zipf_s=args.zipf_s,
                  scale=args.scale, perf_duration_sec=args.perf_duration)

    if args.mode == "shim-smash":
        # Old default behaviour: cool workload + smash, dump the analysis.
        res = stage_run(INSTDIR, libsmash=libsmash, workload="cool", **common)
        if not res["ok"]: return 1
        return analyse(Path(res["log_path"]))
    if args.mode == "shim":
        res = stage_run(INSTDIR, libsmash=None, workload="perf", **common)
        return 0 if res["ok"] else 1
    if args.mode == "stock":
        res = stage_run(INSTDIR_STOCK, libsmash=None, workload="perf", **common)
        return 0 if res["ok"] else 1
    if args.mode == "compare":
        return run_compare(libsmash, runs=args.runs, **common)
    return 2


def find_jemalloc() -> Path | None:
    """Look for libjemalloc.so on standard Linux paths. Returns None if not
    installed (the jemalloc rows just get reported as 'no successful runs')."""
    candidates = [
        "/usr/lib/aarch64-linux-gnu/libjemalloc.so.2",
        "/usr/lib/x86_64-linux-gnu/libjemalloc.so.2",
        "/usr/lib/libjemalloc.so.2",
        "/usr/local/lib/libjemalloc.so.2",
    ]
    for p in candidates:
        if Path(p).exists():
            return Path(p)
    # Fall back to ldconfig cache.
    try:
        out = subprocess.run(["ldconfig", "-p"], capture_output=True,
                             text=True, check=False).stdout
        for line in out.splitlines():
            if "libjemalloc.so" in line and "=>" in line:
                return Path(line.split("=>")[-1].strip())
    except Exception:
        pass
    return None


def run_compare(libsmash: Path, runs: int = 1, **common) -> int:
    """Run stock / shim / shim+smash with the perf workload, print a table.

    Adds jemalloc (LD_PRELOAD'd libjemalloc.so) variants of stock + shim too,
    so we can isolate three orthogonal axes:
      - palloc pooling: stock keeps it, shim removes it.
      - malloc impl: libc / jemalloc / smash.
      - smash compression: only `shim+smash` gets it.
    """
    jemalloc = find_jemalloc()
    if jemalloc is None:
        print("[warn] libjemalloc.so not found — jemalloc configs will be skipped")
    # (label, instdir, libsmash, preload, preload_label)
    configs = [
        ("stock",          INSTDIR_STOCK, None,     None,     ""),
        ("stock+jemalloc", INSTDIR_STOCK, None,     jemalloc, "jemalloc"),
        ("shim",           INSTDIR,       None,     None,     ""),
        ("shim+jemalloc",  INSTDIR,       None,     jemalloc, "jemalloc"),
        ("shim+smash",     INSTDIR,       libsmash, None,     ""),
    ]
    results: dict[str, list[dict]] = {label: [] for label, *_ in configs}
    for r in range(runs):
        for label, instdir, lib, preload, preload_label in configs:
            if preload is None and preload_label == "jemalloc":
                # jemalloc not installed; skip.
                continue
            print(f"\n=== run {r+1}/{runs} — {label} ===")
            res = stage_run(instdir, libsmash=lib,
                            preload=preload, preload_label=preload_label,
                            workload="perf", quiet_pgbench=True, **common)
            if not res.get("ok"):
                print(f"  FAILED, see log: {res.get('log_path','?')}")
                continue
            print(f"  tps={res['tps']:.1f}  "
                  f"latency_avg={res['latency_avg_ms']:.2f} ms  "
                  f"latency_stddev={res['latency_stddev_ms']:.2f} ms")
            results[label].append(res)

    hr("COMPARISON SUMMARY")
    print(f"{'config':<16} {'runs':>4} {'tps_med':>10} {'tps_min':>10} {'tps_max':>10} "
          f"{'lat_avg_med':>12} {'lat_std_med':>12}")
    base_tps = None
    for label, *_ in configs:
        rs = results[label]
        if not rs:
            print(f"{label:<16} {'0':>4}  (no successful runs)")
            continue
        tpsv = sorted(r["tps"] for r in rs)
        latv = sorted(r["latency_avg_ms"] for r in rs)
        # latency_stddev_ms can be NaN if pgbench didn't emit it; filter them.
        stdv = sorted(r["latency_stddev_ms"] for r in rs
                      if r["latency_stddev_ms"] == r["latency_stddev_ms"])  # !=NaN
        med = lambda v: v[len(v)//2] if v else float("nan")
        tps_med = med(tpsv)
        if label == "stock":
            base_tps = tps_med
        delta = ""
        if base_tps and tps_med:
            d = (tps_med - base_tps) / base_tps * 100
            delta = f"  ({d:+.1f}% vs stock)"
        print(f"{label:<16} {len(rs):>4} {tps_med:>10.1f} {tpsv[0]:>10.1f} "
              f"{tpsv[-1]:>10.1f} {med(latv):>12.2f} {med(stdv):>12.2f}{delta}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
