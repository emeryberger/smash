// Regression tests for SMASH_TRACK_EXTERNAL=1 under the mmap/madvise/mprotect
// patterns that bundled allocators (jemalloc, mimalloc, tcmalloc), runtime
// arenas (CPython obmalloc) and coroutine libraries use on memory smash has
// registered and then compressed.
//
// Run under LD_PRELOAD / DYLD_INSERT_LIBRARIES with SMASH_TRACK_EXTERNAL=1,
// SMASH_COLD_TIMEOUT_SEC=1, SMASH_DEFER_PHASES_MS=0, in BOTH SMASH_LARGE_ONLY=0
// and =1 (external tracking is independent of the malloc mode).
//
// Every subtest first drives its region to "compressed" — it waits until the
// kernel reports the region's pages inaccessible (smash maps a compressed page
// PROT_NONE) — and only then applies the operation under test, so each case
// exercises a page smash actually holds a blob for:
//
//   dontneed   madvise(MADV_DONTNEED) on compressed pages must read back zero
//              (Linux semantics jemalloc relies on to skip zeroing) — the old
//              blob must not be decompressed over the purged page.
//   free       madvise(MADV_FREE): each page reads either old data or zero.
//   unmap      munmap of the middle of a compressed range; the rest survives.
//   fixed      mmap(MAP_FIXED) anon over a compressed range reads zero.
//   fixedfile  mmap(MAP_FIXED|MAP_SHARED) of a file over a compressed range
//              reads the file, and msync works (the file page is not tracked).
//   mremap     moving a compressed range keeps its contents (Linux).
//   mprotect   mprotect(PROT_READ) of a compressed range reads the old data.
//   guard      a PROT_NONE guard page inside a tracked range stays PROT_NONE.
//   fork       a child reads compressed external pages correctly.
//   shared     MAP_SHARED|MAP_ANON pages are never compressed (a child's
//              write must stay visible to the parent).
//   sbrk       program-break growth (gperftools tcmalloc) is tracked (Linux).
//   churn      after mapping/unmapping more pages than the external slot
//              budget (CPython arena churn), a fresh mapping is still tracked
//              and compressed.
//
// Fails (exit 1) on the first mismatch, printing which case and where.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {

size_t g_page = 4096;

void fill(void* base, size_t bytes, unsigned seed) {
    auto* b = static_cast<unsigned char*>(base);
    for (size_t i = 0; i < bytes / g_page; ++i)
        std::memset(b + i * g_page, static_cast<int>((i + seed) & 0xff) | 1, g_page);
}

// Returns the first mismatching page index, or SIZE_MAX if all match.
size_t check(const void* base, size_t bytes, unsigned seed) {
    auto* b = static_cast<const unsigned char*>(base);
    for (size_t i = 0; i < bytes / g_page; ++i) {
        unsigned char want = static_cast<unsigned char>(((i + seed) & 0xff) | 1);
        for (size_t j = 0; j < g_page; ++j)
            if (b[i * g_page + j] != want) return i;
    }
    return SIZE_MAX;
}

bool allZero(const void* base, size_t bytes) {
    auto* b = static_cast<const unsigned char*>(base);
    for (size_t i = 0; i < bytes; ++i)
        if (b[i]) return false;
    return true;
}

// Number of pages of [base, base+len) that are currently inaccessible
// (PROT_NONE) — smash's marker for a compressed page.
size_t inaccessiblePages(const void* base, size_t len) {
    uintptr_t lo = reinterpret_cast<uintptr_t>(base), hi = lo + len;
    size_t n = 0;
#ifdef __linux__
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    static char buf[1 << 20];
    size_t got = 0;
    for (;;) {
        ssize_t r = read(fd, buf + got, sizeof(buf) - 1 - got);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
        if (got >= sizeof(buf) - 1) break;
    }
    close(fd);
    buf[got] = 0;
    for (char* line = buf; line && *line;) {
        char* nl = std::strchr(line, '\n');
        unsigned long s = 0, e = 0;
        char perms[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 &&
            perms[0] == '-' && perms[1] == '-' && perms[2] == '-') {
            uintptr_t a = s > lo ? s : lo, z = e < hi ? e : hi;
            if (a < z) n += (z - a) / g_page;
        }
        line = nl ? nl + 1 : nullptr;
    }
#elif defined(__APPLE__)
    mach_vm_address_t addr = lo;
    while (addr < hi) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        mach_vm_address_t a = addr;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) != KERN_SUCCESS)
            break;
        if (a >= hi) break;
        uintptr_t s = a > lo ? a : lo, z = a + size < hi ? a + size : hi;
        if (info.protection == VM_PROT_NONE && s < z) n += (z - s) / g_page;
        addr = a + size;
    }
#endif
    return n;
}

double nowSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Wait until at least half of each region's pages are compressed.
struct Region { void* p; size_t len; const char* tag; };
bool waitCompressed(Region* rs, int n, double timeout_s = 30) {
    double t0 = nowSec();
    for (;;) {
        int ok = 0;
        for (int i = 0; i < n; ++i)
            if (inaccessiblePages(rs[i].p, rs[i].len) * 2 >= rs[i].len / g_page) ++ok;
        if (ok == n) return true;
        if (nowSec() - t0 > timeout_s) {
            for (int i = 0; i < n; ++i)
                fprintf(stderr, "  [%s] inaccessible=%zu of %zu pages\n", rs[i].tag,
                        inaccessiblePages(rs[i].p, rs[i].len), rs[i].len / g_page);
            return false;
        }
        usleep(100 * 1000);
    }
}

void* mapAnon(size_t len) {
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    return p;
}

// EXTPAT_KEEPGOING=1 (diagnostics only): report every failing case instead of
// stopping at the first, so one run shows the whole failure surface.
bool g_keepgoing = false;
int g_failures = 0;
#define FAIL(...) do { fprintf(stderr, "FAIL " __VA_ARGS__); fprintf(stderr, "\n"); \
                       ++g_failures; if (!g_keepgoing) return false; } while (0)

constexpr size_t kLen = 2 * 1024 * 1024;

bool testDontneedFreeUnmapFixedMprotect() {
    Region rs[] = {
        {mapAnon(kLen), kLen, "dontneed"}, {mapAnon(kLen), kLen, "free"},
        {mapAnon(kLen), kLen, "unmap"},    {mapAnon(kLen), kLen, "fixed"},
        {mapAnon(kLen), kLen, "fixedfile"},{mapAnon(kLen), kLen, "mprotect"},
#ifdef __linux__
        {mapAnon(kLen), kLen, "mremap"},
#endif
    };
    constexpr int n = sizeof(rs) / sizeof(rs[0]);
    for (int i = 0; i < n; ++i) fill(rs[i].p, rs[i].len, i);
    if (!waitCompressed(rs, n)) FAIL("[setup]: regions never compressed — is SMASH_TRACK_EXTERNAL=1 set?");

    // dontneed
    if (madvise(rs[0].p, kLen, MADV_DONTNEED) != 0) FAIL("[dontneed]: madvise errno=%d", errno);
#ifdef __linux__
    if (!allZero(rs[0].p, kLen)) FAIL("[dontneed]: purged compressed pages read back non-zero (stale blob)");
#endif
    fill(rs[0].p, kLen, 100);

    // free
#ifdef MADV_FREE
    if (madvise(rs[1].p, kLen, MADV_FREE) == 0) {
        auto* b = static_cast<unsigned char*>(rs[1].p);
        for (size_t i = 0; i < kLen / g_page; ++i) {
            unsigned char want = static_cast<unsigned char>(((i + 1) & 0xff) | 1);
            unsigned char got = b[i * g_page];
            if (got != 0 && got != want) FAIL("[free]: page %zu byte 0x%02x", i, got);
        }
    }
#endif
    fill(rs[1].p, kLen, 101);

    // unmap middle
    char* u = static_cast<char*>(rs[2].p);
    if (munmap(u + kLen / 4, kLen / 2) != 0) FAIL("[unmap]: errno=%d", errno);
    if (check(u, kLen / 4, 2) != SIZE_MAX) FAIL("[unmap]: head corrupted");
    if (check(u + 3 * kLen / 4, kLen / 4, 2 + (3 * kLen / 4) / g_page) != SIZE_MAX)
        FAIL("[unmap]: tail corrupted");
    munmap(u, kLen / 4);
    munmap(u + 3 * kLen / 4, kLen / 4);

    // MAP_FIXED anon over compressed range
    void* f = mmap(rs[3].p, kLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (f != rs[3].p) FAIL("[fixed]: mmap errno=%d", errno);
    if (!allZero(f, kLen)) FAIL("[fixed]: MAP_FIXED anon over compressed range not zero");
    fill(f, kLen, 103);

    // MAP_FIXED file over compressed range
    char path[] = "/tmp/smash-extpat-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) FAIL("[fixedfile]: mkstemp");
    unlink(path);
    if (ftruncate(fd, kLen) != 0) FAIL("[fixedfile]: ftruncate");
    {
        static char blk[65536];
        std::memset(blk, 0x5A, sizeof(blk));
        for (size_t off = 0; off < kLen; off += sizeof(blk))
            if (pwrite(fd, blk, sizeof(blk), off) != (ssize_t)sizeof(blk)) FAIL("[fixedfile]: pwrite");
    }
    void* ff = mmap(rs[4].p, kLen, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (ff != rs[4].p) FAIL("[fixedfile]: mmap errno=%d", errno);
    for (size_t i = 0; i < kLen; i += g_page)
        if (static_cast<unsigned char*>(ff)[i] != 0x5A) FAIL("[fixedfile]: offset %zu not file data", i);

    // mprotect(PROT_READ) of a compressed range
    if (mprotect(rs[5].p, kLen, PROT_READ) != 0) FAIL("[mprotect]: errno=%d", errno);
    if (size_t bad = check(rs[5].p, kLen, 5); bad != SIZE_MAX)
        FAIL("[mprotect]: page %zu wrong after mprotect(PROT_READ)", bad);
    if (mprotect(rs[5].p, kLen, PROT_READ | PROT_WRITE) != 0) FAIL("[mprotect]: RW errno=%d", errno);
    fill(rs[5].p, kLen, 105);

#ifdef __linux__
    // mremap: move (grow forces a move if the next range is occupied)
    void* m = mremap(rs[6].p, kLen, 2 * kLen, MREMAP_MAYMOVE);
    if (m == MAP_FAILED) FAIL("[mremap]: errno=%d", errno);
    if (size_t bad = check(m, kLen, 6); bad != SIZE_MAX)
        FAIL("[mremap]: page %zu wrong after mremap (moved=%d)", bad, m != rs[6].p);
    rs[6].p = m;
    fill(m, 2 * kLen, 106);
    rs[6].len = 2 * kLen;
#endif

    // Second round: the re-written regions must compress and round-trip.
    Region again[] = {
        {rs[0].p, kLen, "dontneed2"}, {rs[1].p, kLen, "free2"}, {rs[3].p, kLen, "fixed2"},
        {rs[5].p, kLen, "mprotect2"},
#ifdef __linux__
        {rs[6].p, rs[6].len, "mremap2"},
#endif
    };
    constexpr int n2 = sizeof(again) / sizeof(again[0]);
    unsigned seeds[] = {100, 101, 103, 105, 106};
    // Whether these re-compress is a coverage question (a page that faulted
    // back is treated as hot); correctness is the hard requirement.
    (void)waitCompressed(again, n2, 6);
    for (int i = 0; i < n2; ++i)
        if (size_t bad = check(again[i].p, again[i].len, seeds[i]); bad != SIZE_MAX)
            FAIL("[%s]: page %zu wrong on second round", again[i].tag, bad);

    if (msync(ff, kLen, MS_SYNC) != 0) FAIL("[fixedfile]: msync errno=%d", errno);
    munmap(ff, kLen);
    close(fd);
    for (int i = 0; i < n2; ++i) munmap(again[i].p, again[i].len);
    fprintf(stderr, "external_patterns_test: dontneed/free/unmap/fixed/fixedfile/mprotect%s PASSED\n",
#ifdef __linux__
            "/mremap"
#else
            ""
#endif
    );
    return true;
}

bool testGuard() {
    char* p = static_cast<char*>(mapAnon(kLen));
    fill(p, kLen, 7);
    char* guard = p + kLen / 2;
    if (mprotect(guard, g_page, PROT_NONE) != 0) FAIL("[guard]: mprotect errno=%d", errno);
    Region r{p, kLen, "guard"};
    if (!waitCompressed(&r, 1)) FAIL("[guard]: never compressed");
    // Touch everything except the guard (faults decompress), then give the
    // compressor several more ticks to (wrongly) re-protect the guard.
    if (check(p, kLen / 2, 7) != SIZE_MAX) FAIL("[guard]: lower half wrong");
    sleep(2);
    if (inaccessiblePages(guard, g_page) != 1) FAIL("[guard]: guard page lost its PROT_NONE");
    if (mprotect(guard, g_page, PROT_READ | PROT_WRITE) != 0) FAIL("[guard]: unprotect");
    if (check(guard, g_page, 7 + kLen / 2 / g_page) != SIZE_MAX) FAIL("[guard]: guard contents changed");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: guard PASSED\n");
    return true;
}

bool testFork() {
    void* p = mapAnon(kLen);
    fill(p, kLen, 8);
    Region r{p, kLen, "fork"};
    if (!waitCompressed(&r, 1)) FAIL("[fork]: never compressed");
    pid_t pid = fork();
    if (pid == 0) _exit(check(p, kLen, 8) == SIZE_MAX ? 0 : 3);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) FAIL("[fork]: child status 0x%x", st);
    if (check(p, kLen, 8) != SIZE_MAX) FAIL("[fork]: parent corrupted");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: fork PASSED\n");
    return true;
}

#ifdef __linux__
// gperftools tcmalloc grows its heap with __sbrk: break growth must be
// tracked (and compress), and a shrink must untrack it.
bool testSbrk() {
    char* cur = static_cast<char*>(sbrk(0));
    // Start on a page boundary so the region is whole pages.
    uintptr_t pad = (g_page - reinterpret_cast<uintptr_t>(cur) % g_page) % g_page;
    if (pad && sbrk(static_cast<intptr_t>(pad)) == reinterpret_cast<void*>(-1)) FAIL("[sbrk]: pad");
    char* p = static_cast<char*>(sbrk(static_cast<intptr_t>(kLen)));
    if (p == reinterpret_cast<char*>(-1)) FAIL("[sbrk]: sbrk errno=%d", errno);
    fill(p, kLen, 12);
    Region r{p, kLen, "sbrk"};
    if (!waitCompressed(&r, 1)) FAIL("[sbrk]: break growth never compressed");
    if (size_t bad = check(p, kLen, 12); bad != SIZE_MAX) FAIL("[sbrk]: page %zu wrong", bad);
    if (sbrk(-static_cast<intptr_t>(kLen)) == reinterpret_cast<void*>(-1)) FAIL("[sbrk]: shrink");
    fprintf(stderr, "external_patterns_test: sbrk PASSED\n");
    return true;
}
#endif

// MAP_SHARED|MAP_ANON memory is shared with forked children (PostgreSQL
// shared buffers, multiprocessing). It must never be compressed: a blob in
// the parent is stale as soon as the child writes, and decompressing it would
// overwrite the child's data.
bool testSharedAnon() {
    void* p = mmap(nullptr, kLen, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) FAIL("[shared]: mmap errno=%d", errno);
    fill(p, kLen, 10);
    sleep(4);  // several cold timeouts: a tracked page would be compressed
    pid_t pid = fork();
    if (pid == 0) { fill(p, kLen, 11); _exit(0); }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) FAIL("[shared]: child status 0x%x", st);
    if (size_t bad = check(p, kLen, 11); bad != SIZE_MAX)
        FAIL("[shared]: page %zu does not show the child's write (stale blob restored)", bad);
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: shared PASSED\n");
    return true;
}

// CPython obmalloc / FFmpeg pattern: map and unmap many short-lived arenas.
// Total pages mapped over the run exceed the external slot budget; a mapping
// made afterwards must still be tracked (slots are recycled on munmap).
bool testChurn() {
    constexpr size_t kArena = 1024 * 1024;
    const size_t total = (size_t{2} << 30);  // 2 GiB of cumulative mmap
    for (size_t done = 0; done < total; done += kArena) {
        void* a = mapAnon(kArena);
        static_cast<char*>(a)[0] = 1;
        munmap(a, kArena);
    }
    void* p = mapAnon(kLen);
    fill(p, kLen, 9);
    Region r{p, kLen, "churn"};
    if (!waitCompressed(&r, 1)) FAIL("[churn]: post-churn mapping never compressed (slot budget leaked)");
    if (check(p, kLen, 9) != SIZE_MAX) FAIL("[churn]: corrupted");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: churn PASSED\n");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) g_page = static_cast<size_t>(ps);
    const char* kg = getenv("EXTPAT_KEEPGOING");
    g_keepgoing = kg && kg[0] == '1';
    const char* only = argc > 1 ? argv[1] : nullptr;
    auto want = [&](const char* name) { return !only || std::strcmp(only, name) == 0; };
    if (want("ops") && !testDontneedFreeUnmapFixedMprotect()) return 1;
    if (want("guard") && !testGuard()) return 1;
    if (want("fork") && !testFork()) return 1;
    if (want("shared") && !testSharedAnon()) return 1;
#ifdef __linux__
    if (want("sbrk") && !testSbrk()) return 1;
#endif
    if (want("churn") && !testChurn()) return 1;
    if (g_failures) return 1;
    fprintf(stderr, "external_patterns_test: ALL PASSED\n");
    return 0;
}
