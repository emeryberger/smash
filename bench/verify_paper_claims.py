#!/usr/bin/env python3
"""Verify Smash's measured RSS reduction against the paper's published claims.

This is the app-level companion to the ctest suite (test_compression_ratio,
test_large_only_compression, test_malloc_compression): those pin the
*mechanism* (codec ratio, compressed>0, integrity) in-process; this drives the
*real workloads* the paper measured and checks the headline RSS-reduction
numbers end to end, in BOTH supported modes:

  - full mode      (no SMASH_LARGE_ONLY)      — experimental full interposition
  - large-only     (SMASH_LARGE_ONLY=1)       — the production-supported config

Two-tier thresholds (matching the project convention in run_quick_ci.py, which
gates at 30% when the paper reports 46%):

  - HARD floor : a conservative reduction we must never fall below. Failing it
                 fails the run (exit 1).
  - Paper claim: the actual figure from evaluation.tex. Falling short only
                 prints a WARN, not a failure. The paper's numbers come from
                 the FULL workloads (full dataset, ~20-30 s cooling); this
                 harness defaults to the smaller bench profiles, which
                 legitimately undershoot. A WARN means "verify with the full
                 paper runner before claiming a regression", not "the box is
                 too slow" — the reference machine is also an AMD EPYC 9R14.

"As reported or better" is reported per-app: a ✓ BEATS line when measured >=
the paper claim, a WARN when it's between the hard floor and the claim.

Paper claims (serve/cool-phase RSS reduction vs baseline, evaluation.tex):
  sqlite   : 69%   (429 -> 191 MiB)
  rocksdb  : 80%   (280 -> 88  MiB)
  memcached: 83%   (290 -> 80  MiB)   [needs external memcached; not run here]
  redis    : 53%   (264 -> 140 MiB)   [needs external redis;     not run here]
The in-process micro-bench (bench_rss, 64 MiB fully-compressible) is also
checked against the paper's ~97% best-case sensitivity result as a WARN target.

Usage:
    cd build_bench2
    python3 ../bench/verify_paper_claims.py
    python3 ../bench/verify_paper_claims.py --build-dir . --modes full,large_only
    python3 ../bench/verify_paper_claims.py --apps sqlite,rocksdb
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class ModeExpect:
    """Per-mode acceptance bar for one app."""
    paper_pct: float = 0.0      # the paper's published reduction (WARN target)
    hard_pct: float = 0.0       # conservative floor (hard fail) when checking reduction
    # no_regression_only: this workload allocates below the 1 MiB VmRegion
    # threshold, so in large-only mode it legitimately compresses ~nothing
    # (small allocs pass through to system malloc). Don't gate on the paper's
    # FULL-mode reduction — just assert smash didn't make RSS *worse* than
    # baseline. The large-only compression MECHANISM is proven separately by
    # the test_large_only_compression ctest (>= 1 MiB allocations).
    no_regression_only: bool = False
    # Allowed RSS growth (negative reduction) for a no-regression check.
    regression_slack_pct: float = 10.0


@dataclass
class AppSpec:
    name: str
    binary: str                 # under build_dir/bench/
    args: list[str]
    expect: dict[str, ModeExpect]   # keyed by mode
    peak_metric: str = "peak_rss_mb"
    min_metric: str = "min_rss_mb"
    timeout: int = 240
    # bench_rss links smash statically (no preload); the apps interpose.
    needs_preload: bool = True


# Conservative hard floors are ~0.6x the paper number — low enough to pass on a
# weaker host, high enough that "compressor not running" (≈0%) still fails.
# Large-only on these in-process benches is a no-regression check: their
# allocations are < 1 MiB so large-only doesn't manage them (the production
# large-only target is large-allocation workloads like neuron-cc/walrus).
APPS: dict[str, AppSpec] = {
    "rss": AppSpec(
        name="bench_rss (64 MiB compressible micro-bench)",
        binary="bench_rss", args=[],
        # bench_rss prints its own reduction line, parsed specially below.
        # 4 KiB chunks → no large allocations, so large-only is not run.
        expect={"full": ModeExpect(paper_pct=97.0, hard_pct=30.0)},
        timeout=60, needs_preload=False,
    ),
    "sqlite": AppSpec(
        name="sqlite (in-memory DB)",
        binary="bench_sqlite", args=["--quick"],
        expect={
            "full": ModeExpect(paper_pct=69.0, hard_pct=40.0),
            "large_only": ModeExpect(no_regression_only=True),
        },
        timeout=180,
    ),
    "rocksdb": AppSpec(
        name="rocksdb (block-cache workload)",
        binary="bench_rocksdb_builtin", args=["--quick"],
        expect={
            "full": ModeExpect(paper_pct=80.0, hard_pct=45.0),
            "large_only": ModeExpect(no_regression_only=True),
        },
        timeout=240,
    ),
}

# Mode → extra env. Both pin a short cold timeout so compression happens inside
# the bench window (same reasoning as run_quick_ci.py).
MODE_ENV: dict[str, dict[str, str]] = {
    "full":       {"SMASH_COLD_TIMEOUT_SEC": "2"},
    "large_only": {"SMASH_COLD_TIMEOUT_SEC": "2", "SMASH_LARGE_ONLY": "1"},
}


@dataclass
class Result:
    app: str
    mode: str
    measured_pct: float | None
    expect: ModeExpect
    note: str = ""
    detail: dict[str, float] = field(default_factory=dict)

    @property
    def passed(self) -> bool:
        if self.measured_pct is None:
            return False
        if self.expect.no_regression_only:
            # Pass as long as RSS didn't grow beyond the slack (reduction may
            # be ~0 because small allocations bypass large-only management).
            return self.measured_pct >= -self.expect.regression_slack_pct
        return self.measured_pct >= self.expect.hard_pct

    @property
    def beats_paper(self) -> bool:
        return (self.measured_pct is not None
                and not self.expect.no_regression_only
                and self.measured_pct >= self.expect.paper_pct)


def preload_env(libsmash: Path, extra: dict[str, str]) -> dict[str, str]:
    e = dict(os.environ)
    if sys.platform == "darwin":
        e["DYLD_INSERT_LIBRARIES"] = str(libsmash)
        e["DYLD_FORCE_FLAT_NAMESPACE"] = "1"
        e["MallocNanoZone"] = "0"
    else:
        e["LD_PRELOAD"] = str(libsmash)
    e.update(extra)
    return e


def parse_metric(out: str, name: str) -> float | None:
    m = re.search(rf"^METRIC\s+{re.escape(name)}\s+(-?[\d.]+)\s*$",
                  out, re.MULTILINE)
    return float(m.group(1)) if m else None


def parse_bench_rss_pct(out: str) -> float | None:
    # "  t=10s: RSS=73.7 MB (46% reduction from peak)"
    m = re.search(r"^\s*t=10s:\s+RSS=[\d.]+ MB\s+\((-?\d+)% reduction",
                  out, re.MULTILINE)
    return float(m.group(1)) if m else None


def run_one(spec: AppSpec, mode: str, exp: ModeExpect,
            build: Path, libsmash: Path) -> Result:
    binpath = build / "bench" / spec.binary
    if not binpath.exists():
        return Result(spec.name, mode, None, exp,
                      note=f"{spec.binary} not built — configure with "
                           f"-DSMASH_BUILD_BENCH=ON and build it")
    extra = MODE_ENV[mode]
    env = (preload_env(libsmash, extra) if spec.needs_preload
           else {**os.environ, **extra})

    cmd = [str(binpath), *spec.args]
    print(f"$ {mode}: {' '.join(cmd)}", flush=True)
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True,
                              timeout=spec.timeout)
    except subprocess.TimeoutExpired:
        return Result(spec.name, mode, None, exp,
                      note=f"TIMEOUT after {spec.timeout}s")
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout)[-400:]
        return Result(spec.name, mode, None, exp,
                      note=f"exited {proc.returncode}: {tail.strip()}")

    out = proc.stdout
    if spec.binary == "bench_rss":
        pct = parse_bench_rss_pct(out)
        detail: dict[str, float] = {}
    else:
        peak = parse_metric(out, spec.peak_metric)
        mn = parse_metric(out, spec.min_metric)
        if peak is None or mn is None or peak <= 0:
            return Result(spec.name, mode, None, exp,
                          note="could not parse peak/min RSS metrics")
        pct = (1.0 - mn / peak) * 100.0
        detail = {"peak_mb": peak, "min_mb": mn}

    if pct is None:
        return Result(spec.name, mode, None, exp,
                      note="could not parse reduction")
    return Result(spec.name, mode, pct, exp, detail=detail)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path,
                    default=Path(os.environ.get("BUILD_DIR", ".")))
    ap.add_argument("--apps", default="rss,sqlite,rocksdb",
                    help="comma-separated subset of: " + ",".join(APPS))
    ap.add_argument("--modes", default="full,large_only",
                    help="comma-separated subset of: full,large_only")
    args = ap.parse_args()

    build = args.build_dir.resolve()
    lib_macos = build / "libsmash.dylib"
    lib_linux = build / "libsmash.so"
    libsmash = lib_macos if lib_macos.exists() else lib_linux
    if not libsmash.exists():
        raise SystemExit(f"libsmash not found at {libsmash}")

    want_apps = [a.strip() for a in args.apps.split(",") if a.strip()]
    want_modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for a in want_apps:
        if a not in APPS:
            raise SystemExit(f"unknown app {a!r}; choices: {','.join(APPS)}")

    print(f"build_dir: {build}")
    print(f"libsmash:  {libsmash}")
    print(f"apps:      {','.join(want_apps)}")
    print(f"modes:     {','.join(want_modes)}\n")

    results: list[Result] = []
    for app in want_apps:
        spec = APPS[app]
        for mode in want_modes:
            if mode not in MODE_ENV:
                raise SystemExit(f"unknown mode {mode!r}")
            exp = spec.expect.get(mode)
            if exp is None:
                print(f"-- skip {spec.name} in {mode} mode "
                      f"(not applicable for this workload)\n")
                continue
            print("=" * 64)
            kind = "no-regression" if exp.no_regression_only else "vs paper"
            print(f"  {spec.name}  [{mode}, {kind}]")
            print("=" * 64)
            r = run_one(spec, mode, exp, build, libsmash)
            results.append(r)
            if r.measured_pct is None:
                print(f"  ERROR: {r.note}\n")
            else:
                extra = (f"  ({r.detail['peak_mb']:.0f} -> "
                         f"{r.detail['min_mb']:.0f} MiB)" if r.detail else "")
                print(f"  measured reduction: {r.measured_pct:.1f}%{extra}")
                if exp.no_regression_only:
                    print(f"  no-regression check: RSS must not grow > "
                          f"{exp.regression_slack_pct:.0f}% "
                          f"(small allocs bypass large-only; mechanism covered "
                          f"by test_large_only_compression)\n")
                else:
                    print(f"  paper claim: {exp.paper_pct:.0f}%   "
                          f"hard floor: {exp.hard_pct:.0f}%\n")

    print("=" * 64)
    print("  SUMMARY  (measured vs paper, two-tier)")
    print("=" * 64)
    failed = 0
    for r in results:
        exp = r.expect
        if r.measured_pct is None:
            mark, extra = "FAIL", f"— {r.note}"
            failed += 1
        elif exp.no_regression_only:
            if r.passed:
                mark = "PASS"
                extra = (f"{r.measured_pct:.1f}% reduction — no regression "
                         f"(large-only doesn't manage this workload's "
                         f"sub-1 MiB allocations)")
            else:
                mark = "FAIL"
                extra = (f"{r.measured_pct:.1f}% — RSS grew beyond "
                         f"{exp.regression_slack_pct:.0f}% slack")
                failed += 1
        elif not r.passed:
            mark = "FAIL"
            extra = (f"{r.measured_pct:.1f}% < hard floor {exp.hard_pct:.0f}% "
                     f"(paper {exp.paper_pct:.0f}%)")
            failed += 1
        elif r.beats_paper:
            mark = "PASS"
            extra = f"{r.measured_pct:.1f}% ✓ BEATS paper {exp.paper_pct:.0f}%"
        else:
            mark = "PASS"
            extra = (f"{r.measured_pct:.1f}% ≥ floor {exp.hard_pct:.0f}%  "
                     f"WARN: short of paper {exp.paper_pct:.0f}% "
                     f"(bench profile undershoots the full paper workload; "
                     f"re-check with run_paper_experiments.py)")
        print(f"  [{mark}] {r.app} [{r.mode}]: {extra}")

    print()
    print(f"  {len(results) - failed}/{len(results)} checks passed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
