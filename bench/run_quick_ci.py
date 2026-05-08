#!/usr/bin/env python3
"""Quick regression-spotting benchmark runner for CI.

Two end-to-end checks against built-in benches (no external services):

  bench_rss     — allocates 64 MiB of compressible data, sleeps 10 s,
                  expects the in-process compressor to reclaim a
                  meaningful fraction of peak RSS. Catches regressions
                  in: compressor not starting, fault handler broken,
                  page state machine wedged, ROI rejecting everything.

  bench_sqlite  — fills an in-memory SQLite DB with ~250 K text rows
                  under libsmash via DYLD_INSERT_LIBRARIES (macOS) or
                  LD_PRELOAD (Linux), measures cooling-phase RSS.
                  Catches regressions in the malloc-interposed path
                  on a realistic compressible workload.

Thresholds are set well below observed locals (46 % rss, 13 % sqlite)
so the CI is signal, not noise. If a real regression drops the numbers
below threshold the build fails.

Usage:
    cd build
    python3 ../bench/run_quick_ci.py
    # or with explicit paths:
    python3 ../bench/run_quick_ci.py --build-dir ./build
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# Conservative thresholds. Locals on M-class macOS:
#   bench_rss          : ~46 % reduction
#   bench_sqlite cool  : ~13 % reduction
# CI runners may differ; thresholds are well below local numbers.
RSS_REDUCTION_MIN_PCT = 30.0
SQLITE_COOLING_MIN_PCT = 5.0


def run(cmd: list[str], env: dict[str, str] | None = None,
        timeout: int = 120) -> str:
    print(f"$ {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True,
                          timeout=timeout)
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    if proc.returncode != 0:
        raise SystemExit(f"FAIL: {cmd[0]} exited {proc.returncode}")
    return proc.stdout


def smash_env(libsmash: Path, base_env: dict[str, str]) -> dict[str, str]:
    e = dict(base_env)
    if sys.platform == "darwin":
        e["DYLD_INSERT_LIBRARIES"] = str(libsmash)
        e["DYLD_FORCE_FLAT_NAMESPACE"] = "1"
        e["MallocNanoZone"] = "0"
    else:
        e["LD_PRELOAD"] = str(libsmash)
    return e


def check_bench_rss(build_dir: Path) -> tuple[bool, str]:
    """bench_rss is linked against smash directly, no preload needed."""
    # Pin the cold-tick floor to its default (= no override means no
    # CPU-pressure multiplier kicks in). Without this, on a busy CI runner
    # the multiplier would push the floor up to 32 s and the 10 s
    # bench_rss window wouldn't see any compression.
    env = dict(os.environ)
    env.setdefault("SMASH_COLD_TIMEOUT_SEC", "2")
    out = run([str(build_dir / "bench" / "bench_rss")], env=env)
    # Match "  t=10s: RSS=73.7 MB (46% reduction from peak)"
    m = re.search(r"^\s*t=10s:\s+RSS=[\d.]+ MB\s+\((\-?\d+)% reduction",
                  out, re.MULTILINE)
    if not m:
        return False, "could not parse t=10s reduction line"
    pct = int(m.group(1))
    ok = pct >= RSS_REDUCTION_MIN_PCT
    return ok, f"bench_rss t=10s reduction {pct}% (need ≥ {RSS_REDUCTION_MIN_PCT:.0f}%)"


def check_bench_sqlite(build_dir: Path, libsmash: Path) -> tuple[bool, str]:
    """bench_sqlite uses system malloc; smash interposes via preload."""
    env = smash_env(libsmash, dict(os.environ))
    # Same cold-tick pin as bench_rss — see comment there.
    env.setdefault("SMASH_COLD_TIMEOUT_SEC", "2")
    out = run([str(build_dir / "bench" / "bench_sqlite"), "--quick"],
              env=env, timeout=180)
    # bench_sqlite emits structured "METRIC cool_reduction_pct N.N" lines —
    # use them, they're stable across format tweaks to the human-readable
    # output above.
    m = re.search(r"^METRIC\s+cool_reduction_pct\s+(\-?[\d.]+)\s*$",
                  out, re.MULTILINE)
    if not m:
        return False, "could not find METRIC cool_reduction_pct line"
    pct = float(m.group(1))
    ok = pct >= SQLITE_COOLING_MIN_PCT
    return ok, (f"bench_sqlite cooling reduction {pct:.1f}% "
                f"(need ≥ {SQLITE_COOLING_MIN_PCT:.0f}%)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", type=Path,
                    default=Path(os.environ.get("BUILD_DIR", "build")))
    args = ap.parse_args()

    build = args.build_dir.resolve()
    libsmash_macos = build / "libsmash.dylib"
    libsmash_linux = build / "libsmash.so"
    libsmash = libsmash_macos if libsmash_macos.exists() else libsmash_linux
    if not libsmash.exists():
        raise SystemExit(f"libsmash not found at {libsmash}")
    if not (build / "bench" / "bench_rss").exists():
        raise SystemExit("bench_rss not built — configure with -DSMASH_BUILD_BENCH=ON")
    if not (build / "bench" / "bench_sqlite").exists():
        raise SystemExit("bench_sqlite not built — configure with -DSMASH_BUILD_BENCH=ON")

    print(f"build_dir: {build}")
    print(f"libsmash:  {libsmash}")
    print()

    results: list[tuple[bool, str]] = []
    print("=" * 60)
    print("bench_rss")
    print("=" * 60)
    results.append(check_bench_rss(build))
    print()

    print("=" * 60)
    print("bench_sqlite --quick")
    print("=" * 60)
    results.append(check_bench_sqlite(build, libsmash))
    print()

    print("=" * 60)
    print("Summary")
    print("=" * 60)
    failed = 0
    for ok, msg in results:
        mark = "PASS" if ok else "FAIL"
        print(f"  [{mark}] {msg}")
        if not ok:
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
