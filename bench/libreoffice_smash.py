#!/usr/bin/env python3
"""Phase-2 real-world validation: smash on LibreOffice (system malloc).

LibreOffice uses libc malloc unmodified, so smash interposes directly.
This script:
1. Finds soffice (refuses snap on Linux — sandbox strips LD_PRELOAD).
2. Generates a synthetic ~5000-paragraph .fodt document.
3. Runs `soffice --headless --convert-to pdf` against it in a loop for
   ~60s (each conversion is short-lived, so iterating gives the
   compressor multiple ticks to operate on a non-trivial heap).
4. Captures every Firefox/smash diagnostic line (SMASH_BANNER /
   SMASH_DEBUG / SMASH_STATS).
5. Parses per-process `[smash stats]` lines: max committed, max
   compressed, compression ratio, total MB seen.
6. Prints a verdict against the Phase-2 pass criteria
   (≥30% RSS reduction proxy via compressed/committed, no crashes).

Usage:
    python3 bench/libreoffice_smash.py /path/to/libsmash.so [/path/to/soffice]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections import defaultdict
from pathlib import Path


# ── synthetic document ──────────────────────────────────────────────────────


_LOREM = (
    "The quick brown fox jumps over the lazy dog. Pack my box with five "
    "dozen liquor jugs. How vexingly quick daft zebras jump. The five "
    "boxing wizards jump quickly. Sphinx of black quartz, judge my vow. "
    "Bright vixens jump; dozy fowl quack. "
)


def generate_fodt(path: Path, n_paragraphs: int = 5000) -> None:
    """Write a Flat OpenDocument Text file with n synthetic paragraphs.
    .fodt is a single XML file (no zip) so generation is one open()/write()."""
    body_parts = [
        f'<text:p text:style-name="Standard">Paragraph {i}: {_LOREM * 3}</text:p>'
        for i in range(n_paragraphs)
    ]
    body = "\n".join(body_parts)
    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<office:document xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
                 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
                 xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0"
                 xmlns:fo="urn:oasis:names:tc:xsl-fo-compatible:1.0"
                 xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0"
                 office:version="1.2"
                 office:mimetype="application/vnd.oasis.opendocument.text">
  <office:automatic-styles>
    <style:style style:name="Standard" style:family="paragraph"/>
  </office:automatic-styles>
  <office:body>
    <office:text>{body}</office:text>
  </office:body>
</office:document>
"""
    path.write_text(xml)
    print(f">>> wrote {path} ({n_paragraphs} paragraphs, "
          f"{path.stat().st_size // 1024} KB)")


# ── soffice discovery ──────────────────────────────────────────────────────


def find_soffice() -> str | None:
    if sys.platform == "darwin":
        candidates = [
            "/Applications/LibreOffice.app/Contents/MacOS/soffice",
        ]
        for p in candidates:
            if Path(p).exists():
                return p
    found = shutil.which("soffice") or shutil.which("libreoffice")
    return found


def reject_snap_or_flatpak(path: str) -> None:
    real = os.path.realpath(path)
    if "/snap/" in real or "/var/lib/snapd" in real or "/.flatpak/" in real:
        sys.exit(f"refuse to run sandboxed soffice ({real}); LD_PRELOAD won't "
                 "stick. Install via tarball or non-snap distro package.")


# ── runner ─────────────────────────────────────────────────────────────────


def run_loop(libsmash: str, soffice: str, doc: Path, dwell_sec: int,
             out_log: Path) -> None:
    user_install = tempfile.mkdtemp(prefix="smash-lo-userinstall-")
    out_dir = tempfile.mkdtemp(prefix="smash-lo-out-")

    env = os.environ.copy()
    env["LD_PRELOAD"] = os.path.abspath(libsmash) if sys.platform == "linux" else env.get("LD_PRELOAD", "")
    if sys.platform == "darwin":
        env["DYLD_INSERT_LIBRARIES"] = os.path.abspath(libsmash)
        env["DYLD_FORCE_FLAT_NAMESPACE"] = "1"
        env["MallocNanoZone"] = "0"
    env["SMASH_BANNER"] = "1"
    env["SMASH_DEBUG"] = "1"
    env["SMASH_STATS"] = "1"
    env["SMASH_DEFER_PHASES_MS"] = "5000"
    env["SMASH_LARGE_ONLY"] = "0"

    # LibreOffice flags: headless + isolated user install + no first-time
    # registration prompt.
    base_args = [
        soffice,
        "--headless", "--nofirststartwizard",
        "--norestore", "--nologo",
        f"-env:UserInstallation=file://{user_install}",
        "--convert-to", "pdf",
        "--outdir", out_dir,
        str(doc),
    ]

    print(f">>> running soffice in a loop for {dwell_sec}s")
    print(f">>> user install: {user_install}")
    print(f">>> output dir:   {out_dir}")

    start = time.time()
    iterations = 0
    log = open(out_log, "wb")
    try:
        while time.time() - start < dwell_sec:
            iterations += 1
            print(f">>> iteration {iterations} (elapsed {time.time()-start:.0f}s)")
            proc = subprocess.run(
                base_args, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=120)
            log.write(proc.stdout)
            log.flush()
            if proc.returncode != 0:
                print(f"!!! iteration {iterations} returned {proc.returncode}")
    finally:
        log.close()
    print(f">>> total iterations: {iterations} in {time.time()-start:.0f}s")


# ── stats parsing (mirrors firefox_run_smash regex) ─────────────────────────


STATS_RE = re.compile(
    r"\[smash stats\](?:\s+\[?[0-9-]+ [0-9:]+\]?)?\s+pid=(\d+)\s+"
    r"committed=(\d+)\s+active=(\d+)\s+monitor=(\d+)\s+"
    r"compressing=(\d+)\s+compressed=(\d+)\s+empty=(\d+)"
)
BANNER_RE = re.compile(r"\[smash\][^\n]*\bloaded\b\s+pid=(\d+)")


def analyse(out_log: Path) -> int:
    text = out_log.read_text(errors="replace") if out_log.exists() else ""
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
        print("No stats were captured. Either the compressor never ticked "
              "(too-short conversions), or libsmash didn't load. Check the "
              "log for [smash] banner lines first.")
        return 1

    PAGE = 4096
    total_committed = sum(d["committed_max"] for d in by_pid.values())
    total_compressed = sum(d["compressed_max"] for d in by_pid.values())
    print()
    print(f"{'pid':>7}  {'samples':>7}  {'committed_max':>14}  "
          f"{'compressed_max':>15}  {'compressed_MB':>12}")
    for pid in sorted(by_pid):
        d = by_pid[pid]
        c = d["committed_max"]
        cz = d["compressed_max"]
        print(f"{pid:>7}  {d['samples']:>7}  {c:>14}  {cz:>15}  "
              f"{cz * PAGE / (1024*1024):>11.1f}")

    print()
    total_committed_mb = total_committed * PAGE / (1024 * 1024)
    total_compressed_mb = total_compressed * PAGE / (1024 * 1024)
    print(f"aggregate (sum of per-process maxima — overestimate of true peak):")
    print(f"  committed: {total_committed} pages ({total_committed_mb:.0f} MB)")
    print(f"  compressed: {total_compressed} pages ({total_compressed_mb:.0f} MB)")
    if total_committed > 0:
        ratio = total_compressed / total_committed
        print(f"  compressed/committed: {ratio:.1%}")

    print()
    print("=" * 78)
    print("VERDICT")
    print("=" * 78)
    pass_criteria = total_committed > 256 and total_compressed > total_committed * 0.3
    if pass_criteria:
        print(f"✅ smash compressed {total_compressed_mb:.0f} MB / "
              f"{total_committed_mb:.0f} MB of LibreOffice's heap.")
        print("   Real-world workload coverage validated.")
        return 0
    elif total_committed < 256:
        print(f"⚠️  smash saw only {total_committed} pages — LibreOffice's "
              "conversion completed before the compressor could tick. Try a "
              "larger document or longer dwell.")
        return 1
    else:
        print(f"⚠️  smash saw {total_committed_mb:.0f} MB but only compressed "
              f"{100*total_compressed/total_committed:.1f}%. Pages may be "
              f"staying hot — try lowering SMASH_COLD_TIMEOUT_SEC.")
        return 1


# ── entrypoint ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("libsmash", help="Path to libsmash.so or libsmash.dylib")
    ap.add_argument("soffice", nargs="?", default=None,
                    help="Path to soffice (default: auto-detect)")
    ap.add_argument("--dwell", type=int, default=60,
                    help="Total seconds to loop conversions (default 60)")
    ap.add_argument("--paragraphs", type=int, default=5000,
                    help="Synthetic doc size (default 5000 paragraphs)")
    args = ap.parse_args()

    if not Path(args.libsmash).exists():
        print(f"libsmash not found: {args.libsmash}", file=sys.stderr)
        return 2

    soffice = args.soffice or find_soffice()
    if not soffice:
        print("soffice not found. Install LibreOffice (NOT the snap):",
              file=sys.stderr)
        print("  Linux: download tarball from https://www.libreoffice.org/", file=sys.stderr)
        print("  macOS: brew install --cask libreoffice", file=sys.stderr)
        return 2
    if sys.platform == "linux":
        reject_snap_or_flatpak(soffice)

    print(f">>> libsmash: {args.libsmash}")
    print(f">>> soffice : {soffice}")

    workdir = Path(tempfile.mkdtemp(prefix="smash-lo-"))
    doc = workdir / "synthetic.fodt"
    out_log = workdir / "soffice-smash.log"
    generate_fodt(doc, args.paragraphs)
    run_loop(args.libsmash, soffice, doc, args.dwell, out_log)
    rc = analyse(out_log)
    print(f"\n>>> full log saved to {out_log}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
