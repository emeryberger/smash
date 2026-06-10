#!/usr/bin/env python3
"""install_bench_deps.py — install everything the smash HEAVY benchmark suite
needs (Redis / memcached / RocksDB external services + the allocator comparison:
mimalloc / jemalloc / tcmalloc / hoard).

Target host: Amazon Linux 2023 (dnf). Run as root:
    sudo python3 tools/install_bench_deps.py

Idempotent: re-running skips anything already present. Verbose: prints a
capability summary at the end so you know exactly what will and won't run.

NOTE on the bench tiers (see CLAUDE.md):
  - CI / regression benches (bench_rss, bench_sqlite) need NOTHING here — they
    build from FetchContent and run in-process. This script is only for the
    PAPER heavy benches gated behind -DSMASH_BUILD_BENCH_DEPS=ON and
    -DSMASH_BUILD_BENCH_ALLOCATORS=ON.
  - The tcmalloc allocator-compare path needs Bazel AND glibc >= 2.35. AL2023
    ships glibc 2.34, so tcmalloc-via-bazel is expected to be unavailable here
    even after this script; the script installs bazelisk anyway and reports the
    glibc gate so the limitation is explicit, not a silent skip.

This is a port of the original install_bench_deps.sh — shell glob/`test`
quoting made the capability checks too brittle (e.g. `ls a* b*` fails when only
one pattern matches), so the logic now lives in Python where glob matching is
unambiguous.
"""

import glob as globmod
import os
import pwd
import shutil
import subprocess
import sys
import tempfile

# ── pretty output ────────────────────────────────────────────────────────────
_C = sys.stderr.isatty() or sys.stdout.isatty()


def _c(code, s):
    return f"\033[{code}m{s}\033[0m" if _C else s


def log(msg):
    print("\n" + _c("1;36", f"==> {msg}"))


def ok(msg):
    print("  " + _c("1;32", "[ok]") + f"   {msg}")


def skip(msg):
    print("  " + _c("1;33", "[skip]") + f" {msg}")


def warn(msg):
    print("  " + _c("1;31", "[warn]") + f" {msg}")


# ── helpers ──────────────────────────────────────────────────────────────────
def have(cmd):
    """True if `cmd` is on PATH."""
    return shutil.which(cmd) is not None


def glob_any(*patterns):
    """True if ANY shell glob pattern matches at least one path."""
    return any(globmod.glob(p) for p in patterns)


def run(cmd, *, check=False, quiet=False, as_user=None):
    """Run a command. `cmd` is a list. Returns the CompletedProcess.

    quiet=True suppresses stdout+stderr. as_user wraps in `sudo -u <user>`.
    Never raises unless check=True.
    """
    if as_user:
        cmd = ["sudo", "-u", as_user] + cmd
    kw = {}
    if quiet:
        kw["stdout"] = subprocess.DEVNULL
        kw["stderr"] = subprocess.DEVNULL
    return subprocess.run(cmd, check=check, **kw)


def dnf_install(pkgs, *, quiet=False):
    """`dnf -y install ...`. Returns True on exit 0, False otherwise."""
    return run(["dnf", "-y", "install", *pkgs], quiet=quiet).returncode == 0


def glibc_version():
    """Return glibc version as a float (e.g. 2.34), or None."""
    try:
        out = subprocess.run(
            ["ldd", "--version"], capture_output=True, text=True
        ).stdout
        import re

        m = re.search(r"(\d+\.\d+)", out)
        return float(m.group(1)) if m else None
    except Exception:
        return None


def main():
    # Resolve the non-root user who invoked sudo, for the per-user installs
    # (bazelisk in ~/.local) that should NOT be root-owned.
    real_user = os.environ.get("SUDO_USER", "root")
    try:
        real_home = pwd.getpwnam(real_user).pw_dir
    except KeyError:
        real_home = os.path.expanduser("~")
    print(f"==> invoking user: {real_user} (home: {real_home})")

    # ── 1. Toolchain + build prerequisites ───────────────────────────────────
    log("Base toolchain (gcc/g++, cmake, ninja, git, python, autotools)")
    if not dnf_install([
        "gcc", "gcc-c++", "make", "cmake", "ninja-build", "git",
        "python3", "python3-pip", "python3-devel",
        "autoconf", "automake", "libtool", "pkgconf-pkg-config",
        "zlib-devel", "bzip2-devel", "lz4-devel", "libzstd-devel", "snappy-devel",
        "openssl-devel", "libevent-devel",
    ]):
        warn("some base packages failed — check dnf output above")

    # ── 2. External services: Redis + memcached ──────────────────────────────
    # Both are in the AL2023 repos. memcached needs libevent (installed above).
    log("Redis + memcached (external-service benches)")
    if not dnf_install(["redis6", "memcached"], quiet=True) \
            and not dnf_install(["redis", "memcached"]):
        warn("redis/memcached install failed")
    # AL2023 ships redis as 'redis6'; symlink to redis-server if bench expects it.
    redis6 = shutil.which("redis6-server")
    if redis6 and not have("redis-server"):
        try:
            os.symlink(redis6, "/usr/local/bin/redis-server")
            ok("symlinked redis6-server -> redis-server")
        except OSError as e:
            warn(f"could not symlink redis6-server: {e}")

    # ── 3. RocksDB ────────────────────────────────────────────────────────────
    log("RocksDB dev libraries")
    # AL2023 has no standalone rocksdb-devel package (only the MariaDB storage-
    # engine plugin), so this dnf install is EXPECTED to no-op there. That's
    # fine: the smash bench's CMake ExternalProject path
    # (-DSMASH_BUILD_BENCH_DEPS=ON) builds RocksDB from source.
    if dnf_install(["rocksdb", "rocksdb-devel"], quiet=True):
        ok("installed rocksdb + rocksdb-devel from repos")
    else:
        skip("no rocksdb-devel in repos (normal on AL2023) — CMake "
             "ExternalProject will build it (-DSMASH_BUILD_BENCH_DEPS=ON)")

    # ── 4. Allocators: jemalloc, tcmalloc(gperftools), mimalloc, hoard ────────
    log("jemalloc + gperftools(tcmalloc) + mimalloc dev libraries")
    if not dnf_install(
        ["jemalloc", "jemalloc-devel", "gperftools", "gperftools-devel"],
        quiet=True,
    ):
        warn("jemalloc/gperftools not all available in repos")

    # mimalloc: not always packaged on AL2023 — build from source into /usr/local.
    mimalloc_globs = [
        "/usr/local/lib64/libmimalloc.so*",
        "/usr/local/lib/libmimalloc.so*",
        "/usr/lib64/libmimalloc.so*",
    ]
    if glob_any(*mimalloc_globs):
        skip("mimalloc already present")
    else:
        log("Building mimalloc from source (v2.1.7)")
        with tempfile.TemporaryDirectory() as tmpd:
            src = os.path.join(tmpd, "mimalloc")
            cloned = run([
                "git", "clone", "--depth", "1", "--branch", "v2.1.7",
                "https://github.com/microsoft/mimalloc", src,
            ], quiet=True).returncode == 0
            if not cloned:
                warn("mimalloc clone failed — skip")
            else:
                out = os.path.join(src, "out")
                run(["cmake", "-S", src, "-B", out,
                     "-DCMAKE_BUILD_TYPE=Release", "-DMI_BUILD_TESTS=OFF"],
                    quiet=True)
                built = run(["cmake", "--build", out, "-j", str(os.cpu_count())],
                            quiet=True).returncode == 0
                if built and run(["cmake", "--install", out],
                                 quiet=True).returncode == 0:
                    run(["ldconfig"], quiet=True)
                    ok("installed mimalloc to /usr/local")
                else:
                    warn("mimalloc build/install failed — skip")

    # Hoard: source build; the smash CMake finds it via find_library, so install
    # the .so into a standard prefix. Built as the invoking user, root-installed.
    if os.path.isfile("/usr/local/lib/libhoard.so"):
        skip("hoard already present")
    else:
        log("Building Hoard from source")
        with tempfile.TemporaryDirectory() as tmpd:
            os.chmod(tmpd, 0o777)  # invoking user clones/builds here
            src = os.path.join(tmpd, "Hoard")
            cloned = run([
                "git", "clone", "--depth", "1", "--recursive",
                "https://github.com/emeryberger/Hoard", src,
            ], quiet=True, as_user=real_user).returncode == 0
            if not cloned:
                warn("hoard clone failed — skip")
            else:
                built = run(
                    ["bash", "-c",
                     f"cd '{src}/src' && make -j{os.cpu_count()}"],
                    quiet=True, as_user=real_user,
                ).returncode == 0
                if not built:
                    warn("hoard build failed — skip "
                         "(non-fatal; allocator-compare just omits hoard)")
                else:
                    found = globmod.glob(os.path.join(src, "**", "libhoard.so"),
                                         recursive=True)
                    if found:
                        shutil.copy(found[0], "/usr/local/lib/libhoard.so")
                        os.chmod("/usr/local/lib/libhoard.so", 0o755)
                        run(["ldconfig"], quiet=True)
                        ok("installed libhoard.so")
                    else:
                        warn("hoard built but libhoard.so not found")

    # ── 5. Bazelisk (for the tcmalloc-preload allocator-compare target) ───────
    # Installed per-user into ~/.local/bin (Bazel dislikes running as root).
    log("bazelisk (tcmalloc allocator-compare build driver)")
    has_bazel = run(
        ["bash", "-c", "command -v bazelisk || command -v bazel"],
        quiet=True, as_user=real_user,
    ).returncode == 0
    if has_bazel:
        skip(f"bazel/bazelisk already on {real_user}'s PATH")
    else:
        bindir = os.path.join(real_home, ".local", "bin")
        run(["mkdir", "-p", bindir], quiet=True, as_user=real_user)
        bzl = os.path.join(bindir, "bazelisk")
        url = ("https://github.com/bazelbuild/bazelisk/releases/latest/"
               "download/bazelisk-linux-amd64")
        if run(["curl", "-fsSL", url, "-o", bzl], quiet=True).returncode == 0:
            os.chmod(bzl, 0o755)
            shutil.chown(bzl, user=real_user)
            link = os.path.join(bindir, "bazel")
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(bzl, link)
            os.chown(link, pwd.getpwnam(real_user).pw_uid, -1,
                     follow_symlinks=False)
            ok(f"installed bazelisk to {bindir} (ensure it's on PATH)")
        else:
            warn("bazelisk download failed — tcmalloc-via-bazel target "
                 "will be skipped by CMake")

    # ── 6. Capability summary ─────────────────────────────────────────────────
    log("Capability summary (what the heavy suite can run after this)")

    def check(label, predicate):
        (ok if predicate else lambda m: warn(f"{m} — NOT available"))(label)

    check("redis-server", have("redis-server") or have("redis6-server"))
    check("memcached", have("memcached"))
    check("rocksdb headers", os.path.isfile("/usr/include/rocksdb/db.h"))
    check("jemalloc", glob_any("/usr/lib64/libjemalloc.so*"))
    check("tcmalloc (gperftools)", glob_any("/usr/lib64/libtcmalloc*.so*"))
    check("mimalloc", glob_any(*mimalloc_globs))
    check("hoard", os.path.isfile("/usr/local/lib/libhoard.so"))
    check("bazelisk (per-user)", run(
        ["bash", "-lc", "command -v bazelisk || command -v bazel"],
        quiet=True, as_user=real_user,
    ).returncode == 0)

    glibc = glibc_version()
    print()
    print(f"  glibc version: {glibc if glibc is not None else 'unknown'}")
    if glibc is not None and glibc < 2.35:
        warn("glibc < 2.35: the tcmalloc-via-Bazel allocator-compare target "
             "will NOT build")
        warn("(this is a host limitation per CLAUDE.md, not a smash bug). "
             "gperftools tcmalloc")
        warn("above still works for the LD_PRELOAD allocator benches.")

    print("""
Next steps to build + run the heavy suite:
  cd build
  cmake .. -DSMASH_BUILD_BENCH=ON \\
           -DSMASH_BUILD_BENCH_DEPS=ON \\
           -DSMASH_BUILD_BENCH_ALLOCATORS=ON
  make -j$(nproc)
  # then e.g.:
  bash bench/bench_redis.sh --quick
  bash bench/bench_rocksdb.sh --quick
  python3 bench/bench_allocator_compare.py

If bazelisk was installed to ~/.local/bin, make sure that's on PATH first:
  export PATH="$HOME/.local/bin:$PATH"
""")


if __name__ == "__main__":
    main()
