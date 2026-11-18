// smash-estimate: how much would smash save this process? (Linux)
//
//   smash-estimate [options] <pid>
//   smash-estimate [options] -- <command> [args...]
//
// No preload, no restart. Finds the target's anonymous pages that stay idle
// over a window, samples their contents, compresses the samples with zstd-1
// (smash's fast tier) applying smash's store rules (16 B blob granularity,
// 0.75 ratio gate, all-zero pages free), and projects the RSS reduction with
// a 95% confidence interval from sampling.
//
// It never writes the target's memory. It does write kernel tracking state:
//   - /proc/<pid>/clear_refs ("1" referenced bits, "4" soft-dirty bits): the
//     kernel write-protects / ages PTEs, so the target takes minor faults on
//     its next write to each page and its pages look older to reclaim. A
//     target that uses soft-dirty itself (CRIU, a smash run) loses that
//     tick's write information.
//   - /sys/kernel/mm/page_idle/bitmap (idle method): sets idle flags.
//
// Idle detection methods (--method, default auto = first that works):
//   idle        page_idle bitmap. Reads AND writes, per page. Needs root
//               (CAP_SYS_ADMIN for PFNs) and CONFIG_IDLE_PAGE_TRACKING.
//               Pages of a THP are tracked through the folio head PFN
//               (/proc/kpageflags), so one access marks the whole folio.
//               smash-idled cannot stand in for root here: it only answers
//               for the connecting process's own pages (agents/estimator.md).
//   soft-dirty  clear_refs 4 + pagemap bit 55. Writes only, per page. Same uid
//               is enough; the arch must implement soft-dirty (x86-64 yes,
//               arm64 no). Combined with the referenced method below to cap
//               each mapping's idle count by its unreferenced bytes, which
//               brings reads back in at mapping granularity.
//   referenced  clear_refs 1 + smaps "Referenced". Reads and writes, but only
//               per mapping; samples are drawn from all present pages of the
//               mapping instead of from its idle pages.
//
// Reading page contents (process_vm_readv) needs ptrace access: root, the
// target's parent (the "-- command" form), kernel.yama.ptrace_scope=0, or a
// target that called prctl(PR_SET_PTRACER). Without it, idle bytes are still
// reported, but no compressibility or savings.
//
// See agents/estimator.md for what the projection does and does not capture.

#include <zstd.h>

#include "core/size_classes.h"  // smash's slab size classes (allocator-layout model)
#include "vm/page_idle.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <random>
#include <string>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr double kMinCompressRatio = 0.75;  // smash's default kMinCompressRatio
constexpr int kZstdLevel = 1;               // smash's default fast tier

size_t g_page = 4096;

struct Options {
    pid_t pid = 0;
    std::vector<char*> cmd;
    double window = 10.0;      // seconds
    int points = 3;            // idle-age sample points within the window
    double warmup = 5.0;       // launch mode: seconds before measuring
    size_t samples = 4096;     // pages to read + compress
    std::string method = "auto";
    bool json = false;
    bool kill_child = false;
    unsigned seed = 1;
    double cold = 10.0;        // smash's cold timeout (SMASH_COLD_TIMEOUT_SEC default)
};

[[noreturn]] void usage(int rc) {
    fprintf(rc ? stderr : stdout,
        "usage: smash-estimate [options] <pid>\n"
        "       smash-estimate [options] -- <command> [args...]\n"
        "  --window S       idle window in seconds (default 10)\n"
        "  --points N       idle-age sample points within the window (default 3)\n"
        "  --warmup S       launch mode: run S seconds before measuring (default 5)\n"
        "  --samples N      cold pages to read and compress (default 4096)\n"
        "  --method M       auto | idle | soft-dirty | referenced (default auto)\n"
        "  --seed N         sampling seed (default 1)\n"
        "  --cold-timeout S smash's cold timeout the projection assumes (default 10); a\n"
        "                   window with points at S+2 and 2S+1 s also models refault backoff:\n"
        "                   --window 21 --points 7 for the default (see agents/estimator.md)\n"
        "  --kill           launch mode: kill the command after the estimate\n"
        "  --json           JSON on stdout instead of the text report\n");
    exit(rc);
}

Options parse(int argc, char** argv) {
    Options o;
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) usage(2); return argv[++i]; };
        if (a == "--") { ++i; break; }
        else if (a == "-h" || a == "--help") usage(0);
        else if (a == "--window") o.window = atof(val());
        else if (a == "--points") o.points = atoi(val());
        else if (a == "--warmup") o.warmup = atof(val());
        else if (a == "--samples") o.samples = static_cast<size_t>(atol(val()));
        else if (a == "--method") o.method = val();
        else if (a == "--seed") o.seed = static_cast<unsigned>(atol(val()));
        else if (a == "--cold-timeout") o.cold = atof(val());
        else if (a == "--kill") o.kill_child = true;
        else if (a == "--json") o.json = true;
        else if (!a.empty() && a[0] != '-' && o.pid == 0) o.pid = static_cast<pid_t>(atol(a.c_str()));
        else { fprintf(stderr, "smash-estimate: unknown argument '%s'\n", a.c_str()); usage(2); }
    }
    for (; i < argc; ++i) o.cmd.push_back(argv[i]);
    if ((o.pid == 0) == o.cmd.empty()) usage(2);
    if (o.points < 1) o.points = 1;
    if (o.points > 16) o.points = 16;
    if (o.window <= 0) o.window = 1;
    if (o.cold <= 0) o.cold = 10;
    o.cmd.push_back(nullptr);
    return o;
}

// ── /proc parsing ───────────────────────────────────────────────────────────

enum Class { kHeap, kArena, kAnon, kStack, kNumClass };
const char* kClassName[kNumClass] = {"heap", "malloc-arena", "anon-mmap", "stack"};
const char* kClassNote[kNumClass] = {
    "brk heap (glibc small allocations): managed in full mode; large-only mode only manages "
    "allocations >= 1 MiB (rare here)",
    "glibc thread arena (small allocations): managed in full mode only",
    "anonymous mmap: managed if these are malloc allocations >= 1 MiB (both modes); direct mmaps "
    "need SMASH_TRACK_EXTERNAL=1",
    "stack: never managed by smash"};

struct Mapping {
    uintptr_t start = 0, end = 0;
    std::string name;
    Class cls = kAnon;
    // smaps
    uint64_t rss_kb = 0, anon_kb = 0, ref0_kb = 0;
    std::vector<uint64_t> ref_kb;   // Referenced at each point
    std::vector<uint64_t> anon_at;  // Anonymous at each point
    // per-page tracking (sorted by address; pages appearing at a point join then)
    std::vector<uintptr_t> pages;   // page-aligned vaddrs
    std::vector<uint64_t> pfn;      // idle method: PFN when first tracked
    std::vector<uint64_t> head;     // idle method: its folio's head PFN (bitmap index)
    std::vector<uint32_t> hot;      // bit k-1: accessed in segment k (between points k-1 and k)
    std::vector<uint8_t> born;      // tracked from segment born+1 on (0 = present at t0)
    std::vector<uint8_t> gone;      // no longer present / remapped
};

// Trailing idle segments of tracked page j at the end of a window of P segments.
int trailingIdle(const Mapping& m, size_t j, int P) {
    int t = 0;
    for (int seg = P; seg > m.born[j]; --seg) {
        if (m.hot[j] & (1u << (seg - 1))) break;
        ++t;
    }
    return t;
}


bool readFile(const std::string& path, std::string& out) {
    out.clear();
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return true;
}

bool writeStr(const std::string& path, const char* s) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write(fd, s, strlen(s)) == static_cast<ssize_t>(strlen(s));
    close(fd);
    return ok;
}

// Anonymous private writable mappings from /proc/<pid>/maps.
std::vector<Mapping> readMaps(pid_t pid) {
    std::vector<Mapping> out;
    std::string txt;
    if (!readFile("/proc/" + std::to_string(pid) + "/maps", txt)) return out;
    size_t pos = 0;
    uintptr_t prev_end = 0;
    std::string prev_perms;
    while (pos < txt.size()) {
        size_t nl = txt.find('\n', pos);
        if (nl == std::string::npos) nl = txt.size();
        std::string line = txt.substr(pos, nl - pos);
        pos = nl + 1;
        unsigned long s = 0, e = 0, off = 0, ino = 0;
        char perms[8] = {}, dev[16] = {};
        int name_at = 0;
        if (sscanf(line.c_str(), "%lx-%lx %7s %lx %15s %lu %n", &s, &e, perms, &off, dev, &ino, &name_at) < 6)
            continue;
        std::string name = name_at > 0 && name_at <= static_cast<int>(line.size()) ? line.substr(name_at) : "";
        const bool rw_private = perms[0] == 'r' && perms[1] == 'w' && perms[3] == 'p';
        const bool anon = ino == 0 && (name.empty() || name[0] == '[');
        const uintptr_t this_prev_end = prev_end;
        const std::string this_prev_perms = prev_perms;
        prev_end = e;
        prev_perms = perms;
        if (!rw_private || !anon) continue;
        if (name == "[vvar]" || name == "[vdso]" || name == "[vsyscall]" || name == "[vvar_vclock]") continue;
        Mapping m;
        m.start = s;
        m.end = e;
        m.name = name;
        if (name == "[heap]") m.cls = kHeap;
        else if (name.rfind("[stack", 0) == 0) m.cls = kStack;
        else if (name.empty() && (s & ((64UL << 20) - 1)) == 0 && (e - s) <= (64UL << 20))
            m.cls = kArena;  // glibc non-main arenas are 64 MiB-aligned HEAP_MAX_SIZE reservations
        else if (name.empty() && this_prev_end == s && this_prev_perms.rfind("---p", 0) == 0 &&
                 (e - s) == (8UL << 20))
            m.cls = kStack;  // default-size pthread stack (RLIMIT_STACK 8 MiB) above its guard
        else m.cls = kAnon;
        out.push_back(std::move(m));
    }
    return out;
}

// A growing brk heap moves its end; pick up the current end of each mapping
// (matched by start, as readSmaps does).
void refreshEnds(pid_t pid, std::vector<Mapping>& maps) {
    std::string txt;
    if (!readFile("/proc/" + std::to_string(pid) + "/maps", txt)) return;
    std::map<uintptr_t, Mapping*> by;
    for (auto& m : maps) by[m.start] = &m;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t nl = txt.find('\n', pos);
        if (nl == std::string::npos) nl = txt.size();
        unsigned long s = 0, e = 0;
        if (sscanf(txt.c_str() + pos, "%lx-%lx", &s, &e) == 2) {
            auto it = by.find(s);
            if (it != by.end()) it->second->end = e;
        }
        pos = nl + 1;
    }
}

// Thread stacks: each task's /proc/<pid>/task/<tid>/syscall ends with its
// stack pointer and pc (readable with ptrace access; "running" tasks report
// no registers). Mark the anonymous mapping holding any sp as a stack.
void markThreadStacks(pid_t pid, std::vector<Mapping>& maps) {
    std::string dir = "/proc/" + std::to_string(pid) + "/task";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* de = readdir(d)) {
        if (de->d_name[0] == '.') continue;
        const std::string tid = de->d_name;
        std::string txt;
        if (!readFile(dir + "/" + tid + "/syscall", txt)) continue;
        // "<nr> a0 .. a5 sp pc" ; sp is the second-to-last field
        std::vector<std::string> tok;
        size_t p = 0;
        while (p < txt.size()) {
            while (p < txt.size() && isspace(static_cast<unsigned char>(txt[p]))) ++p;
            size_t q = p;
            while (q < txt.size() && !isspace(static_cast<unsigned char>(txt[q]))) ++q;
            if (q > p) tok.push_back(txt.substr(p, q - p));
            p = q;
        }
        if (tok.size() < 3) continue;
        uintptr_t sp = strtoull(tok[tok.size() - 2].c_str(), nullptr, 16);
        for (auto& m : maps)
            if (sp >= m.start && sp < m.end) m.cls = kStack;
    }
    closedir(d);
}

// smaps: Rss / Anonymous / Referenced per mapping (matched by start address).
bool readSmaps(pid_t pid, std::vector<Mapping>& maps, int point) {
    std::string txt;
    if (!readFile("/proc/" + std::to_string(pid) + "/smaps", txt)) return false;
    std::map<uintptr_t, Mapping*> by;
    for (auto& m : maps) by[m.start] = &m;
    Mapping* cur = nullptr;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t nl = txt.find('\n', pos);
        if (nl == std::string::npos) nl = txt.size();
        const char* l = txt.c_str() + pos;
        unsigned long s, e;
        if (sscanf(l, "%lx-%lx ", &s, &e) == 2 && strchr(std::string(l, nl - pos).c_str(), ' ')) {
            // Match on start only: a growing brk heap keeps its start.
            auto it = by.find(s);
            cur = it != by.end() ? it->second : nullptr;
        } else if (cur) {
            unsigned long kb;
            if (point < 0 && sscanf(l, "Rss: %lu kB", &kb) == 1) cur->rss_kb = kb;
            else if (sscanf(l, "Anonymous: %lu kB", &kb) == 1) {
                if (point < 0) cur->anon_kb = kb;
                else { cur->anon_at.resize(static_cast<size_t>(point) + 1); cur->anon_at[point] = kb; }
            }
            else if (sscanf(l, "Referenced: %lu kB", &kb) == 1) {
                if (point < 0) cur->ref0_kb = kb;
                else { cur->ref_kb.resize(static_cast<size_t>(point) + 1); cur->ref_kb[point] = kb; }
            }
        }
        pos = nl + 1;
    }
    return true;
}

uint64_t statusKb(pid_t pid, const char* key) {
    std::string txt;
    if (!readFile("/proc/" + std::to_string(pid) + "/status", txt)) return 0;
    size_t p = txt.find(key);
    if (p == std::string::npos) return 0;
    return strtoull(txt.c_str() + p + strlen(key), nullptr, 10);
}

std::string comm(pid_t pid) {
    std::string s;
    readFile("/proc/" + std::to_string(pid) + "/comm", s);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// ── pagemap / idle bitmap ───────────────────────────────────────────────────

constexpr uint64_t kPmPresent = 1ULL << 63, kPmSwap = 1ULL << 62, kPmSoftDirty = 1ULL << 55;
constexpr uint64_t kPmPfnMask = (1ULL << 55) - 1;

// Read pagemap entries for [start, end) in chunks; fn(vaddr, entry).
template <class Fn>
bool scanPagemap(int fd, uintptr_t start, uintptr_t end, Fn fn) {
    std::vector<uint64_t> buf(8192);
    for (uintptr_t a = start; a < end;) {
        size_t n = std::min<size_t>(buf.size(), (end - a) / g_page);
        ssize_t got = pread(fd, buf.data(), n * 8, static_cast<off_t>(a / g_page * 8));
        if (got <= 0) return false;
        size_t k = static_cast<size_t>(got) / 8;
        for (size_t j = 0; j < k; ++j) fn(a + j * g_page, buf[j]);
        a += k * g_page;
    }
    return true;
}

// /proc/kpageflags (root, like PFNs): the idle bitmap tracks a large folio
// (THP, mTHP) by its head PFN only (vm/page_idle.h), and a present pagemap
// entry can be the shared zero page (an anonymous page read but never
// written), which is not in RSS and has no idle state.
using smash::vm::kKpfCompoundTail;
using smash::vm::kKpfZeroPage;

struct KPageFlags {
    int fd = -1;
    std::vector<uint64_t> win = std::vector<uint64_t>(512);
    uint64_t w0 = 0, wn = 0;
    bool get(uint64_t pfn, uint64_t& f) {
        if (fd < 0) return false;
        if (pfn < w0 || pfn >= w0 + wn) {
            w0 = pfn;
            ssize_t r = pread(fd, win.data(), win.size() * 8, static_cast<off_t>(w0 * 8));
            wn = r > 0 ? static_cast<uint64_t>(r) / 8 : 0;
            if (wn == 0) return false;
        }
        f = win[pfn - w0];
        return true;
    }
    bool zero(uint64_t pfn) { uint64_t f; return get(pfn, f) && (f & kKpfZeroPage); }
    bool tail(uint64_t pfn) { uint64_t f; return get(pfn, f) && (f & kKpfCompoundTail); }
};

// Does this kernel/arch set soft-dirty? Probe on our own page.
bool softDirtyWorks() {
    int cr = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    bool ok = false;
    if (cr >= 0 && pm >= 0) {
        std::vector<char> v(2 * g_page);
        volatile char* p = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(v.data()) + g_page - 1) & ~(g_page - 1));
        p[0] = 1;
        if (write(cr, "4", 1) == 1) {
            p[0] = 2;
            uint64_t e = 0;
            if (pread(pm, &e, 8, static_cast<off_t>(reinterpret_cast<uintptr_t>(p) / g_page * 8)) == 8)
                ok = (e & kPmSoftDirty) != 0;
        }
    }
    if (cr >= 0) close(cr);
    if (pm >= 0) close(pm);
    return ok;
}

// ── compression sample ──────────────────────────────────────────────────────

struct Sample { int cls; double saving; double blob; bool zero; bool rejected; };

// Bytes smash's store would charge for this page, and whether it stays
// uncompressed (ratio gate).
void costPage(const unsigned char* p, ZSTD_CCtx* cctx, std::vector<unsigned char>& out,
              double& saving, double& blob, bool& zero, bool& rejected) {
    zero = true;
    for (size_t k = 0; k < g_page; k += 8) {
        uint64_t w;
        memcpy(&w, p + k, 8);
        if (w) { zero = false; break; }
    }
    rejected = false;
    if (zero) { saving = static_cast<double>(g_page); blob = 0; return; }
    size_t c = ZSTD_compressCCtx(cctx, out.data(), out.size(), p, g_page, kZstdLevel);
    if (ZSTD_isError(c) || c > static_cast<size_t>(g_page * kMinCompressRatio)) {
        rejected = true; saving = 0; blob = static_cast<double>(g_page); return;
    }
    size_t a = (c + 15) & ~static_cast<size_t>(15);
    if (a < 16) a = 16;
    blob = static_cast<double>(a);
    saving = static_cast<double>(g_page) - blob;
}

// ── smash's own footprint ───────────────────────────────────────────────────
//
// smash's metadata RSS, from the smash_rss_part_bytes{part="metadata"}
// metric on cloudnew (x86-64, 4 KiB pages; agents/estimator.md): per resident
// page of slab memory (4 KiB objects) and of large allocations (2 MiB
// objects) and per compressed page, from bench_rss runs at 64 MiB - 1 GiB
// before and after compression; the fixed part (compressor workers'
// contexts, thread caches) from bench_sqlite, bench_kv_store and memcached
// runs (3.7 - 4.7 MiB above the per-page terms).
constexpr double kMetaFixed = 4.2 * 1048576, kMetaPerSlabPage = 28, kMetaPerLargePage = 14,
                 kMetaPerCompressedPage = 30;
// RSS smash adds outside its metadata: libsmash's own image, its non-metadata
// mappings (fault-slot and compressor buffers) and thread stacks, measured as
// the smash_rss_part_bytes{part="other"} growth over the program's glibc run
// on bench_sqlite (a C++ program); plus libstdc++ and libm, which libsmash
// pulls in, when the program does not map them already (memcached, C).
constexpr double kRuntimeFixed = 4.4 * 1048576, kRuntimeLibstdcxx = 1.4 * 1048576,
                 kRuntimeLibm = 0.25 * 1048576;

// Bytes smash would use for one glibc chunk of `sz` bytes (header included):
// the smallest request that maps to this chunk size, rounded to smash's size
// class and charged its share of the span (tail waste included); requests
// above the slab range are page-rounded, as in LargeAlloc.
double smashBytesForChunk(uint64_t sz) {
    const size_t req = sz > 24 ? static_cast<size_t>(sz - 23) : 1;
    if (req <= smash::kMaxSmallSize) {
        const smash::SizeClassInfo& c = smash::kSizeClasses[smash::sizeToClass(req)];
        return static_cast<double>(c.pages) * static_cast<double>(smash::kPageSize) / c.objects;
    }
    return static_cast<double>((req + g_page - 1) / g_page * g_page);
}

// Walk the glibc chunk chain of a [heap] or arena mapping (reads only, via
// process_vm_readv in 1 MiB blocks) and total the in-use chunks' bytes under
// glibc and under smash's size classes. The chain is read while the target
// runs, so it can break where the target was modifying it; the walk stops
// there, and the caller uses the in-use ratio of the covered part when it
// spans at least half of the mapping. Non-main arenas start
// with heap_info (+ malloc_state); the first chunk is found by trying 16-byte
// offsets for a chain of at least 32 plausible chunks.
struct LayoutWalk { double glibc = 0, smash = 0, covered = 0; size_t chunks = 0; };
bool walkGlibcHeap(pid_t pid, const Mapping& m, LayoutWalk& w) {
    std::vector<unsigned char> buf(1 << 20);
    uintptr_t bb = 0, be = 0;
    auto rd = [&](uintptr_t a, uint64_t& v) -> bool {
        if (a < m.start || a + 8 > m.end) return false;
        if (a < bb || a + 8 > be) {
            bb = a & ~(static_cast<uintptr_t>(g_page) - 1);
            const size_t n = std::min<size_t>(buf.size(), m.end - bb);
            struct iovec l{buf.data(), n}, r{reinterpret_cast<void*>(bb), n};
            const ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
            if (got < 16) { bb = be = 0; return false; }
            be = bb + static_cast<size_t>(got);
            if (a + 8 > be) return false;
        }
        memcpy(&v, buf.data() + (a - bb), 8);
        return true;
    };
    auto plausible = [&](uintptr_t c, uint64_t& sz) {
        uint64_t v;
        if (!rd(c + 8, v)) return false;
        sz = v & ~static_cast<uint64_t>(7);
        return sz >= 32 && (sz & 15) == 0 && c + sz <= m.end && !(v & 2);  // not IS_MMAPPED
    };
    uintptr_t first = m.start;
    if (m.cls == kArena) {
        first = 0;
        for (uintptr_t off = 32; off < 8192 && !first; off += 16) {
            uintptr_t c = m.start + off;
            uint64_t sz;
            int n = 0;
            while (n < 32 && plausible(c, sz)) { c += sz; ++n; }
            if (n >= 32 || (n > 0 && c == m.end)) first = m.start + off;
        }
        if (!first) return false;
    }
    uintptr_t c = first;
    uint64_t sz;
    if (!plausible(c, sz)) return false;
    while (true) {
        const uintptr_t next = c + sz;
        uint64_t nsz, nv;
        if (next + 16 > m.end || !rd(next + 8, nv)) { c = next; break; }  // c was the top chunk
        nsz = nv & ~static_cast<uint64_t>(7);
        if (nsz < 32 || (nsz & 15) || next + nsz > m.end) { c = next; break; }
        if (nv & 1) {  // PREV_INUSE of the next chunk: c is allocated
            w.glibc += static_cast<double>(sz);
            w.smash += smashBytesForChunk(sz);
            ++w.chunks;
        }
        c = next;
        sz = nsz;
    }
    w.covered = static_cast<double>(c - first);
    return w.chunks > 0;
}

std::string human(double bytes) {
    char b[64];
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    double v = bytes;
    while (std::fabs(v) >= 1024 && i < 4) { v /= 1024; ++i; }
    snprintf(b, sizeof(b), i ? "%.1f %s" : "%.0f %s", v, u[i]);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    g_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    Options o = parse(argc, argv);
    pid_t child = 0;
    if (!o.cmd.empty() && o.cmd[0]) {
        child = fork();
        if (child < 0) { perror("fork"); return 1; }
        if (child == 0) {
            execvp(o.cmd[0], o.cmd.data());
            perror("execvp");
            _exit(127);
        }
        o.pid = child;
        std::this_thread::sleep_for(std::chrono::duration<double>(o.warmup));
        int st;
        if (waitpid(child, &st, WNOHANG) == child) {
            fprintf(stderr, "smash-estimate: command exited during warmup\n");
            return 1;
        }
    }
    const pid_t pid = o.pid;
    const std::string proc = "/proc/" + std::to_string(pid);
    if (access(proc.c_str(), F_OK) != 0) { fprintf(stderr, "smash-estimate: no such process %d\n", pid); return 1; }
    std::vector<std::string> notes;

    // ── pick the method ────────────────────────────────────────────────
    int pm = open((proc + "/pagemap").c_str(), O_RDONLY | O_CLOEXEC);
    const bool can_clear = access((proc + "/clear_refs").c_str(), W_OK) == 0;
    bool pfn_ok = false;
    std::vector<Mapping> maps = readMaps(pid);
    if (maps.empty()) { fprintf(stderr, "smash-estimate: cannot read %s/maps (permission?)\n", proc.c_str()); return 1; }
    markThreadStacks(pid, maps);
    if (pm >= 0) {
        for (auto& m : maps) {
            scanPagemap(pm, m.start, std::min(m.end, m.start + 64 * g_page), [&](uintptr_t, uint64_t e) {
                if ((e & kPmPresent) && (e & kPmPfnMask)) pfn_ok = true;
            });
            if (pfn_ok) break;
        }
    }
    int idle_fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR | O_CLOEXEC);
    const bool idle_ok = idle_fd >= 0 && pfn_ok;
    const bool sd_ok = pm >= 0 && can_clear && softDirtyWorks();
    std::string method = o.method;
    if (method == "auto") method = idle_ok ? "idle" : sd_ok ? "soft-dirty" : can_clear ? "referenced" : "";
    auto why = [&]() {
        std::string w;
        if (idle_fd < 0) w += access("/sys/kernel/mm/page_idle/bitmap", F_OK) == 0
            ? " idle: /sys/kernel/mm/page_idle/bitmap needs root;"
            : " idle: kernel lacks CONFIG_IDLE_PAGE_TRACKING;";
        else if (!pfn_ok) w += " idle: pagemap PFNs hidden (needs CAP_SYS_ADMIN);";
        if (!can_clear) w += " soft-dirty/referenced: cannot write " + proc + "/clear_refs (needs same uid or root);";
        else if (!sd_ok) w += " soft-dirty: this kernel/arch never sets the soft-dirty bit;";
        return w;
    };
    if (method.empty() || (method == "idle" && !idle_ok) || (method == "soft-dirty" && !sd_ok) ||
        (method == "referenced" && !can_clear) ||
        (method != "idle" && method != "soft-dirty" && method != "referenced")) {
        fprintf(stderr, "smash-estimate: method '%s' unavailable:%s\n",
                o.method.c_str(), why().c_str());
        if (child && o.kill_child) kill(child, SIGKILL);
        return 1;
    }
    if (method != "idle") notes.push_back("idle-page tracking unavailable:" + why());
    if (method == "soft-dirty")
        notes.push_back("soft-dirty sees writes only; read-hot pages are excluded per mapping via smaps "
                        "Referenced (mapping granularity)");
    if (method == "referenced")
        notes.push_back("referenced method: idle bytes are per mapping, and compressibility is sampled from "
                        "all present pages of each mapping (not only idle ones)");

    // ── t0: snapshot present pages, reset tracking ─────────────────────
    // The window is split into --points segments; tracking is reset at the
    // start of each, so every page gets an access bit per segment and its idle
    // age at the END of the window is its run of trailing unaccessed segments
    // (pages appearing mid-window join at the next point). That is the age
    // smash's cold timeout tests at the moment the estimate stands for.
    const bool per_page = method != "referenced";
    const int P = o.points;
    readSmaps(pid, maps, -1);
    KPageFlags kpf;
    if (pfn_ok) kpf.fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
    size_t n_zero = 0, n_tail = 0;
    // Folio head of the previous tracked page, so consecutive tails of one
    // folio (consecutive PFNs) don't each walk back to the head.
    uint64_t prev_pfn = 0, prev_head = 0;
    auto track = [&](Mapping& m, uintptr_t va, uint64_t e, uint8_t born) {
        const uint64_t pfn = e & kPmPfnMask;
        uint64_t h = 0;
        if (method == "idle") {
            h = !kpf.tail(pfn) ? pfn
                : (prev_pfn && pfn == prev_pfn + 1) ? prev_head
                : smash::vm::folioHeadPfn(kpf.fd, pfn);
            n_tail += h != pfn;
            prev_pfn = pfn;
            prev_head = h;
        }
        m.pages.push_back(va);
        m.pfn.push_back(method == "idle" ? pfn : 0);
        m.head.push_back(h);
        m.hot.push_back(0);
        m.born.push_back(born);
        m.gone.push_back(0);
    };
    // A present entry for the shared zero page: not in RSS, no idle state.
    auto zeroPage = [&](uint64_t e) {
        const uint64_t pfn = e & kPmPfnMask;
        if (pfn && kpf.zero(pfn)) { ++n_zero; return true; }
        return false;
    };
    if (per_page) {
        for (auto& m : maps) {
            prev_pfn = prev_head = 0;
            scanPagemap(pm, m.start, m.end, [&](uintptr_t va, uint64_t e) {
                if ((e & kPmPresent) && !(e & kPmSwap) && !zeroPage(e)) track(m, va, e, 0);
            });
        }
    }
    auto resetTracking = [&]() {
        if (can_clear) writeStr(proc + "/clear_refs", "1");  // referenced bits (all methods)
        if (method == "soft-dirty") writeStr(proc + "/clear_refs", "4");
        if (method == "idle") {
            // Set the idle flag for every PFN we track: bitmap words are 64 PFNs.
            std::map<uint64_t, uint64_t> words;
            for (auto& m : maps)
                for (size_t j = 0; j < m.pages.size(); ++j)
                    if (!m.gone[j]) words[m.head[j] / 64] |= 1ULL << (m.head[j] % 64);
            for (auto& w : words)
                if (pwrite(idle_fd, &w.second, 8, static_cast<off_t>(w.first * 8)) != 8) {}
        }
    };
    resetTracking();
    const auto t0 = std::chrono::steady_clock::now();

    // ── sample points ──────────────────────────────────────────────────
    std::vector<double> point_s;
    for (int k = 1; k <= P; ++k) {
        const double at = o.window * k / P;
        point_s.push_back(at);
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(at)));
        if (kill(pid, 0) != 0) { fprintf(stderr, "smash-estimate: target exited during the window\n"); return 1; }
        refreshEnds(pid, maps);
        readSmaps(pid, maps, k - 1);
        if (!per_page) { if (k < P) resetTracking(); continue; }
        const uint32_t bit = 1u << (k - 1);
        for (auto& m : maps) {
            // Re-read pagemap over the mapping; walk tracked pages in order and
            // collect pages that appeared during this segment.
            size_t idx = 0;
            std::vector<std::pair<uintptr_t, uint64_t>> fresh;
            scanPagemap(pm, m.start, m.end, [&](uintptr_t va, uint64_t e) {
                while (idx < m.pages.size() && m.pages[idx] < va) ++idx;
                const bool present = (e & kPmPresent) && !(e & kPmSwap);
                if (idx >= m.pages.size() || m.pages[idx] != va) {
                    if (present && k < P && !zeroPage(e)) fresh.push_back({va, e});
                    return;
                }
                if (m.gone[idx]) return;
                if (!present) { m.gone[idx] = 1; return; }
                if (method == "soft-dirty") {
                    if (e & kPmSoftDirty) m.hot[idx] |= bit;
                } else if ((e & kPmPfnMask) != m.pfn[idx]) {
                    m.gone[idx] = 1;  // migrated / replaced: idle flag lost
                }
            });
            for (size_t j = 0; j < m.pages.size(); ++j)
                if (m.pages[j] >= m.end) m.gone[j] = 1;  // mapping shrank
            if (!fresh.empty()) {
                // Merge (both sorted by address).
                Mapping old;
                old.pages.swap(m.pages); old.pfn.swap(m.pfn); old.head.swap(m.head); old.hot.swap(m.hot);
                old.born.swap(m.born); old.gone.swap(m.gone);
                size_t i = 0, f = 0;
                while (i < old.pages.size() || f < fresh.size()) {
                    if (f >= fresh.size() || (i < old.pages.size() && old.pages[i] < fresh[f].first)) {
                        m.pages.push_back(old.pages[i]); m.pfn.push_back(old.pfn[i]); m.head.push_back(old.head[i]);
                        m.hot.push_back(old.hot[i]);
                        m.born.push_back(old.born[i]); m.gone.push_back(old.gone[i]);
                        ++i;
                    } else {
                        track(m, fresh[f].first, fresh[f].second, static_cast<uint8_t>(k));
                        ++f;
                    }
                }
            }
        }
        if (method == "idle") {
            // One bitmap read per 64-PFN word, then classify every tracked page.
            std::map<uint64_t, uint64_t> words;
            for (auto& m : maps)
                for (size_t j = 0; j < m.pages.size(); ++j)
                    if (!m.gone[j] && m.born[j] < k) words[m.head[j] / 64] = 0;
            for (auto& w : words)
                if (pread(idle_fd, &w.second, 8, static_cast<off_t>(w.first * 8)) != 8) w.second = 0;
            for (auto& m : maps)
                for (size_t j = 0; j < m.pages.size(); ++j) {
                    if (m.gone[j] || m.born[j] >= k) continue;
                    const uint64_t f = m.head[j];
                    if (!(words[f / 64] & (1ULL << (f % 64)))) m.hot[j] |= bit;
                }
        }
        if (k < P) resetTracking();
    }

    // Which idle ages the projection uses (segment length L = window / P):
    // idle >= the cold timeout is what smash compresses; idle >= twice it is
    // what a run whose buckets backed off after a refault compresses (smash
    // doubles the cold wait of a page, and of fresh pages from the same
    // allocation site, once one of them refaults). When the window reaches 2x
    // the cold timeout the projection uses the latter: the refaults that
    // trigger the backoff are mostly reads of compressed pages, which this
    // tool cannot attribute, and assuming none overestimated bench_sqlite by
    // ~50 MiB (agents/estimator.md).
    //
    // smash acts on whole compressor ticks (1 s): a page reaching the cold
    // timeout is read-checked or snapshotted the next tick and its frame is
    // dropped one deferred-reclaim tick later (T + 2 s); a backed-off page is
    // compressed at 2T and dropped at 2T + 1 s. The default window/points
    // (10 s / 3) end at T; --window 21 --points 7 (3 s segments) gives both
    // ages.
    const double age_cold = o.cold + 2, age_back = 2 * o.cold + 1;
    int k_cold = -1, k_back = -1;
    for (int k = 0; k < P; ++k) {
        if (k_cold < 0 && point_s[k] >= age_cold - 1e-6) k_cold = k;
        if (k_back < 0 && point_s[k] >= age_back - 1e-6) k_back = k;
    }
    if (k_cold < 0) {
        k_cold = P - 1;
        if (point_s[k_cold] < o.cold - 1e-6)
            notes.push_back("window shorter than the cold timeout: pages idle for the whole window are "
                            "projected as compressed");
    }
    const bool backoff = k_back >= 0;
    const int k_base = backoff ? k_back : k_cold;
    if (!backoff) {
        char nb[200];
        snprintf(nb, sizeof(nb), "window too short for refault backoff (use --window %.0f --points %.0f); "
                 "an upper bound when the program re-reads compressed pages", 2 * o.cold + 1,
                 (2 * o.cold + 1) / 3);
        notes.push_back(nb);
    }

    // ── per-class idle accounting ──────────────────────────────────────
    std::mt19937_64 rng(o.seed);
    struct ClassAgg {
        double present = 0;   // bytes present at t0 (per-page) or Anonymous (referenced)
        std::vector<double> idle_at;  // [m]: bytes idle for >= m+1 segments at the end
        double idle = 0;      // bytes idle at the projection's age (k_base)
        double anon_end = 0;  // Anonymous at the last point (the RSS the projection is relative to)
        std::vector<std::pair<Mapping*, size_t>> cand;  // idle pages (per-page) or present pages
        std::vector<Sample> s;
        int mappings = 0;
    } agg[kNumClass];
    for (auto& a : agg) a.idle_at.assign(static_cast<size_t>(P), 0);
    int n_polluted = 0;
    for (auto& m : maps) {
        auto& a = agg[m.cls];
        const double anon0 = static_cast<double>(m.anon_kb) * 1024;
        if (anon0 <= 0 && m.pages.empty()) continue;
        ++a.mappings;
        // anon_pt[q]: Anonymous at point q (0 = t0); ref_seg[k]: Referenced in segment k.
        std::vector<double> anon_pt(static_cast<size_t>(P) + 1, anon0), ref_seg(static_cast<size_t>(P) + 1, 0);
        for (int k = 1; k <= P; ++k) {
            if (k - 1 < static_cast<int>(m.anon_at.size())) anon_pt[k] = static_cast<double>(m.anon_at[k - 1]) * 1024;
            if (k - 1 < static_cast<int>(m.ref_kb.size())) ref_seg[k] = static_cast<double>(m.ref_kb[k - 1]) * 1024;
        }
        a.anon_end += anon_pt[P];
        // unref[mi]: bytes idle for the last mi+1 segments (since point
        // b = P-mi-1), mapping granularity. Pages faulted in after b are
        // Referenced but were not there at b and cannot be idle; charging them
        // against the older pages undercounted a growing brk heap by its growth
        // (bench_sqlite's heap grows ~7 MiB/s while serving: ~65 MiB over a
        // 10 s window). So each segment's Referenced bytes are reduced by the
        // growth since b (every new page may be among them), and segments are
        // combined with max(), a lower bound of the union of their referenced
        // sets (exact for a stable hot set).
        std::vector<double> unref(static_cast<size_t>(P), 0);
        for (int mi = 0; mi < P; ++mi) {
            const int b = P - mi - 1;
            double refmax = 0;
            for (int seg = b + 1; seg <= P; ++seg)
                refmax = std::max(refmax, ref_seg[seg] - std::max(0.0, anon_pt[seg] - anon_pt[b]));
            unref[mi] = std::max(0.0, std::min(anon_pt[b], anon_pt[P]) - refmax);
        }
        if (per_page) {
            for (size_t j = 0; j < m.pages.size(); ++j)
                if (m.born[j] == 0) a.present += static_cast<double>(g_page);
            std::vector<double> idle_k(static_cast<size_t>(P), 0);
            std::vector<std::pair<Mapping*, size_t>> mc;
            for (size_t j = 0; j < m.pages.size(); ++j) {
                if (m.gone[j]) continue;
                const int t = trailingIdle(m, j, P);
                for (int mi = 0; mi < t; ++mi) idle_k[mi] += static_cast<double>(g_page);
                if (t > k_base) mc.push_back({&m, j});
            }
            // A VMA that changed during a segment (brk growth, mremap, a
            // neighbouring mprotect) gets VM_SOFTDIRTY and pagemap then reports
            // EVERY page in it dirty. Written pages are a subset of referenced
            // ones, so more dirty bytes than referenced bytes means the bits
            // are polluted: fall back to Referenced for this mapping.
            bool polluted = false;
            if (method == "soft-dirty" && can_clear) {
                for (int k = 1; k <= P && !polluted; ++k) {
                    double live = 0, dirty = 0;
                    for (size_t j = 0; j < m.pages.size(); ++j) {
                        if (m.born[j] >= k || m.gone[j]) continue;
                        live += static_cast<double>(g_page);
                        if (m.hot[j] & (1u << (k - 1))) dirty += static_cast<double>(g_page);
                    }
                    polluted = dirty > ref_seg[k] + std::max(4.0 * static_cast<double>(g_page), 0.05 * live);
                }
                if (polluted) {
                    ++n_polluted;
                    idle_k.assign(idle_k.size(), 0);
                    mc.clear();
                    for (size_t j = 0; j < m.pages.size(); ++j) {
                        if (m.gone[j]) continue;
                        for (int mi = 0; mi < P - m.born[j]; ++mi) idle_k[mi] += static_cast<double>(g_page);
                        if (P - m.born[j] > k_base) mc.push_back({&m, j});
                    }
                }
            }
                double base = 0;
            for (int mi = 0; mi < P; ++mi) {
                // soft-dirty misses reads: cap by the mapping's unreferenced bytes.
                double v = (method == "soft-dirty" && can_clear) ? std::min(idle_k[mi], unref[mi]) : idle_k[mi];
                a.idle_at[mi] += v;
                if (mi == k_base) base = v;
            }
            // The cap removed read-hot pages we cannot identify individually:
            // thin this mapping's candidates to match, so the sample pool
            // stays proportional to the idle bytes it stands for.
            if (idle_k[k_base] > 0 && base < idle_k[k_base]) {
                std::shuffle(mc.begin(), mc.end(), rng);
                mc.resize(static_cast<size_t>(static_cast<double>(mc.size()) * base / idle_k[k_base] + 0.5));
            }
            a.cand.insert(a.cand.end(), mc.begin(), mc.end());
        } else {
            a.present += anon0;
            for (int mi = 0; mi < P; ++mi) a.idle_at[mi] += unref[mi];
            // candidates: present pages (for sampling); read pagemap if we can
            if (pm >= 0) {
                scanPagemap(pm, m.start, m.end, [&](uintptr_t va, uint64_t e) {
                    if (!(e & kPmPresent) || (e & kPmSwap) || zeroPage(e)) return;
                    m.pages.push_back(va);
                });
                // Thin to the mapping's unreferenced fraction (see above):
                // which pages are idle is unknown, but how many is.
                std::vector<std::pair<Mapping*, size_t>> mc;
                for (size_t j = 0; j < m.pages.size(); ++j) mc.push_back({&m, j});
                const double pres = static_cast<double>(m.pages.size() * g_page);
                if (pres > 0 && unref[k_base] < pres) {
                    std::shuffle(mc.begin(), mc.end(), rng);
                    mc.resize(static_cast<size_t>(static_cast<double>(mc.size()) * unref[k_base] / pres + 0.5));
                }
                a.cand.insert(a.cand.end(), mc.begin(), mc.end());
            }
        }
        a.idle = a.idle_at[k_base];
    }

    if (method == "idle" && kpf.fd < 0)
        notes.push_back("cannot read /proc/kpageflags: pages of transparent huge pages are reported hot");
    if (n_tail)
        notes.push_back(std::to_string(n_tail) + " page(s) in large folios (THP): tracked by the folio's head "
                        "page, so one access marks the whole folio hot");
    if (n_zero)
        notes.push_back(human(static_cast<double>(n_zero * g_page)) + " mapped to the shared zero page (read, "
                        "never written; not in RSS): excluded");
    if (n_polluted)
        notes.push_back(std::to_string(n_polluted) + " mapping(s) changed during the window (soft-dirty "
                        "reports every page of a changed VMA dirty); used smaps Referenced for them");
    // ── sample idle pages and compress ─────────────────────────────────
    double total_cand = 0;
    for (auto& a : agg) total_cand += static_cast<double>(a.cand.size());
    bool read_ok = true, any_read = false;
    std::string read_err;
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    std::vector<unsigned char> zbuf(ZSTD_compressBound(g_page));
    for (int c = 0; c < kNumClass; ++c) {
        auto& a = agg[c];
        if (a.cand.empty() || total_cand == 0) continue;
        size_t want = static_cast<size_t>(std::ceil(static_cast<double>(o.samples) * static_cast<double>(a.cand.size()) / total_cand));
        want = std::max<size_t>(want, std::min<size_t>(64, a.cand.size()));
        want = std::min(want, a.cand.size());
        // partial Fisher-Yates
        for (size_t j = 0; j < want; ++j) {
            std::uniform_int_distribution<size_t> d(j, a.cand.size() - 1);
            std::swap(a.cand[j], a.cand[d(rng)]);
        }
        constexpr size_t kBatch = 256;
        std::vector<unsigned char> buf(kBatch * g_page);
        for (size_t j = 0; j < want && read_ok; j += kBatch) {
            size_t n = std::min(kBatch, want - j);
            for (size_t q = 0; q < n; ++q) {
                auto [m, idx] = a.cand[j + q];
                struct iovec l{buf.data() + q * g_page, g_page};
                struct iovec r{reinterpret_cast<void*>(m->pages[idx]), g_page};
                ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
                if (got != static_cast<ssize_t>(g_page)) {
                    if (got < 0 && (errno == EPERM || errno == ESRCH || errno == ENOSYS)) {
                        read_ok = false;
                        read_err = strerror(errno);
                        break;
                    }
                    continue;  // unmapped since: skip
                }
                any_read = true;
                Sample s{c, 0, 0, false, false};
                costPage(buf.data() + q * g_page, cctx, zbuf, s.saving, s.blob, s.zero, s.rejected);
                a.s.push_back(s);
            }
        }
    }
    ZSTD_freeCCtx(cctx);
    if (!read_ok)
        notes.push_back("cannot read target memory (" + read_err + "): run as root, as the target's parent "
                        "(smash-estimate -- <command>), or set kernel.yama.ptrace_scope=0; idle bytes are "
                        "reported but no savings");

    // ── projection ─────────────────────────────────────────────────────
    // Allocator layout (glibc chunk walk) for the slab-managed classes.
    struct Lay { double glibc = 0, smash = 0, factor = 1; size_t chunks = 0; int walked = 0, failed = 0; } lay[kNumClass];
    if (read_ok && any_read) {
        for (auto& m : maps) {
            if (m.cls != kHeap && m.cls != kArena) continue;
            LayoutWalk w;
            const double anon = m.anon_at.empty() ? static_cast<double>(m.anon_kb) * 1024
                                                  : static_cast<double>(m.anon_at.back()) * 1024;
            if (walkGlibcHeap(pid, m, w) && w.covered >= 0.5 * anon) {
                lay[m.cls].glibc += w.glibc; lay[m.cls].smash += w.smash; lay[m.cls].chunks += w.chunks;
                ++lay[m.cls].walked;
            } else if (anon >= 1048576) {
                ++lay[m.cls].failed;
            }
        }
        for (auto& l : lay) if (l.glibc > 0) l.factor = l.smash / l.glibc;
        if (lay[kHeap].failed + lay[kArena].failed)
            notes.push_back(std::to_string(lay[kHeap].failed + lay[kArena].failed) +
                            " heap mapping(s) could not be walked as glibc chunks (another allocator, or the "
                            "chain changed mid-walk): no layout adjustment for them");
    }

    const double rss = static_cast<double>(statusKb(pid, "VmRSS:")) * 1024;
    struct Out {
        double mean = 0, sd = 0, saving = 0, saving_nb = 0, half = 0, ratio = 0, layout = 0, layout_nb = 0,
               comp_pages = 0, comp_pages_nb = 0;
        size_t n = 0, zero = 0, rej = 0;
    } out[kNumClass];
    double tot_saving = 0, tot_saving_nb = 0, tot_var = 0, tot_idle = 0, tot_present = 0;
    double tot_layout = 0, tot_layout_nb = 0, comp_pages = 0, comp_pages_nb = 0;
    double meta_pages = kMetaFixed;  // metadata bytes that do not depend on the compressed set
    std::vector<double> tot_idle_at(static_cast<size_t>(o.points), 0);
    for (int c = 0; c < kNumClass; ++c) {
        auto& a = agg[c];
        auto& r = out[c];
        tot_idle += a.idle;
        tot_present += a.present;
        for (int k = 0; k < o.points; ++k) tot_idle_at[k] += a.idle_at[k];
        if (c != kStack)
            meta_pages += a.anon_end / static_cast<double>(g_page) *
                          (c == kAnon ? kMetaPerLargePage : kMetaPerSlabPage);
        r.n = a.s.size();
        if (!r.n) continue;
        double sum = 0, sq = 0, blob = 0;
        for (auto& s : a.s) { sum += s.saving; sq += s.saving * s.saving; blob += s.blob; r.zero += s.zero; r.rej += s.rejected; }
        r.mean = sum / static_cast<double>(r.n);
        r.sd = r.n > 1 ? std::sqrt(std::max(0.0, (sq - sum * sum / static_cast<double>(r.n)) / static_cast<double>(r.n - 1))) : 0;
        r.ratio = blob > 0 ? static_cast<double>(g_page) * static_cast<double>(r.n) / blob : 0;
        const double kept = 1.0 - static_cast<double>(r.rej) / static_cast<double>(r.n);
        const double pages = a.idle_at[k_base] / static_cast<double>(g_page);
        const double pages_nb = a.idle_at[k_cold] / static_cast<double>(g_page);
        const double N = static_cast<double>(a.cand.size());
        const double fpc = N > 1 ? std::sqrt(std::max(0.0, (N - static_cast<double>(r.n)) / (N - 1))) : 0;
        r.saving = pages * r.mean;
        r.saving_nb = pages_nb * r.mean;
        if (c != kStack) {
            r.comp_pages = pages * kept;
            r.comp_pages_nb = pages_nb * kept;
            // smash stores these objects in factor x the bytes; idle pages
            // compress that padding away, resident (hot) pages keep it.
            r.layout = std::max(0.0, a.anon_end - a.idle_at[k_base]) * (lay[c].factor - 1);
            r.layout_nb = std::max(0.0, a.anon_end - a.idle_at[k_cold]) * (lay[c].factor - 1);
        }
        const double se = pages * r.sd / std::sqrt(static_cast<double>(r.n)) * fpc;
        r.half = 1.96 * se;
        tot_saving += r.saving;
        tot_saving_nb += r.saving_nb;
        tot_layout += r.layout;
        tot_layout_nb += r.layout_nb;
        comp_pages += r.comp_pages;
        comp_pages_nb += r.comp_pages_nb;
        tot_var += se * se;
    }
    double runtime = kRuntimeFixed;
    {
        std::string mtxt;
        readFile(proc + "/maps", mtxt);
        if (mtxt.find("libstdc++.so") == std::string::npos) runtime += kRuntimeLibstdcxx;
        if (mtxt.find("/libm.so") == std::string::npos) runtime += kRuntimeLibm;
    }
    const double meta = meta_pages + comp_pages * kMetaPerCompressedPage;
    const double meta_nb = meta_pages + comp_pages_nb * kMetaPerCompressedPage;
    const double net = tot_saving - tot_layout - meta - runtime;
    const double net_nb = tot_saving_nb - tot_layout_nb - meta_nb - runtime;
    const double tot_half = 1.96 * std::sqrt(tot_var);
    const bool have_savings = any_read;
    auto pct = [&](double v) { return rss > 0 ? 100.0 * v / rss : 0.0; };

    if (o.json) {
        printf("{\"pid\": %d, \"comm\": \"%s\", \"method\": \"%s\", \"window_s\": %.1f, \"page_size\": %zu,"
               " \"cold_timeout_s\": %.1f,\n",
               pid, comm(pid).c_str(), method.c_str(), o.window, g_page, o.cold);
        printf(" \"rss_bytes\": %.0f, \"anon_present_bytes\": %.0f, \"idle_bytes\": %.0f,\n", rss, tot_present, tot_idle);
        printf(" \"idle_age\": [");
        for (int k = 0; k < o.points; ++k)
            printf("%s{\"idle_for_s\": %.1f, \"bytes\": %.0f}", k ? ", " : "", point_s[k], tot_idle_at[k]);
        printf("],\n \"savings_available\": %s, \"projected_saving_bytes\": %.0f, \"ci95_bytes\": %.0f,"
               " \"projected_reduction_pct\": %.1f, \"ci95_pct\": %.1f,\n",
               have_savings ? "true" : "false", net, tot_half, pct(net), pct(tot_half));
        printf(" \"backoff_modeled\": %s, \"projected_saving_no_backoff_bytes\": %.0f,"
               " \"compression_saving_bytes\": %.0f, \"compression_saving_no_backoff_bytes\": %.0f,\n"
               " \"smash_metadata_bytes\": %.0f, \"smash_runtime_bytes\": %.0f, \"layout_bytes\": %.0f,\n",
               backoff ? "true" : "false", net_nb, tot_saving, tot_saving_nb, meta, runtime, tot_layout);
        printf(" \"classes\": [\n");
        bool first = true;
        for (int c = 0; c < kNumClass; ++c) {
            auto& a = agg[c];
            auto& r = out[c];
            if (!a.mappings) continue;
            printf("%s  {\"class\": \"%s\", \"mappings\": %d, \"present_bytes\": %.0f, \"idle_bytes\": %.0f,"
                   " \"sampled_pages\": %zu, \"zero_pages\": %zu, \"rejected_pages\": %zu, \"ratio\": %.2f,"
                   " \"projected_saving_bytes\": %.0f, \"ci95_bytes\": %.0f, \"layout_factor\": %.3f,"
                   " \"layout_chunks\": %zu, \"layout_bytes\": %.0f, \"smash\": \"%s\"}",
                   first ? "" : ",\n", kClassName[c], a.mappings, a.present, a.idle_at[k_base], r.n, r.zero,
                   r.rej, r.ratio, r.saving, r.half, lay[c].factor, lay[c].chunks, r.layout, kClassNote[c]);
            first = false;
        }
        printf("\n ],\n \"notes\": [");
        for (size_t k = 0; k < notes.size(); ++k) {
            std::string esc;
            for (char ch : notes[k]) { if (ch == '"' || ch == '\\') esc += '\\'; esc += ch; }
            printf("%s\"%s\"", k ? ", " : "", esc.c_str());
        }
        printf("]}\n");
    } else {
        printf("smash-estimate: pid %d (%s), method %s, idle window %.0f s, cold timeout %.0f s\n", pid,
               comm(pid).c_str(), method.c_str(), o.window, o.cold);
        printf("RSS %s; anonymous present at start %s\n", human(rss).c_str(), human(tot_present).c_str());
        printf("idle for:");
        for (int k = 0; k < o.points; ++k)
            printf("  >=%.0fs %s (%.0f%%)", point_s[k], human(tot_idle_at[k]).c_str(),
                   tot_present > 0 ? 100.0 * tot_idle_at[k] / tot_present : 0.0);
        printf("\n\n%-13s %10s %10s %8s %6s %7s %12s  %s\n", "class", "present", "idle", "sampled", "ratio",
               "layout", "saving", "managed by smash?");
        for (int c = 0; c < kNumClass; ++c) {
            auto& a = agg[c];
            auto& r = out[c];
            if (!a.mappings) continue;
            printf("%-13s %10s %10s %8zu %5.1fx %6.3fx %12s  %s\n", kClassName[c], human(a.present).c_str(),
                   human(a.idle_at[k_base]).c_str(), r.n, r.ratio, lay[c].factor,
                   have_savings ? human(r.saving).c_str() : "n/a", kClassNote[c]);
        }
        if (have_savings) {
            printf("\ncompressed-page saving %s, minus smash's metadata %s, runtime %s and allocator layout %s\n",
                   human(tot_saving).c_str(), human(meta).c_str(), human(runtime).c_str(),
                   human(tot_layout).c_str());
            printf("Projected RSS reduction: %s (%.1f%% of RSS), 95%% CI +/- %s (%.1f%%)%s\n",
                   human(net).c_str(), pct(net), human(tot_half).c_str(), pct(tot_half),
                   backoff ? ", with refault backoff" : "");
            if (backoff)
                printf("  without refault backoff (no compressed page is ever re-read): %s (%.1f%%)\n",
                       human(net_nb).c_str(), pct(net_nb));
        } else {
            printf("\nProjected RSS reduction: unavailable (page contents unreadable)\n");
        }
        printf("(zstd-1, smash's 0.75 ratio gate and 16-byte blob granularity; assumes every idle page in a\n"
               " class is smash-managed -- see the class notes, and agents/estimator.md for what is not modeled)\n");
        for (auto& n : notes) printf("note: %s\n", n.c_str());
    }
    fflush(stdout);
    if (child) {
        if (o.kill_child) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        else { int st; waitpid(child, &st, 0); }
    }
    return have_savings ? 0 : 3;
}
