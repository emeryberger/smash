#!/usr/bin/env python3
"""Confirm whether libsmash's `mmap` wrapper is actually being called
when Firefox runs under LD_PRELOAD.

Background: firefox_strace_diag.py showed Firefox makes ~5000 anonymous
mmap calls but smash's committed-page count stayed at 4. Two possible
causes:
  A) The mmap symbol is bypassed entirely (glibc's mmap64@GLIBC_X.Y
     is what Firefox actually links against, not the unversioned mmap
     we export). Our wrapper never runs.
  B) The wrapper runs but registerLinuxExternalRange filters most calls
     out (currently requires MAP_ANONYMOUS|PROT_WRITE and rejects
     PROT_NONE reservations).

This script distinguishes A from B:
  1. Patches src/linux_syscall_wrappers.cpp to add a stderr print at the
     top of the `mmap` wrapper.
  2. Rebuilds libsmash.
  3. Runs Firefox briefly under that build.
  4. Restores the unpatched source (in a try/finally so Ctrl-C doesn't
     leave the tree dirty).
  5. Reports the count of `[smash mmap]` lines.

Outcome:
  - count > 0  → our wrapper IS being called, issue is in
                 registerLinuxExternalRange (filter or page-state setup).
  - count == 0 → the symbol is bypassed entirely; need to also export
                 mmap64 / __mmap64 like we did for fstat / fstat64.

Usage:
    python3 bench/firefox_mmap_probe.py /path/to/firefox-bin
    # assumes the smash repo root is the script's parent's parent
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE = REPO_ROOT / "src" / "linux_syscall_wrappers.cpp"
BUILD_DIR = REPO_ROOT / "linux-build"
LIBSMASH = BUILD_DIR / "libsmash.so"

# Inject this stderr print at the top of the mmap wrapper body.
# Use static n-counter so we don't drown in output (Firefox makes
# thousands of mmap calls).
PATCH_FROM = (
    "SMASH_VISIBLE void* mmap(void* addr, size_t len, int prot, "
    "int flags, int fd, off_t offset) {\n"
)
PATCH_TO = (
    "SMASH_VISIBLE void* mmap(void* addr, size_t len, int prot, "
    "int flags, int fd, off_t offset) {\n"
    "    { static int _smash_mmap_probe_n = 0;\n"
    "      if (_smash_mmap_probe_n++ < 20)\n"
    "          fprintf(stderr, \"[smash mmap] called: len=%zu prot=0x%x flags=0x%x\\n\","
    " len, prot, flags); }\n"
)


def patch_source() -> str:
    """Insert the probe print. Return the original text for rollback."""
    text = SOURCE.read_text()
    if PATCH_FROM not in text:
        sys.exit(f"Could not find mmap wrapper signature in {SOURCE}.\n"
                 "Did the wrapper signature change? Inspect the file.")
    if "_smash_mmap_probe_n" in text:
        sys.exit("Source already patched — bail out and clean up first.")
    SOURCE.write_text(text.replace(PATCH_FROM, PATCH_TO, 1))
    return text


def rebuild() -> None:
    if not BUILD_DIR.exists():
        sys.exit(f"{BUILD_DIR} does not exist — run cmake first:\n"
                 f"  mkdir {BUILD_DIR.name} && cd {BUILD_DIR.name} && "
                 f"cmake .. && make -j")
    print(">>> rebuilding libsmash with the probe…")
    subprocess.run(
        ["make", "-j", str(os.cpu_count() or 4), "smash"],
        cwd=BUILD_DIR, check=True)
    if not LIBSMASH.exists():
        sys.exit(f"Build did not produce {LIBSMASH}")


def kill_firefoxes() -> None:
    me = os.getpid()
    parents = {me, os.getppid()}
    victims = []
    for p in Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        pid = int(p.name)
        if pid in parents:
            continue
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue
        base = os.path.basename(exe)
        if base in ("firefox", "firefox-bin", "crashhelper") or "/firefox/" in exe:
            victims.append(pid)
    for pid in victims:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    if victims:
        time.sleep(2)
        for pid in victims:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass


def run_firefox(firefox: str, dwell_sec: int) -> tuple[int, str]:
    """Run firefox under the patched libsmash. Return (mmap-line count, log)."""
    profile = tempfile.mkdtemp(prefix="smash-mmap-probe-")
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(LIBSMASH)
    env["SMASH_BANNER"] = "1"
    env["SMASH_TRACK_EXTERNAL"] = "1"
    env["SMASH_LARGE_ONLY"] = "0"
    env["MOZ_DISABLE_CONTENT_SANDBOX"] = "1"
    cmd = [firefox, "--headless", "--no-remote", "--profile", profile,
           "https://example.com"]
    print(f">>> launching firefox under probed libsmash for {dwell_sec}s")
    proc = subprocess.Popen(
        cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        preexec_fn=os.setsid)
    time.sleep(dwell_sec)
    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    try:
        out_b, _ = proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        out_b, _ = proc.communicate()
    out = out_b.decode("utf-8", errors="replace")
    n = sum(1 for line in out.splitlines() if line.startswith("[smash mmap]"))
    return n, out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firefox", help="Path to firefox or firefox-bin")
    ap.add_argument("--dwell", type=int, default=15,
                    help="Seconds to let firefox run (default 15)")
    args = ap.parse_args()

    if sys.platform != "linux":
        print("Linux-only.", file=sys.stderr)
        return 2
    if not Path(args.firefox).exists():
        print(f"firefox not found: {args.firefox}", file=sys.stderr)
        return 2
    if "/snap/" in os.path.realpath(args.firefox):
        print(f"refuse to run snap firefox; use a tarball.", file=sys.stderr)
        return 2

    # 1. Patch source (saving original for rollback).
    print(f">>> patching {SOURCE.relative_to(REPO_ROOT)}")
    original = patch_source()

    try:
        # 2. Rebuild.
        rebuild()

        # 3. Kill leftover firefoxes, run.
        kill_firefoxes()
        n, log = run_firefox(args.firefox, args.dwell)

        # 4. Show first few hits + total.
        print()
        print("=" * 78)
        print("RESULTS")
        print("=" * 78)
        print(f"`[smash mmap]` lines emitted: {n}")
        if n > 0:
            sample = [l for l in log.splitlines()
                      if l.startswith("[smash mmap]")][:5]
            for l in sample:
                print(f"  {l}")
            print()
            print("VERDICT: our mmap wrapper IS being called.")
            print("Issue must be in registerLinuxExternalRange — either")
            print("the filter (MAP_ANONYMOUS|PROT_WRITE only) is too strict")
            print("for mozjemalloc's PROT_NONE-then-mprotect pattern, or")
            print("the page-state setup isn't recording the registrations.")
            print()
            print("Look for [smash] / [smash debug] alongside [smash mmap]")
            print("in the log to compare. To force more output, raise the")
            print("`< 20` cap in the patch.")
        else:
            print()
            print("VERDICT: our mmap wrapper was NOT called.")
            print("Firefox is bypassing the `mmap` symbol entirely — most")
            print("likely it's linking against `mmap64@GLIBC_2.2.5` (the LFS")
            print("alias), which we don't export from libsmash. Same root")
            print("cause as the earlier fstat / fstat64 / statx versioning")
            print("issues.")
            print()
            print("FIX: add mmap64 + __mmap64 wrappers in linux_syscall_")
            print("wrappers.cpp + matching version-script entries.")
    finally:
        # 5. Always restore the original source so the tree stays clean.
        SOURCE.write_text(original)
        print(f"\n>>> restored {SOURCE.relative_to(REPO_ROOT)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
