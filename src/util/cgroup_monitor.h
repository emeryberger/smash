// smash/src/util/cgroup_monitor.h - cgroup v2 memory-limit detection + policy
//
// Containers (Docker, Kubernetes, systemd services) bill memory through a
// cgroup v2 memory controller: memory.current counts anon + page cache +
// kernel memory + the zswap pool, and memory.high / memory.max cap it. Near
// the cap the kernel reclaims inside the cgroup: it throttles allocating
// tasks, drops clean page cache and swaps anonymous pages. Two things in that
// regime are bad for smash specifically (measurements: agents/containers.md):
//
//   * smash's CompressStore pages hold already-compressed blobs. zswap cannot
//     compress them again, rejects them (reject_compress_fail) and they go to
//     the disk swap device, so a later cold access pays a disk read plus a
//     decompress. Under glibc+zswap the same data is compressible and stays
//     in zswap's RAM pool. Response: CompressStore::keepWarm() each tick.
//   * The compressor snapshots a page to compress it. If the kernel already
//     swapped that page out, the snapshot swaps it back IN (charging the
//     cgroup, pushing the reclaimer to evict something else) only to build a
//     blob about the size of the copy zswap already holds. Response: skip
//     pages mincore() reports non-resident.
//
// A third response, compressing sooner under pressure (Decision::cold_floor),
// is implemented but opt-in (SMASH_CGROUP_EARLY=1): it lowered SQLite's mean
// memory.current but cost Redis ~25% GET throughput under a 160M cap.
//
// This header is split in two so the logic is unit-testable off Linux:
//
//   1. Pure parsers + a pure policy function (no I/O, no allocation):
//      parseSelfCgroup / parseLimit / parseKeyed / parsePsi / decide().
//   2. Monitor: locates the process's cgroup directory (from /proc/self/cgroup
//      under the cgroup2 mount, or SMASH_CGROUP_ROOT for tests), walks up to
//      every ancestor that carries a memory limit (a Kubernetes container has
//      a limit on both its own and the pod cgroup), and samples them with raw
//      open/read/close syscalls into stack buffers. No malloc, no stdio: safe
//      to call from the compressor thread at any point.
//
// Policy is decided per sample (once per compressor tick, 1 s) and consumed
// by phase 2 in compressor_thread.h; see decide() for the rules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace smash::cgroup {

inline constexpr int64_t kUnlimited = -1;   // "max"
inline constexpr int64_t kParseError = -2;
inline constexpr int kMaxDepth = 8;          // limited ancestors tracked
inline constexpr size_t kPathMax = 384;

// ── Pure parsers ───────────────────────────────────────────────────────────

// /proc/self/cgroup on a cgroup v2 host holds a single "0::<path>" line
// (hybrid hosts add v1 "N:ctrl:<path>" lines first). Copies <path> (e.g.
// "/kubepods.slice/.../cri-containerd-x.scope", or "/" inside a cgroup
// namespace) into out. Returns false if no v2 line is present.
inline bool parseSelfCgroup(const char* buf, char* out, size_t outsz) {
    const char* p = buf;
    while (p && *p) {
        if (p[0] == '0' && p[1] == ':' && p[2] == ':') {
            const char* s = p + 3;
            size_t n = 0;
            while (s[n] && s[n] != '\n') ++n;
            if (n == 0 || n + 1 > outsz) return false;
            std::memcpy(out, s, n);
            out[n] = '\0';
            return true;
        }
        p = std::strchr(p, '\n');
        if (p) ++p;
    }
    return false;
}

// memory.high / memory.max / memory.current: "max\n" or a decimal byte count.
inline int64_t parseLimit(const char* buf) {
    while (*buf == ' ' || *buf == '\t') ++buf;
    if (std::strncmp(buf, "max", 3) == 0) return kUnlimited;
    if (*buf < '0' || *buf > '9') return kParseError;
    int64_t v = 0;
    while (*buf >= '0' && *buf <= '9') {
        v = v * 10 + (*buf - '0');
        ++buf;
    }
    return v;
}

// "key value\n" files (memory.events, memory.stat). Matches whole keys only.
inline bool parseKeyed(const char* buf, const char* key, uint64_t* out) {
    const size_t kl = std::strlen(key);
    const char* p = buf;
    while (p && *p) {
        if (std::strncmp(p, key, kl) == 0 && p[kl] == ' ') {
            const char* s = p + kl + 1;
            uint64_t v = 0;
            bool any = false;
            while (*s >= '0' && *s <= '9') { v = v * 10 + static_cast<uint64_t>(*s - '0'); ++s; any = true; }
            if (!any) return false;
            *out = v;
            return true;
        }
        p = std::strchr(p, '\n');
        if (p) ++p;
    }
    return false;
}

struct PsiLine {
    uint32_t avg10_x100 = 0;   // avg10 percentage * 100 (e.g. 12.34% -> 1234)
    uint64_t total_us = 0;     // cumulative stall time
    bool valid = false;
};

// memory.pressure: "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
//                  "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
inline bool parsePsi(const char* buf, PsiLine* some, PsiLine* full) {
    auto parseLine = [](const char* p, PsiLine* o) {
        const char* a = std::strstr(p, "avg10=");
        const char* t = std::strstr(p, "total=");
        const char* eol = std::strchr(p, '\n');
        if (!a || !t || (eol && (a > eol || t > eol))) return;
        a += 6;
        uint32_t ip = 0, fp = 0, fd = 0;
        while (*a >= '0' && *a <= '9') { ip = ip * 10 + static_cast<uint32_t>(*a - '0'); ++a; }
        if (*a == '.') {
            ++a;
            while (*a >= '0' && *a <= '9' && fd < 2) { fp = fp * 10 + static_cast<uint32_t>(*a - '0'); ++a; ++fd; }
            if (fd == 1) fp *= 10;
        }
        o->avg10_x100 = ip * 100 + fp;
        t += 6;
        uint64_t v = 0;
        while (*t >= '0' && *t <= '9') { v = v * 10 + static_cast<uint64_t>(*t - '0'); ++t; }
        o->total_us = v;
        o->valid = true;
    };
    bool any = false;
    const char* p = buf;
    while (p && *p) {
        if (std::strncmp(p, "some ", 5) == 0 && some) { parseLine(p, some); any = any || some->valid; }
        if (std::strncmp(p, "full ", 5) == 0 && full) { parseLine(p, full); any = any || full->valid; }
        p = std::strchr(p, '\n');
        if (p) ++p;
    }
    return any;
}

// ── Sample + policy ────────────────────────────────────────────────────────

struct Sample {
    bool valid = false;
    int64_t limit = kUnlimited;     // tightest of memory.high / memory.max on the chain
    int64_t current = 0;            // memory.current of the cgroup owning `limit`
    int64_t headroom = 0;           // limit - current (may be < 0 above memory.high)
    int binding_depth = -1;         // 0 = own cgroup, 1 = parent, ...
    uint64_t events_high = 0;       // memory.events "high" (reclaim-at-high count)
    uint64_t events_max = 0;        // memory.events "max"
    uint64_t events_oom_kill = 0;
    PsiLine psi_some;               // own cgroup's memory.pressure
    PsiLine psi_full;
};

enum class Pressure : uint8_t { kNone = 0, kElevated = 1, kCritical = 2 };

struct PolicyParams {
    // Elevated when headroom < elevated_pct% of the limit, Critical below
    // critical_pct%. memory.high reclaim keeps memory.current pinned AT the
    // limit, so "headroom ~0" is the steady state of a tight container.
    uint32_t elevated_pct = 10;
    uint32_t critical_pct = 3;
    // PSI memory "some" avg10 (x100) at which we treat the cgroup as Critical
    // regardless of headroom (stalls are the cost we are trying to avoid).
    uint32_t psi_critical_x100 = 1000;   // 10%
    // Pressure is sticky for this many samples after the last trigger, so
    // the policy does not flap while the kernel oscillates around high.
    uint32_t hold_samples = 10;
};

struct Decision {
    Pressure level = Pressure::kNone;
    // Leave pages the kernel already swapped out (mincore: non-resident) to
    // the kernel instead of faulting them back in to compress them.
    bool skip_nonresident = false;
    // Override for the cold-tick floor (0 = no override). Under pressure the
    // compressor compresses sooner so that idle data is compressed by smash
    // while it is still resident instead of being swapped by the kernel.
    uint32_t cold_floor = 0;
};

// Pure policy. `prev_level` + `*hold` implement the sticky window; the caller
// keeps both across samples. `base_floor` is the configured cold-tick floor.
inline Decision decide(const Sample& s, const PolicyParams& p, Pressure prev_level,
                       uint32_t* hold, uint32_t base_floor, uint32_t min_floor) {
    Decision d;
    Pressure now = Pressure::kNone;
    if (s.valid && s.limit > 0) {
        const int64_t elev = s.limit / 100 * static_cast<int64_t>(p.elevated_pct);
        const int64_t crit = s.limit / 100 * static_cast<int64_t>(p.critical_pct);
        if (s.headroom < crit) now = Pressure::kCritical;
        else if (s.headroom < elev) now = Pressure::kElevated;
        if (s.psi_some.valid && s.psi_some.avg10_x100 >= p.psi_critical_x100)
            now = Pressure::kCritical;
    }
    if (now != Pressure::kNone) {
        *hold = p.hold_samples;
    } else if (*hold > 0 && prev_level != Pressure::kNone) {
        --*hold;
        now = prev_level;   // sticky: keep the previous level while held
    }
    d.level = now;
    if (now == Pressure::kNone) return d;
    d.skip_nonresident = true;
    if (now == Pressure::kCritical) {
        d.cold_floor = min_floor < base_floor ? min_floor : base_floor;
    } else {
        uint32_t half = base_floor / 2;
        if (half < min_floor) half = min_floor;
        d.cold_floor = half < base_floor ? half : base_floor;
    }
    return d;
}

// ── Monitor (I/O) ──────────────────────────────────────────────────────────

class Monitor {
public:
    // Locate the cgroup chain. root_override: SMASH_CGROUP_ROOT — treat this
    // directory as the process's own cgroup (tests point it at a fake tree).
    // Otherwise read /proc/self/cgroup and resolve under /sys/fs/cgroup.
    // Returns true if at least one cgroup on the chain has a memory limit
    // (memory.high or memory.max != "max") — i.e. the process is capped.
    bool init(const char* root_override) {
        ndirs_ = 0;
        char leaf[kPathMax];
        if (root_override && root_override[0]) {
            if (!copyStr(leaf, sizeof(leaf), root_override)) return false;
        } else {
            char buf[1024];
            if (readFile("/proc/self/cgroup", buf, sizeof(buf)) <= 0) return false;
            char rel[kPathMax];
            if (!parseSelfCgroup(buf, rel, sizeof(rel))) return false;
            const char* mnt = "/sys/fs/cgroup";
            size_t ml = std::strlen(mnt), rl = std::strlen(rel);
            if (ml + rl + 1 > sizeof(leaf)) return false;
            std::memcpy(leaf, mnt, ml);
            std::memcpy(leaf + ml, rel, rl + 1);
            // "/" → "/sys/fs/cgroup/": strip a trailing slash.
            size_t l = ml + rl;
            while (l > 1 && leaf[l - 1] == '/') leaf[--l] = '\0';
        }
        // Walk up while the directory has a memory controller (memory.max
        // exists; the root cgroup has none). Keep only limited ancestors,
        // but always keep the leaf (its PSI is the process's own stall).
        char cur[kPathMax];
        copyStr(cur, sizeof(cur), leaf);
        bool any_limit = false;
        for (int depth = 0; depth < 32; ++depth) {
            char f[kPathMax + 16];
            if (!join(f, sizeof(f), cur, "memory.max")) break;
            char b[64];
            if (readFile(f, b, sizeof(b)) <= 0) break;   // no controller: stop
            int64_t mx = parseLimit(b);
            int64_t hi = kUnlimited;
            if (join(f, sizeof(f), cur, "memory.high") && readFile(f, b, sizeof(b)) > 0)
                hi = parseLimit(b);
            bool limited = mx > 0 || hi > 0;
            if ((depth == 0 || limited) && ndirs_ < kMaxDepth) {
                copyStr(dirs_[ndirs_], kPathMax, cur);
                depth_[ndirs_] = depth;
                ++ndirs_;
            }
            any_limit = any_limit || limited;
            // Parent directory.
            char* slash = std::strrchr(cur, '/');
            if (!slash || slash == cur) break;
            *slash = '\0';
        }
        capped_ = any_limit;
        return capped_;
    }

    bool capped() const { return capped_; }
    int depthCount() const { return ndirs_; }
    const char* leafDir() const { return ndirs_ ? dirs_[0] : ""; }

    // One sample across the chain: ~3 small file reads per limited cgroup
    // plus memory.events + memory.pressure. Raw syscalls, stack buffers.
    Sample sample() const {
        Sample s;
        if (ndirs_ == 0) return s;
        char f[kPathMax + 16];
        char b[1024];
        int binding = -1;
        for (int i = 0; i < ndirs_; ++i) {
            int64_t hi = kUnlimited, mx = kUnlimited, cur = -1;
            if (join(f, sizeof(f), dirs_[i], "memory.high") && readFile(f, b, 64) > 0) hi = parseLimit(b);
            if (join(f, sizeof(f), dirs_[i], "memory.max") && readFile(f, b, 64) > 0) mx = parseLimit(b);
            if (join(f, sizeof(f), dirs_[i], "memory.current") && readFile(f, b, 64) > 0) cur = parseLimit(b);
            int64_t lim = kUnlimited;
            if (hi > 0) lim = hi;
            if (mx > 0 && (lim < 0 || mx < lim)) lim = mx;
            if (lim <= 0 || cur < 0) continue;
            int64_t hr = lim - cur;
            if (binding < 0 || hr < s.headroom) {
                s.limit = lim;
                s.current = cur;
                s.headroom = hr;
                s.binding_depth = depth_[i];
                binding = i;
            }
        }
        if (binding >= 0) {
            if (join(f, sizeof(f), dirs_[binding], "memory.events") && readFile(f, b, sizeof(b)) > 0) {
                parseKeyed(b, "high", &s.events_high);
                parseKeyed(b, "max", &s.events_max);
                parseKeyed(b, "oom_kill", &s.events_oom_kill);
            }
            s.valid = true;
        }
        if (join(f, sizeof(f), dirs_[0], "memory.pressure") && readFile(f, b, sizeof(b)) > 0)
            parsePsi(b, &s.psi_some, &s.psi_full);
        return s;
    }

private:
    char dirs_[kMaxDepth][kPathMax] = {};
    int depth_[kMaxDepth] = {};
    int ndirs_ = 0;
    bool capped_ = false;

    static bool copyStr(char* dst, size_t n, const char* src) {
        size_t l = std::strlen(src);
        if (l + 1 > n) return false;
        std::memcpy(dst, src, l + 1);
        return true;
    }
    static bool join(char* dst, size_t n, const char* dir, const char* file) {
        size_t dl = std::strlen(dir), fl = std::strlen(file);
        if (dl + 1 + fl + 1 > n) return false;
        std::memcpy(dst, dir, dl);
        dst[dl] = '/';
        std::memcpy(dst + dl + 1, file, fl + 1);
        return true;
    }
    // Read up to n-1 bytes and NUL-terminate. Raw syscalls: smash interposes
    // read() (EFAULT retry) and must not recurse into itself here.
    static long readFile(const char* path, char* buf, size_t n) {
#if defined(__linux__)
        long fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        long got = 0;
        while (static_cast<size_t>(got) < n - 1) {
            long r = syscall(SYS_read, static_cast<int>(fd), buf + got, n - 1 - static_cast<size_t>(got));
            if (r <= 0) break;
            got += r;
        }
        syscall(SYS_close, static_cast<int>(fd));
        buf[got] = '\0';
        return got;
#else
        // cgroup v2 is Linux-only: /proc/self/cgroup does not exist elsewhere,
        // so init() never reports a cap. Plain POSIX I/O keeps the fake-tree
        // tests (SMASH_CGROUP_ROOT) runnable on macOS.
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        long got = 0;
        while (static_cast<size_t>(got) < n - 1) {
            ssize_t r = ::read(fd, buf + got, n - 1 - static_cast<size_t>(got));
            if (r <= 0) break;
            got += r;
        }
        ::close(fd);
        buf[got] = '\0';
        return got;
#endif
    }
};

}  // namespace smash::cgroup
