#!/usr/bin/env python3
"""Refined probe: instrument every decision point in the mmap →
registerLinuxExternalRange path and tally calls per phase.

Background: firefox_mmap_probe.py confirmed our mmap wrapper IS called
~110 times during Firefox startup, but `committed` still stayed at 4.
The wrapper has two filters and a vm-null guard:

    if (ret == MAP_FAILED) return ret;
    bool anon = (flags & MAP_ANONYMOUS) != 0;
    bool writable = (prot & PROT_WRITE) != 0;
    if (!anon || !writable || len == 0) return ret;   // FILTER A
    auto* vm = smash::g_smash_vm_region;
    if (vm) registerLinuxExternalRange(vm, ret, len); // VM-NULL GUARD

…and inside registerLinuxExternalRange:

    if (!externalTrackingEnabledLinux()) return;      // ENV CHECK
    for (each page) trackExternalPage(p);             // SLOT EXHAUSTION

This script patches each of those points to bump a counter, runs Firefox
under it briefly, then dumps the counters so we can see which gate is
killing the registration:

    mmap_total                      — every mmap returning success
    mmap_after_filter               — passed (anon && write && len>0)
    mmap_vm_null                    — filter passed but vm wasn't ready yet
    mmap_registered                 — registerLinuxExternalRange entered
    register_env_disabled           — SMASH_TRACK_EXTERNAL was off
    register_pages_attempted        — trackExternalPage called
    register_pages_recorded         — trackExternalPage returned non-zero idx

If mmap_vm_null is dominant: smash's globals aren't set yet when
mozjemalloc allocates → init-order issue, fix by lazy-init or earlier
constructor priority.
If register_pages_attempted is huge but recorded is tiny: trackExternalPage
slot table is too small.
If register_env_disabled is non-zero: env var not propagating.

Usage:
    python3 bench/firefox_mmap_probe2.py /path/to/firefox-bin
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
WRAPPER = REPO_ROOT / "src" / "linux_syscall_wrappers.cpp"
BUILD = REPO_ROOT / "linux-build"
LIBSMASH = BUILD / "libsmash.so"


# Patch site 1: top of mmap wrapper body.
# Adds a 'mmap_total' counter and prints once near program-end via atexit.
PATCH1_FROM = """SMASH_VISIBLE void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    using fn_t = void*(*)(void*, size_t, int, int, int, off_t);
    SMASH_LAZY_RESOLVE(fn_t, mmap);
    if (!real_mmap) return MAP_FAILED;
    void* ret = real_mmap(addr, len, prot, flags, fd, offset);
    if (ret == MAP_FAILED) return ret;
    bool anon = (flags & MAP_ANONYMOUS) != 0;
    bool writable = (prot & PROT_WRITE) != 0;
    if (!anon || !writable || len == 0) return ret;
    auto* vm = smash::g_smash_vm_region;
    if (vm) registerLinuxExternalRange(vm, ret, len);
    return ret;
}"""

PATCH1_TO = """// PROBE2 counters
static std::atomic<unsigned long> _smash_p_mmap_total{0};
static std::atomic<unsigned long> _smash_p_mmap_after_filter{0};
static std::atomic<unsigned long> _smash_p_mmap_vm_null{0};
static std::atomic<unsigned long> _smash_p_mmap_registered{0};
static std::atomic<unsigned long> _smash_p_reg_env_disabled{0};
static std::atomic<unsigned long> _smash_p_reg_pages_attempted{0};
static std::atomic<unsigned long> _smash_p_reg_pages_recorded{0};
static void _smash_probe2_dump() {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "[smash probe2] pid=%d mmap_total=%lu after_filter=%lu vm_null=%lu "
        "registered=%lu reg_env_disabled=%lu reg_pages_attempted=%lu "
        "reg_pages_recorded=%lu\\n",
        (int)getpid(),
        _smash_p_mmap_total.load(), _smash_p_mmap_after_filter.load(),
        _smash_p_mmap_vm_null.load(), _smash_p_mmap_registered.load(),
        _smash_p_reg_env_disabled.load(), _smash_p_reg_pages_attempted.load(),
        _smash_p_reg_pages_recorded.load());
    if (n > 0) (void)!write(2, buf, (size_t)n);
}
__attribute__((constructor(70)))
static void _smash_probe2_register() { atexit(_smash_probe2_dump); }

SMASH_VISIBLE void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    using fn_t = void*(*)(void*, size_t, int, int, int, off_t);
    SMASH_LAZY_RESOLVE(fn_t, mmap);
    if (!real_mmap) return MAP_FAILED;
    void* ret = real_mmap(addr, len, prot, flags, fd, offset);
    if (ret == MAP_FAILED) return ret;
    _smash_p_mmap_total.fetch_add(1);
    bool anon = (flags & MAP_ANONYMOUS) != 0;
    bool writable = (prot & PROT_WRITE) != 0;
    if (!anon || !writable || len == 0) return ret;
    _smash_p_mmap_after_filter.fetch_add(1);
    auto* vm = smash::g_smash_vm_region;
    if (!vm) { _smash_p_mmap_vm_null.fetch_add(1); return ret; }
    _smash_p_mmap_registered.fetch_add(1);
    registerLinuxExternalRange(vm, ret, len);
    return ret;
}"""


# Patch site 2: registerLinuxExternalRange body
PATCH2_FROM = """inline void registerLinuxExternalRange(smash::VmRegion* vm, void* base, size_t len) {
    if (!vm || !base || !len) return;
    if (!externalTrackingEnabledLinux()) return;
    auto start = reinterpret_cast<uintptr_t>(base) & ~(uintptr_t{smash::kPageSize} - 1);
    auto end = (reinterpret_cast<uintptr_t>(base) + len + smash::kPageSize - 1)
               & ~(uintptr_t{smash::kPageSize} - 1);
    for (uintptr_t p = start; p < end; p += smash::kPageSize) {
        size_t idx = vm->trackExternalPage(p);
        if (idx == 0) continue;
        if (smash::g_smash_page_states_for_external)
            smash::g_smash_page_states_for_external->set(idx, smash::PageState::ACTIVE);
    }
}"""

PATCH2_TO = """inline void registerLinuxExternalRange(smash::VmRegion* vm, void* base, size_t len) {
    if (!vm || !base || !len) return;
    if (!externalTrackingEnabledLinux()) {
        _smash_p_reg_env_disabled.fetch_add(1);
        return;
    }
    auto start = reinterpret_cast<uintptr_t>(base) & ~(uintptr_t{smash::kPageSize} - 1);
    auto end = (reinterpret_cast<uintptr_t>(base) + len + smash::kPageSize - 1)
               & ~(uintptr_t{smash::kPageSize} - 1);
    for (uintptr_t p = start; p < end; p += smash::kPageSize) {
        _smash_p_reg_pages_attempted.fetch_add(1);
        size_t idx = vm->trackExternalPage(p);
        if (idx == 0) continue;
        _smash_p_reg_pages_recorded.fetch_add(1);
        if (smash::g_smash_page_states_for_external)
            smash::g_smash_page_states_for_external->set(idx, smash::PageState::ACTIVE);
    }
}"""


# We need extern decls for the counters in registerLinuxExternalRange
# (which is in the anonymous namespace earlier in the file). The cleanest
# way is to put them at file scope; the static atomics in PATCH1_TO are
# at the same translation unit so visible everywhere here.
# Actually they're declared static — can't be referenced from the
# anonymous namespace block? Let me check: registerLinuxExternalRange is
# inside `namespace { ... }` at file scope; static at TU-file-scope is
# also visible inside that namespace. Should work.


def patch() -> str:
    text = WRAPPER.read_text()
    if "_smash_p_mmap_total" in text:
        sys.exit("Source already patched — bail and clean up first.")
    if PATCH1_FROM not in text or PATCH2_FROM not in text:
        sys.exit("Could not find one of the patch sites — wrapper signature "
                 "may have changed. Inspect linux_syscall_wrappers.cpp.")
    new = text.replace(PATCH1_FROM, PATCH1_TO, 1)
    new = new.replace(PATCH2_FROM, PATCH2_TO, 1)
    WRAPPER.write_text(new)
    return text


def rebuild() -> None:
    if not BUILD.exists():
        sys.exit(f"{BUILD} missing — run cmake first.")
    print(">>> rebuilding libsmash with probe2 instrumentation…")
    subprocess.run(["make", "-j", str(os.cpu_count() or 4), "smash"],
                   cwd=BUILD, check=True)


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


def run_firefox(firefox: str, dwell: int) -> str:
    profile = tempfile.mkdtemp(prefix="smash-mmap-probe2-")
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(LIBSMASH)
    env["SMASH_BANNER"] = "1"
    env["SMASH_TRACK_EXTERNAL"] = "1"
    env["SMASH_LARGE_ONLY"] = "0"
    env["MOZ_DISABLE_CONTENT_SANDBOX"] = "1"
    cmd = [firefox, "--headless", "--no-remote", "--profile", profile,
           "https://example.com"]
    print(f">>> running firefox for {dwell}s…")
    proc = subprocess.Popen(
        cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        preexec_fn=os.setsid)
    time.sleep(dwell)
    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    try:
        out_b, _ = proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        out_b, _ = proc.communicate()
    return out_b.decode("utf-8", errors="replace")


def parse(log: str) -> None:
    print()
    print("=" * 78)
    print("PER-PROCESS COUNTERS (from atexit dumps)")
    print("=" * 78)
    lines = [l for l in log.splitlines() if "[smash probe2]" in l]
    if not lines:
        print("No probe2 lines emitted — atexit didn't run (process killed),")
        print("or the build didn't pick up the patch.")
        return
    print(f"{len(lines)} processes printed counters.")
    print()
    # totals
    keys = ["mmap_total", "after_filter", "vm_null", "registered",
            "reg_env_disabled", "reg_pages_attempted", "reg_pages_recorded"]
    totals = {k: 0 for k in keys}
    for line in lines:
        for k in keys:
            import re
            m = re.search(rf"\b{k}=(\d+)", line)
            if m:
                totals[k] += int(m.group(1))
    for line in lines[:8]:
        print(f"  {line}")
    if len(lines) > 8:
        print(f"  … ({len(lines) - 8} more)")
    print()
    print("AGGREGATE TOTALS:")
    for k in keys:
        print(f"  {k:<26} = {totals[k]}")

    print()
    print("=" * 78)
    print("DIAGNOSIS")
    print("=" * 78)
    if totals["mmap_total"] == 0:
        print("No mmaps captured at all — atexit didn't run before SIGTERM,")
        print("or wrapper isn't being called. Check probe1 first.")
        return
    after = totals["after_filter"]
    vmn = totals["vm_null"]
    reg = totals["registered"]
    if vmn > 0 and vmn >= reg:
        print(f"⚠️  {vmn}/{after} ({100*vmn/max(after,1):.0f}%) post-filter mmaps")
        print(f"    found g_smash_vm_region == nullptr. Smash's globals weren't")
        print(f"    set yet when these mmaps fired. Init-order bug.")
        print(f"    FIX: register early in mmap interposer regardless, OR")
        print(f"    raise smash's constructor priority so SmashHeap is created")
        print(f"    before mozjemalloc's first mmap.")
    elif totals["reg_env_disabled"] > 0:
        print(f"⚠️  registerLinuxExternalRange short-circuits because")
        print(f"    SMASH_TRACK_EXTERNAL is not '1' from the env-cache.")
        print(f"    Check that the env propagated into firefox-bin via /proc.")
    elif totals["reg_pages_attempted"] > 0:
        ratio = totals["reg_pages_recorded"] / totals["reg_pages_attempted"]
        print(f"registration: {totals['reg_pages_attempted']} attempts, "
              f"{totals['reg_pages_recorded']} recorded ({ratio:.1%}).")
        if ratio < 0.1:
            print(f"⚠️  trackExternalPage rejected most pages — slot table")
            print(f"    (kTrackMaxPages = 128 K) probably full from one big")
            print(f"    mmap. Bump kTrackMaxPages or use a more space-efficient")
            print(f"    tracking structure.")
        else:
            print(f"✅ Registration is working at the syscall layer.")
            print(f"    If `committed` still stays low, the issue is downstream:")
            print(f"    PageState transitions or compressor not seeing these.")
    else:
        print("Filter passed but registerLinuxExternalRange was never entered.")
        print("That should be impossible given the patch — re-examine the diff.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firefox", help="Path to firefox or firefox-bin")
    ap.add_argument("--dwell", type=int, default=20)
    args = ap.parse_args()

    if not Path(args.firefox).exists() or "/snap/" in os.path.realpath(args.firefox):
        print("firefox missing or snap; use a tarball.", file=sys.stderr)
        return 2

    original = patch()
    try:
        rebuild()
        kill_firefoxes()
        log = run_firefox(args.firefox, args.dwell)
        parse(log)
    finally:
        WRAPPER.write_text(original)
        print(f"\n>>> restored {WRAPPER.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
