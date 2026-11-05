#!/usr/bin/env python3
"""Per-pid heap-footprint sampling, portable across macOS and Linux.

The paper's "heap footprint" metric is anonymous, non-reclaimable resident
memory.  The C++ monitors (e.g. ``getRSSBytesForPid`` in
``bench/bench_coldhotmix.cpp``) already implement this correctly:

  * macOS  -> ``proc_pid_rusage(...).ri_phys_footprint``  (excludes clean,
             file-backed pages; matches what the C++ ``ri_phys_footprint``
             path reports)
  * Linux  -> ``/proc/<pid>/status`` ``VmRSS``

This module gives the Python samplers the SAME behavior so their numbers are
consistent with the C++ monitors.  ``ps -o rss=`` is deliberately NOT used on
macOS: it returns ``resident_size`` (clean file-backed pages included), which
is inconsistent with the ``phys_footprint`` the rest of the harness reports.

``METRIC_SOURCE`` names the metric actually being sampled so results files can
be self-describing (macOS ``phys_footprint`` vs Linux ``proc_vmrss``).

Dependency-free: standard library only (``ctypes`` on macOS, ``/proc`` on
Linux, ``ps`` for the cross-platform descendant walk on macOS).
"""
import os
import platform
import subprocess

_IS_DARWIN = platform.system() == "Darwin"

# What metric pid_footprint_kb() actually returns on this host.  Downstream
# code stamps this into results (JSON field / CSV header) so a reader can tell
# macOS(phys_footprint) data apart from Linux(VmRSS) data at a glance.
if _IS_DARWIN:
    METRIC_SOURCE = "phys_footprint"
else:
    METRIC_SOURCE = "proc_vmrss"


# ── macOS: proc_pid_rusage -> ri_phys_footprint via ctypes ───────────────────

if _IS_DARWIN:
    import ctypes

    class RUsageInfoV0(ctypes.Structure):
        # rusage_info_v0 layout (sys/resource.h).  ri_phys_footprint is
        # present in v0 already, so we only need the smallest flavor.
        _fields_ = [
            ("ri_uuid", ctypes.c_uint8 * 16),
            ("ri_user_time", ctypes.c_uint64),
            ("ri_system_time", ctypes.c_uint64),
            ("ri_pkg_idle_wkups", ctypes.c_uint64),
            ("ri_interrupt_wkups", ctypes.c_uint64),
            ("ri_pageins", ctypes.c_uint64),
            ("ri_wired_size", ctypes.c_uint64),
            ("ri_resident_size", ctypes.c_uint64),
            ("ri_phys_footprint", ctypes.c_uint64),
            ("ri_proc_start_abstime", ctypes.c_uint64),
            ("ri_proc_exit_abstime", ctypes.c_uint64),
        ]

    _RUSAGE_INFO_V0 = 0

    try:
        _libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
        _libproc.proc_pid_rusage.argtypes = [
            ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        _libproc.proc_pid_rusage.restype = ctypes.c_int
    except OSError:
        _libproc = None

    def _footprint_kb_darwin(pid):
        if _libproc is None:
            return 0
        ri = RUsageInfoV0()
        rc = _libproc.proc_pid_rusage(
            int(pid), _RUSAGE_INFO_V0, ctypes.byref(ri))
        if rc == 0:
            return int(ri.ri_phys_footprint) // 1024
        return 0


def _footprint_kb_linux(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (OSError, ValueError):
        pass
    return 0


def pid_footprint_kb(pid):
    """Heap footprint of a single pid in kilobytes, or 0 if unavailable.

    macOS: ``ri_phys_footprint`` (bytes -> KB).  Linux: ``/proc`` ``VmRSS``
    (KB), preserving the existing Linux semantics exactly.
    """
    if _IS_DARWIN:
        return _footprint_kb_darwin(pid)
    return _footprint_kb_linux(pid)


# ── Descendant process discovery (both OSes) ─────────────────────────────────

def _descendants_linux(root_pid):
    children = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        try:
            with open(f"/proc/{pid}/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            ppid = int(fields[1])
            children.setdefault(ppid, []).append(pid)
        except (OSError, IndexError, ValueError):
            continue
    return _walk_tree(root_pid, children)


def _descendants_darwin(root_pid):
    children = {}
    try:
        out = subprocess.check_output(
            ["ps", "-axo", "pid=,ppid="], text=True)
    except (OSError, subprocess.CalledProcessError):
        return [root_pid]
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        try:
            pid, ppid = int(parts[0]), int(parts[1])
        except ValueError:
            continue
        children.setdefault(ppid, []).append(pid)
    return _walk_tree(root_pid, children)


def _walk_tree(root_pid, children):
    pids = {root_pid}
    queue = [root_pid]
    while queue:
        p = queue.pop()
        for c in children.get(p, ()):
            if c not in pids:
                pids.add(c)
                queue.append(c)
    return list(pids)


def descendant_pids(root_pid):
    """Return root_pid plus every pid currently in its descendant tree.

    Linux walks ``/proc/*/stat``; macOS builds the tree from
    ``ps -axo pid=,ppid=``.  Same return shape on both.
    """
    if _IS_DARWIN:
        return _descendants_darwin(root_pid)
    return _descendants_linux(root_pid)


if __name__ == "__main__":
    _self = os.getpid()
    print(f"METRIC_SOURCE={METRIC_SOURCE}")
    print(f"pid_footprint_kb(self)={pid_footprint_kb(_self)} KB")
    print(f"descendant_pids(self)={descendant_pids(_self)}")
