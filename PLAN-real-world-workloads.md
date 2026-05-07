# Real-world workload coverage for smash

PR #10 closed with a comprehensive syscall surface but no real-world
multi-hundred-MB workloads beyond the existing benches (SQLite,
RocksDB, Redis, memcached). The Firefox investigation showed that
**any browser or Electron app bundles its own allocator and bypasses
libsmash's malloc**. This plan covers (1) building Firefox with
`--disable-jemalloc` so we can finally measure compression on it, and
(2) picking other real-world apps that work with smash *without* a
custom build.

## Phase 1 — Firefox with `--disable-jemalloc`

**Goal:** end-state numbers showing what fraction of Firefox's heap
smash compresses on a real browsing workload.

### Build steps

```sh
# Prereqs
sudo apt-get install -y mercurial python3 python3-pip clang lld \
  libgtk-3-dev libpulse-dev libdbus-glib-1-dev libxt-dev \
  libasound2-dev libxcb-shm0-dev libnss3-dev pkg-config
mkdir ~/firefox-src && cd ~/firefox-src

# Pull source (use stable rather than nightly to keep this reproducible)
hg clone https://hg.mozilla.org/releases/mozilla-esr115/   # or current ESR
cd mozilla-esr115

# Bootstrap the toolchain Mozilla expects (Rust, sccache, clang etc.)
./mach bootstrap --application-choice browser --no-system-changes

# mozconfig — disable jemalloc + enable opt + skip tests
cat > mozconfig <<'EOF'
ac_add_options --enable-application=browser
ac_add_options --enable-release
ac_add_options --disable-tests
ac_add_options --disable-jemalloc
ac_add_options --disable-crashreporter
mk_add_options MOZ_OBJDIR=@TOPSRCDIR@/obj-system-malloc
EOF

# Build (~2-4 hours on 8-core, ~30 GB disk)
./mach build
./mach package    # creates obj-system-malloc/dist/firefox-*.tar.bz2
```

### Verify the build is allocator-free

```sh
# Should show no mozjemalloc symbols, libxul should call libc malloc
nm obj-system-malloc/dist/bin/libxul.so | grep -c jemalloc   # expect 0
ldd obj-system-malloc/dist/bin/firefox | grep libc           # libc.so.6 present
```

### Test under smash

```sh
SMASH_BANNER=1 SMASH_DEBUG=1 SMASH_STATS=1 \
  MOZ_DISABLE_CONTENT_SANDBOX=1 \
  LD_PRELOAD=$PWD/linux-build/libsmash.so \
  ~/firefox-src/mozilla-esr115/obj-system-malloc/dist/bin/firefox \
  --headless --no-remote --profile $(mktemp -d) \
  https://en.wikipedia.org/wiki/Compression
# Browse for a few minutes / open multiple tabs
```

In the periodic `[smash stats]` output, `committed=N` should grow into
the tens of thousands of pages (N×4KB), and `compressed=N` should be a
meaningful fraction. That's the headline number.

### Skip if Phase 2 covers what we need

Building Firefox is a 2-4 hour investment plus 30 GB disk. If the
workloads in Phase 2 give us comparable signal (real-world apps with
hundreds of MB working set on system malloc), Phase 1 may not be worth
the cost. Decide after Phase 2.

## Phase 2 — Apps that already use system malloc

These need no rebuild. All allocate via libc, so smash interposes
directly.

### Confirmed system-malloc, plausible smash workloads

| App | Why it's a good target | Notes |
|---|---|---|
| **LibreOffice** | Hundreds of MB RSS for a typical doc; mixed text + image data → heterogeneous compressibility | Open a large .docx/.odp; smash's per-arena routing should differentiate text from image arenas |
| **Inkscape / GIMP** | Image manipulation, large bitmap allocations | GIMP is a classic memory-pressure test target |
| **PostgreSQL / MySQL** | Long-lived shared-buffer pool, query workspace | Run pgbench / sysbench against; we already partially cover this in `bench_*` |
| **nginx with large worker pool** | Many worker processes, each system-malloc | OK signal but small per-worker working set |
| **Native editors**: vim/neovim/helix/kate | Small, low-pressure | Useful as smoke tests, not for compression numbers |
| **Apache OpenOffice** | Same family as LibreOffice but older; slower than LO | Use LibreOffice instead |

### Quick verification protocol

For each candidate, run the same diagnostic pattern PR #10 used:

```sh
# 1. Confirm libsmash actually maps in
SMASH_BANNER=1 LD_PRELOAD=$PWD/linux-build/libsmash.so $APP --version
# Should print [smash] [...] loaded pid=N ...

# 2. Run under smash for 1-2 minutes of real workload
SMASH_BANNER=1 SMASH_DEBUG=1 SMASH_STATS=1 \
  LD_PRELOAD=$PWD/linux-build/libsmash.so $APP <typical-workload>
# Watch [smash stats] lines: committed should grow, compressed should
# be a meaningful fraction after the cold-tick threshold.

# 3. Capture exit stats and compare to without smash
```

## Phase 3 — Apps with bundled allocators (need rebuild)

Documented for completeness, but lower priority since they require the
same multi-hour build investment as Firefox:

| App | Allocator | How to disable |
|---|---|---|
| Chromium | PartitionAlloc | `gn gen --args='use_partition_alloc_as_malloc=false'` then ninja |
| VS Code (Electron) | Chromium → PartitionAlloc | Have to rebuild Electron itself; effectively impossible without committing to that toolchain |
| Node.js | V8 + libc | V8 heap is JS-only; native malloc passes through libc — actually works with smash directly for non-JS allocations |
| Redis | Bundled jemalloc by default | `make MALLOC=libc` to use system malloc; we already test this in `bench/bench_redis.sh` |

## Order of operations

1. **Phase 2 first** (low cost, high signal): pick LibreOffice and
   GIMP, run the diagnostic protocol, paste the `[smash stats]` numbers
   into a follow-up issue/PR. If we see meaningful compression there,
   smash's claims are already supported on real workloads.
2. **Phase 1 only if Phase 2 leaves gaps**: e.g., if we want
   specifically to claim coverage for browser-class workloads (memory
   profile is genuinely different — JS heap vs document heap). Worth
   the 2-4 hour build only if that distinction matters for the paper.
3. **Phase 3 (Chromium etc.) — skip**: same workload character as
   Firefox, so Phase 1 numbers cover the claim.

## Critical files / no code changes here

This is a workflow plan, not a code-change plan. No files in
`src/` are modified. Output is:

- A handful of measurement runs producing `[smash stats]` numbers
- Optionally a `bench/run_real_world_workloads.sh` harness that
  automates Phase 2 across LibreOffice / GIMP / others (not in scope
  of this plan; spin off only after manual verification)

## Verification

For each app tested in Phase 2, capture:

1. `[smash stats]` peak `committed` count (working set smash sees)
2. `[smash stats]` peak `compressed/committed` ratio (compression effectiveness)
3. RSS comparison: with-smash vs without-smash via `/usr/bin/time -v`
4. No crashes / functional regressions over a 5-minute interactive run

Pass criteria for "smash works on real workloads": at least one Phase 2
app shows ≥ 30% RSS reduction with ≥ 50% of committed pages compressed
over a 5-minute typical workload, with no functional issues.
