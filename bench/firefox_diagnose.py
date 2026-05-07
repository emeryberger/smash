#!/usr/bin/env python3
"""Diagnose why libsmash isn't reaching Firefox content processes.

Usage:
    python3 firefox_diagnose.py /path/to/libsmash.so [/path/to/firefox]

What it does:
1. Inspects the system: distro, kernel, glibc, snap/flatpak, AppArmor.
2. Inspects the Firefox binary on PATH (or the one you pass): shebang,
   setuid/setgid bits, file caps, package origin.
3. Inspects libsmash.so itself.
4. Launches Firefox under LD_PRELOAD=libsmash.so with SMASH_BANNER=1
   and SMASH_STATS=1, lets it run for 30s, then walks the entire
   process tree it spawned and reports per-PID:
       exe path
       LD_PRELOAD env (preserved or stripped)
       AT_SECURE auxv flag (1 means glibc loader scrubs LD_PRELOAD)
       libsmash.so present in /proc/PID/maps (yes = it loaded; no = it didn't)
5. Prints a SUMMARY of where libsmash made it and where it didn't.

Run as the same user that owns the Firefox processes — we read
/proc/PID/{environ,maps,auxv} which require uid match or root.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path


# ── helpers ──────────────────────────────────────────────────────────────────


def sh(cmd: str, *, check: bool = False) -> str:
    """Run a shell command, return stdout (decoded). On failure return ''."""
    try:
        out = subprocess.run(
            cmd, shell=True, capture_output=True, text=True,
            check=check, timeout=20)
        return (out.stdout or "") + (("\n[stderr] " + out.stderr) if out.stderr.strip() else "")
    except Exception as exc:
        return f"<error: {exc}>"


def banner(s: str) -> None:
    print()
    print("=" * 78)
    print(s)
    print("=" * 78)


def short(s: str, n: int = 240) -> str:
    s = (s or "").rstrip()
    return s if len(s) <= n else s[: n - 3] + "..."


# ── /proc/PID readers ───────────────────────────────────────────────────────


def proc_read_text(pid: int, name: str) -> str:
    try:
        return Path(f"/proc/{pid}/{name}").read_text(errors="replace")
    except OSError as exc:
        return f"<unreadable: {exc}>"


def proc_read_bytes(pid: int, name: str) -> bytes:
    try:
        return Path(f"/proc/{pid}/{name}").read_bytes()
    except OSError:
        return b""


def proc_environ(pid: int) -> dict[str, str]:
    raw = proc_read_bytes(pid, "environ")
    if not raw:
        return {}
    out: dict[str, str] = {}
    for entry in raw.split(b"\0"):
        if not entry or b"=" not in entry:
            continue
        k, _, v = entry.decode("utf-8", "replace").partition("=")
        out[k] = v
    return out


def proc_at_secure(pid: int) -> int | None:
    """Read AT_SECURE from /proc/PID/auxv. Returns 0/1 or None."""
    AT_SECURE = 23
    raw = proc_read_bytes(pid, "auxv")
    if not raw:
        return None
    # 64-bit: 8-byte key + 8-byte value, native endian.
    fmt = "@QQ"
    sz = struct.calcsize(fmt)
    for i in range(0, len(raw) - sz + 1, sz):
        key, val = struct.unpack_from(fmt, raw, i)
        if key == AT_SECURE:
            return int(val)
        if key == 0:  # AT_NULL — terminator
            break
    return None


def proc_has_libsmash(pid: int, libsmash_basename: str) -> bool:
    maps = proc_read_text(pid, "maps")
    return libsmash_basename in maps


def proc_exe(pid: int) -> str:
    try:
        return os.readlink(f"/proc/{pid}/exe")
    except OSError as exc:
        return f"<unreadable: {exc}>"


def proc_cmdline(pid: int) -> str:
    raw = proc_read_bytes(pid, "cmdline")
    return raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()


# ── subtree enumeration ─────────────────────────────────────────────────────


def descendants(root: int) -> list[int]:
    """All descendants of root_pid via /proc/*/status PPid."""
    parents: dict[int, int] = {}
    for p in Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        pid = int(p.name)
        try:
            for line in (p / "status").read_text().splitlines():
                if line.startswith("PPid:"):
                    parents[pid] = int(line.split()[1])
                    break
        except OSError:
            continue
    out: list[int] = []
    queue = [root]
    while queue:
        cur = queue.pop()
        for pid, ppid in parents.items():
            if ppid == cur and pid not in out:
                out.append(pid)
                queue.append(pid)
    return sorted(out)


def all_firefox_pids() -> list[int]:
    """Every process system-wide whose comm or cmdline contains 'firefox'.
    Catches sandbox-escapees that fall out of our spawn subtree."""
    out: list[int] = []
    for p in Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        pid = int(p.name)
        try:
            comm = (p / "comm").read_text(errors="replace").strip().lower()
            cmdline = (p / "cmdline").read_bytes().replace(b"\0", b" ").decode(
                "utf-8", "replace").lower()
            if ("firefox" in comm) or ("firefox" in cmdline) or ("mozilla" in cmdline):
                out.append(pid)
        except OSError:
            continue
    return sorted(out)


# ── sections ────────────────────────────────────────────────────────────────


def section_system() -> None:
    banner("system")
    print("uname:", short(sh("uname -a")))
    print("/etc/os-release:")
    print(short(sh("cat /etc/os-release"), 600))
    print("glibc:", short(sh("ldd --version | head -1")))
    print("kernel.yama.ptrace_scope:",
          short(sh("cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null")))
    print("kernel.unprivileged_userns_clone:",
          short(sh("cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null")))
    print("aa-status (apparmor) profile count:",
          short(sh("aa-status 2>/dev/null | head -3")))


def section_firefox_binary(firefox: str) -> bool:
    """Return True if firefox is sandboxed (snap/flatpak) — caller should
    skip the launch+walk step since it can't possibly work."""
    banner(f"firefox binary: {firefox}")
    print("type:", short(sh(f"file {shlex.quote(firefox)}")))
    print("size+mode:", short(sh(f"stat -c '%A %s %n' {shlex.quote(firefox)}")))
    # Shebang?
    try:
        with open(firefox, "rb") as f:
            head = f.read(200)
        if head.startswith(b"#!"):
            line = head.split(b"\n", 1)[0].decode("utf-8", "replace")
            print(f"SHEBANG: {line}")
            print("script body (first 30 lines):")
            print(short(sh(f"head -30 {shlex.quote(firefox)}"), 2000))
    except OSError:
        pass
    # Caps and setuid status
    print("getcap:", short(sh(f"getcap {shlex.quote(firefox)} 2>/dev/null")) or "(none)")
    print("dpkg origin:", short(sh(f"dpkg -S {shlex.quote(firefox)} 2>/dev/null")))
    print("snap firefox:", short(sh("snap list firefox 2>/dev/null")) or "(not installed via snap)")
    print("flatpak firefox:",
          short(sh("flatpak list | grep -i firefox 2>/dev/null")) or "(not installed via flatpak)")
    is_snap = ("/snap/" in firefox or "/var/lib/snapd" in firefox or
               sh(f"snap list firefox 2>/dev/null | grep -q firefox && echo y").strip() == "y")
    if is_snap:
        # Pick the right Mozilla download URL for this arch.
        arch = os.uname().machine
        if arch in ("x86_64", "amd64"):
            mozilla_os = "linux64"
        elif arch in ("aarch64", "arm64"):
            mozilla_os = "linux-aarch64"
        else:
            mozilla_os = f"linux-{arch}  # may not exist; check archive.mozilla.org"
        print()
        print("!!! firefox is from snap — snap confinement strips LD_PRELOAD")
        print("!!! at the sandbox boundary. The wrapper /usr/bin/firefox loads")
        print("!!! libsmash, then exec's into the snap confined namespace where")
        print("!!! the env is scrubbed. There is no fix for this short of using")
        print("!!! a non-snap Firefox build.")
        print("!!!")
        print(f"!!! Option A — Mozilla tarball ({arch}):")
        print(f"!!!   wget -O firefox.tar.xz \\")
        print(f"!!!     'https://download.mozilla.org/?product=firefox-latest-ssl&os={mozilla_os}&lang=en-US'")
        print(f"!!!   tar -xf firefox.tar.xz")
        print(f"!!!   ./firefox/firefox --version    # confirm arch")
        print("!!!")
        print("!!! Option B — non-snap distro package (ARM64 Ubuntu ships .deb, not snap):")
        print("!!!   sudo apt install firefox-esr && firefox-esr --version")
        print("!!!")
        print("!!! Then re-run this script:")
        print("!!!   python3 firefox_diagnose.py /path/to/libsmash.so /path/to/firefox")
        return True
    return False


def section_libsmash(libsmash: str) -> None:
    banner(f"libsmash: {libsmash}")
    p = Path(libsmash)
    if not p.exists():
        print("ERROR: libsmash does not exist at this path")
        return
    print("size:", p.stat().st_size, "bytes")
    h = hashlib.sha256(p.read_bytes()).hexdigest()
    print("sha256:", h)
    print("file type:", short(sh(f"file {shlex.quote(libsmash)}")))
    syms = sh(f"nm -D --defined-only {shlex.quote(libsmash)} 2>/dev/null | "
              f"grep -E ' T (malloc|fstat|read|recv|getdents64|statx)$' | head -20")
    print("key exports:")
    print(short(syms or "(nm not available)", 1200))


# ── main launch + walk ──────────────────────────────────────────────────────


def launch_and_walk(libsmash: str, firefox: str, dwell_sec: int) -> None:
    libsmash_abs = os.path.abspath(libsmash)
    libsmash_base = os.path.basename(libsmash_abs)

    env = os.environ.copy()
    env["LD_PRELOAD"] = libsmash_abs
    env["SMASH_BANNER"] = "1"
    env["SMASH_STATS"] = "1"
    env["SMASH_DEFER_PHASES_MS"] = "30000"
    env["SMASH_TRACK_EXTERNAL"] = "1"
    env["SMASH_LARGE_ONLY"] = "0"

    banner(f"launching firefox under smash for {dwell_sec}s")
    print("env added: LD_PRELOAD, SMASH_{BANNER,STATS,DEFER_PHASES_MS,TRACK_EXTERNAL,LARGE_ONLY}")
    # New session so we can kill the whole subtree cleanly.
    proc = subprocess.Popen(
        [firefox, "--headless", "--no-remote", "https://example.com"],
        env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        preexec_fn=os.setsid)
    print(f"launched: pid={proc.pid}")

    # Let Firefox spin up content/GPU/networking children.
    print(f"sleeping {dwell_sec}s …")
    time.sleep(dwell_sec)

    # Snapshot the tree before we kill anything. Also include any
    # firefox-named process system-wide — sandbox-escapees (snap, flatpak)
    # fall out of our subtree because they're in a different cgroup.
    subtree = [proc.pid] + descendants(proc.pid)
    all_ff = all_firefox_pids()
    extra = [p for p in all_ff if p not in subtree]
    pids = sorted(set(subtree + all_ff))
    banner(f"process scan: {len(subtree)} in subtree under {proc.pid}; "
           f"{len(extra)} additional firefox-named processes elsewhere")
    if extra:
        print(f"!!! {len(extra)} firefox process(es) escaped our subtree —")
        print("!!! likely snap/flatpak/sandbox confinement. They will not "
              "have inherited LD_PRELOAD.")
        for p in extra[:5]:
            print(f"    pid={p} exe={proc_exe(p)} cmd={short(proc_cmdline(p), 100)}")
    rows: list[tuple[int, str, str, str, str]] = []
    for pid in pids:
        try:
            exe = proc_exe(pid)
            cmd = proc_cmdline(pid)
            env_p = proc_environ(pid)
            ld = env_p.get("LD_PRELOAD", "<missing>")
            at_sec = proc_at_secure(pid)
            has = proc_has_libsmash(pid, libsmash_base)
            ld_short = "libsmash✓" if libsmash_base in ld else (
                "<missing>" if ld == "<missing>" else f"other={ld[:30]}")
            print(f"\npid={pid} exe={exe}")
            print(f"  cmd: {short(cmd, 200)}")
            print(f"  LD_PRELOAD env: {ld_short}")
            print(f"  AT_SECURE: {at_sec}  (1 = loader scrubs LD_PRELOAD)")
            print(f"  libsmash mapped: {'YES' if has else 'NO'}")
            rows.append((pid, os.path.basename(exe), ld_short,
                         "Y" if has else "N",
                         "1" if at_sec == 1 else "0" if at_sec == 0 else "?"))
        except Exception as exc:
            print(f"pid={pid} <error: {exc}>")

    # Kill the subtree.
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=10)
    except Exception:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass

    banner("SUMMARY")
    loaded = sum(1 for r in rows if r[3] == "Y")
    print(f"libsmash mapped in {loaded} / {len(rows)} processes")
    print()
    print(f"{'pid':>7}  {'exe':<32}  {'LD_PRELOAD':<22}  {'mapped':<6}  AT_SECURE")
    for pid, exe, ld, mapped, at_sec in rows:
        print(f"{pid:>7}  {exe:<32}  {ld:<22}  {mapped:<6}  {at_sec}")

    if loaded == 0:
        print("\nINTERPRETATION: libsmash never loaded into any Firefox process.")
        print("Common causes:")
        print("  - snap/flatpak firefox (sandbox strips LD_PRELOAD)")
        print("  - The launcher is a wrapper script that unsets LD_PRELOAD")
        print("  - Firefox binary has setuid/setgid/setcap → AT_SECURE=1")
    elif loaded < len(rows):
        unloaded = [r for r in rows if r[3] == "N"]
        print(f"\nINTERPRETATION: libsmash loaded into the wrapper but not into "
              f"{len(unloaded)} child(ren). The first child without libsmash is "
              f"where the env was stripped — inspect its parent's exec sequence.")
        print("First unloaded child:")
        for pid, exe, ld, mapped, at_sec in rows:
            if mapped == "N":
                print(f"  pid={pid} exe={exe}  AT_SECURE={at_sec}  LD_PRELOAD={ld}")
                break
    else:
        print("\nINTERPRETATION: libsmash loaded everywhere. The issue (if any) "
              "is elsewhere — likely Firefox bypasses our malloc via mozjemalloc "
              "internally. Check `[smash stats]` lines for non-trivial committed=N.")


# ── entrypoint ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("libsmash", help="Path to libsmash.so")
    ap.add_argument("firefox", nargs="?", default=None,
                    help="Path to firefox (default: $(which firefox))")
    ap.add_argument("--dwell", type=int, default=30,
                    help="Seconds to let Firefox run before snapshot (default 30)")
    args = ap.parse_args()

    if sys.platform != "linux":
        print("This script is Linux-only (uses /proc).", file=sys.stderr)
        return 2

    libsmash = os.path.abspath(args.libsmash)
    if not os.path.exists(libsmash):
        print(f"libsmash not found: {libsmash}", file=sys.stderr)
        return 2

    firefox = args.firefox or shutil.which("firefox")
    if not firefox:
        print("firefox not found on PATH; pass it as the second arg",
              file=sys.stderr)
        return 2
    firefox = os.path.abspath(firefox) if os.path.exists(firefox) else firefox

    section_system()
    sandboxed = section_firefox_binary(firefox)
    section_libsmash(libsmash)
    if sandboxed:
        banner("SUMMARY")
        print("Firefox is in a confined sandbox (snap/flatpak). LD_PRELOAD")
        print("cannot reach the actual Firefox processes through the sandbox")
        print("boundary. Install a non-confined Firefox (Mozilla tarball or")
        print("a non-snap distro package) and retry.")
        return 0
    launch_and_walk(libsmash, firefox, args.dwell)
    return 0


if __name__ == "__main__":
    sys.exit(main())
