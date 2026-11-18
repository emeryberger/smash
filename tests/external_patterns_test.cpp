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
//   sigmask    a thread with every signal blocked (jemalloc's background
//              thread) reads compressed pages without being killed.
//   ucstack    a suspended makecontext coroutine stack (plain RW mmap) gets
//              compressed; resuming it must not kill the process (Linux).
//   mlock      mlock() of a compressed range restores it; it is never
//              compressed again.
//   path       open / rename / stat / fopen / unlink with path strings on compressed
//              pages.
//   noreserve  untouched pages of a MAP_NORESERVE reservation are never made
//              inaccessible; the touched part compresses.
//   thp        a MADV_HUGEPAGE region compresses, round-trips, and a partial
//              DONTNEED of it reads back zero.
//   vmacap     near vm.max_map_count, aging external pages never lose data
//              (Linux; the classic PROT_NONE path splits VMAs).
//   churn      after mapping/unmapping more pages than the external slot
//              budget (CPython arena churn), a fresh mapping is still tracked
//              and compressed.
//
// Fails (exit 1) on the first mismatch, printing which case and where.

#include <atomic>
#include <vector>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/userfaultfd.h>
#include <sys/syscall.h>
#include <ucontext.h>
#endif

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

// Pages of [base, base+len) smash holds compressed right now: PROT_NONE
// (classic path) or, on Linux, not resident: in uffd mode smash reclaims a
// compressed page with UFFDIO_MOVE, leaving it MISSING with its protection
// untouched. Only meaningful for regions the test filled.
size_t compressedPages(const void* base, size_t len) {
#ifdef __linux__
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base);
    const size_t np = len / g_page;
    std::vector<uint64_t> e(np);
    static int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || pread(fd, e.data(), np * 8, static_cast<off_t>(lo / g_page * 8)) !=
                      static_cast<ssize_t>(np * 8))
        return inaccessiblePages(base, len);
    size_t n = 0;
    for (size_t k = 0; k < np; ++k) {
        if (!(e[k] & (3ULL << 62))) { ++n; continue; }  // neither present nor swapped
        // Resident but PROT_NONE: classic path, madvise still deferred.
        size_t run = 1;
        while (k + run < np && (e[k + run] & (3ULL << 62))) ++run;
        n += inaccessiblePages(reinterpret_cast<void*>(lo + k * g_page), run * g_page);
        k += run - 1;
    }
    return n;
#else
    return inaccessiblePages(base, len);
#endif
}

#ifdef __linux__
// smash reclaims through userfaultfd in this process (Linux 6.8+ default).
bool uffdActive() {
    char path[64], link[128];
    for (int fd = 0; fd < 1024; ++fd) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, link, sizeof(link) - 1);
        if (n <= 0) continue;
        link[n] = 0;
        if (std::strstr(link, "userfaultfd")) return true;
    }
    return false;
}
#endif

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
            if (compressedPages(rs[i].p, rs[i].len) * 2 >= rs[i].len / g_page) ++ok;
        if (ok == n) return true;
        if (nowSec() - t0 > timeout_s) {
            for (int i = 0; i < n; ++i)
                fprintf(stderr, "  [%s] inaccessible=%zu of %zu pages\n", rs[i].tag,
                        compressedPages(rs[i].p, rs[i].len), rs[i].len / g_page);
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

// jemalloc's background purge thread runs with every signal blocked. A
// thread that blocks SIGSEGV must still be able to touch compressed memory:
// smash keeps SIGSEGV/SIGBUS out of every sigmask (otherwise the kernel kills
// the process on the first decompression fault).
struct MaskedArg { void* p; size_t len; bool ok; };
void* maskedReader(void* v) {
    auto* a = static_cast<MaskedArg*>(v);
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, nullptr);
    a->ok = check(a->p, a->len, 13) == SIZE_MAX;
    return nullptr;
}
bool testSigmask() {
    void* p = mapAnon(kLen);
    fill(p, kLen, 13);
    Region r{p, kLen, "sigmask"};
    if (!waitCompressed(&r, 1)) FAIL("[sigmask]: never compressed");
    MaskedArg a{p, kLen, false};
    pthread_t t;
    if (pthread_create(&t, nullptr, maskedReader, &a) != 0) FAIL("[sigmask]: pthread_create");
    pthread_join(t, nullptr);
    if (!a.ok) FAIL("[sigmask]: data wrong when read from a fully-masked thread");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: sigmask PASSED\n");
    return true;
}

#ifdef __linux__
// Coroutine / fiber stacks are often plain mmap(PROT_READ|PROT_WRITE,
// MAP_PRIVATE|MAP_ANON) without MAP_STACK (boost.context, folly, hand-rolled
// makecontext). While a coroutine is suspended its stack is cold and gets
// compressed; on resume, the first touch of a compressed stack page faults
// with the stack pointer inside the inaccessible range, so the kernel has
// nowhere to push the signal frame unless the handler runs on an alternate
// stack.
ucontext_t g_main_uc, g_co_uc;
volatile int g_co_sum = 0;
[[gnu::noinline]] int deepTouch(int depth) {
    volatile char frame[2048];
    std::memset(const_cast<char*>(frame), depth & 0x7f, sizeof(frame));
    int r = depth ? deepTouch(depth - 1) : 0;
    return r + frame[depth % sizeof(frame)];
}
void coroutineBody() {
    g_co_sum = deepTouch(400);   // ~800 KiB of stack
    swapcontext(&g_co_uc, &g_main_uc);  // suspend: stack goes cold
    g_co_sum += deepTouch(400);  // resume and re-touch every page
    swapcontext(&g_co_uc, &g_main_uc);
}
bool testCoroutineStack() {
    constexpr size_t kStack = 2 * 1024 * 1024;
    void* stk = mapAnon(kStack);
    getcontext(&g_co_uc);
    g_co_uc.uc_stack.ss_sp = stk;
    g_co_uc.uc_stack.ss_size = kStack;
    g_co_uc.uc_link = &g_main_uc;
    makecontext(&g_co_uc, coroutineBody, 0);
    swapcontext(&g_main_uc, &g_co_uc);
    int first = g_co_sum;
    // Only the used part of the stack is resident; wait for it to compress.
    Region r{static_cast<char*>(stk) + kStack - 1024 * 1024, 1024 * 1024, "ucstack"};
    if (!waitCompressed(&r, 1)) FAIL("[ucstack]: suspended coroutine stack never compressed");
    swapcontext(&g_main_uc, &g_co_uc);
    if (g_co_sum != 2 * first) FAIL("[ucstack]: sum %d want %d", g_co_sum, 2 * first);
    munmap(stk, kStack);
    fprintf(stderr, "external_patterns_test: ucstack PASSED\n");
    return true;
}
#endif

// MAP_NORESERVE reservation, mostly never touched (mimalloc maps 1 GiB
// arenas this way). Only resident pages may be compressed: "compressing" an
// untouched page saves nothing and turns the app's first touch into a fault.
bool testNoreserve() {
    constexpr size_t kRes = 64 * 1024 * 1024;
    char* p = static_cast<char*>(mmap(nullptr, kRes, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0));
    if (p == MAP_FAILED) FAIL("[noreserve]: mmap errno=%d", errno);
    fill(p, kLen, 14);  // touch only the first 2 MiB
    Region r{p, kLen, "noreserve"};
    if (!waitCompressed(&r, 1)) FAIL("[noreserve]: touched part never compressed");
    sleep(1);
    if (size_t n = inaccessiblePages(p + kLen, kRes - kLen); n != 0)
        FAIL("[noreserve]: %zu untouched pages were made inaccessible", n);
#ifdef __linux__
    // uffd mode: registering the reservation maps its untouched pages to the
    // zero page (a MISSING PTE in a registered range would make a kernel
    // access fail with EFAULT), and none of them may be reclaimed.
    if (uffdActive()) {
        if (size_t n = compressedPages(p + kLen, kRes - kLen); n != 0)
            FAIL("[noreserve]: %zu untouched pages left MISSING in a uffd-registered range", n);
        // A kernel-mode write into an untouched page must not EFAULT.
        int pfd[2];
        if (pipe(pfd) != 0) FAIL("[noreserve]: pipe");
        if (write(pfd[1], "xyz", 3) != 3) FAIL("[noreserve]: write");
        const ssize_t r = syscall(SYS_read, pfd[0], p + kLen + 5 * g_page, 3);
        close(pfd[0]);
        close(pfd[1]);
        if (r != 3) FAIL("[noreserve]: raw read() into an untouched page returned %zd errno=%d", r, errno);
    }
#endif
    if (check(p, kLen, 14) != SIZE_MAX) FAIL("[noreserve]: touched part corrupted");
    if (!allZero(p + kLen, 4 * g_page)) FAIL("[noreserve]: untouched part not zero");
    munmap(p, kRes);
    fprintf(stderr, "external_patterns_test: noreserve PASSED\n");
    return true;
}

#ifdef MADV_HUGEPAGE
// jemalloc / mimalloc may madvise(MADV_HUGEPAGE). Compressing 4 KiB pieces of
// a THP region splits it; contents must survive, and DONTNEED of part of a
// compressed huge region must still read back zero.
bool testThp() {
    constexpr size_t kHuge = 2 * 1024 * 1024, kRegion = 4 * kHuge;
    char* raw = static_cast<char*>(mapAnon(kRegion + kHuge));
    char* p = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(raw) + kHuge - 1) & ~(kHuge - 1));
    (void)madvise(p, kRegion, MADV_HUGEPAGE);  // may be unsupported: still a valid test
    fill(p, kRegion, 15);
    Region r{p, kRegion, "thp"};
    if (!waitCompressed(&r, 1)) FAIL("[thp]: never compressed");
    if (madvise(p, kHuge, MADV_DONTNEED) != 0) FAIL("[thp]: dontneed errno=%d", errno);
#ifdef __linux__
    if (!allZero(p, kHuge)) FAIL("[thp]: purged huge page not zero");
#endif
    if (size_t bad = check(p + kHuge, kRegion - kHuge, 15 + kHuge / g_page); bad != SIZE_MAX)
        FAIL("[thp]: page %zu wrong", bad);
    munmap(raw, kRegion + kHuge);
    fprintf(stderr, "external_patterns_test: thp PASSED\n");
    return true;
}
#endif

#ifdef __linux__
long readLongFile(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char b[64];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    return strtol(b, nullptr, 10);
}
long vmaCount() {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    long n = 0;
    static char b[65536];
    ssize_t r;
    while ((r = read(fd, b, sizeof(b))) > 0)
        for (ssize_t k = 0; k < r; ++k) n += b[k] == '\n';
    close(fd);
    return n;
}

// External pages always take the classic mprotect path, which splits VMAs.
// Near vm.max_map_count a PROT_NONE mprotect fails with ENOMEM; the reclaim
// then decommitted the still-readable page and the app read zeros (seen on a
// 1 GiB mimalloc arena in uffd mode, where the VMA guard assumed no splits).
// Use up VMAs to just below the cap, then let a region with alternating
// hot/cold pages age: every page must keep its data.
bool testVmaCap() {
    const long cap = readLongFile("/proc/sys/vm/max_map_count");
    if (cap <= 0 || cap > (1L << 20)) {
        fprintf(stderr, "external_patterns_test: vmacap SKIPPED (max_map_count=%ld)\n", cap);
        return true;
    }
    constexpr long kHeadroom = 3000;
    const long need = cap - kHeadroom - vmaCount();
    char* filler = nullptr;
    size_t filler_len = 0;
    if (need > 0) {
        filler_len = static_cast<size_t>(need + 2) * g_page;
        filler = static_cast<char*>(mmap(nullptr, filler_len, PROT_NONE,
                                         MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0));
        if (filler == MAP_FAILED) FAIL("[vmacap]: filler mmap errno=%d", errno);
        for (long k = 0; k < need; k += 2)
            if (mprotect(filler + k * g_page, g_page, PROT_READ) != 0) break;
    }
    constexpr size_t kRegion = 32 * 1024 * 1024;
    char* p = static_cast<char*>(mapAnon(kRegion));
    fill(p, kRegion, 16);
    const size_t pages = kRegion / g_page;
    double t0 = nowSec();
    while (nowSec() - t0 < 6) {  // keep odd pages hot, let even pages age
        for (size_t i = 1; i < pages; i += 2) p[i * g_page] = static_cast<char>(((i + 16) & 0xff) | 1);
        usleep(100 * 1000);
    }
    size_t bad = check(p, kRegion, 16);
    if (filler) munmap(filler, filler_len);
    if (bad != SIZE_MAX) FAIL("[vmacap]: page %zu lost its data near vm.max_map_count", bad);
    munmap(p, kRegion);
    fprintf(stderr, "external_patterns_test: vmacap PASSED\n");
    return true;
}
#endif

// mlock'd memory must stay resident (it often holds secrets kept out of
// swap): locking a compressed range restores it and smash never compresses
// it again.
bool testMlock() {
    void* p = mapAnon(kLen);
    fill(p, kLen, 17);
    Region r{p, kLen, "mlock"};
    if (!waitCompressed(&r, 1)) FAIL("[mlock]: never compressed");
    rlimit rl{};
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 2 * kLen) {
        fprintf(stderr, "external_patterns_test: mlock SKIPPED (RLIMIT_MEMLOCK=%llu)\n",
                (unsigned long long)rl.rlim_cur);
        munmap(p, kLen);
        return true;
    }
    // (Before the fix, mlock of a compressed — PROT_NONE — range failed
    // with ENOMEM.)
    if (mlock(p, kLen) != 0) FAIL("[mlock]: mlock errno=%d", errno);
    if (size_t bad = check(p, kLen, 17); bad != SIZE_MAX) FAIL("[mlock]: page %zu wrong", bad);
    sleep(3);  // several cold timeouts
    if (size_t n = compressedPages(p, kLen); n != 0) FAIL("[mlock]: %zu locked pages compressed", n);
    munlock(p, kLen);
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: mlock PASSED\n");
    return true;
}

// Path arguments on compressed pages: the kernel's string copy returns
// EFAULT without raising a signal (a Valkey BGSAVE child's rename() of its
// RDB file failed this way — the target path is a heap string). open,
// rename, stat, unlink with paths living in compressed memory must work.
bool testPathArgs() {
    char* p = static_cast<char*>(mapAnon(kLen));
    fill(p, kLen, 18);
    char* a = p + 10 * g_page + 100;   // two paths in the middle of the region
    char* b = p + 20 * g_page + 100;
    snprintf(a, 64, "/tmp/smash-extpat-path-%d-a", static_cast<int>(getpid()));
    snprintf(b, 64, "/tmp/smash-extpat-path-%d-b", static_cast<int>(getpid()));
    Region r{p, kLen, "path"};
    // The pages holding the paths were rewritten after the fill; wait for
    // those exact pages to be compressed.
    Region ra{p + 10 * g_page, g_page, "path-a"}, rb{p + 20 * g_page, g_page, "path-b"};
    Region rs[] = {r, ra, rb};
    if (!waitCompressed(rs, 3)) FAIL("[path]: never compressed");
    int fd = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) FAIL("[path]: open(compressed path) errno=%d", errno);
    close(fd);
    while (compressedPages(p + 10 * g_page, g_page) == 0 ||
           compressedPages(p + 20 * g_page, g_page) == 0)
        usleep(100 * 1000);  // let both recompress, so rename sees compressed paths
    if (rename(a, b) != 0) FAIL("[path]: rename(compressed paths) errno=%d", errno);
    struct stat st;
    if (stat(b, &st) != 0) FAIL("[path]: stat errno=%d", errno);
    while (compressedPages(p + 20 * g_page, g_page) == 0) usleep(100 * 1000);
    FILE* f = fopen(b, "r");  // glibc/libSystem open internally: no syscall wrapper sees it
    if (!f) FAIL("[path]: fopen(compressed path) errno=%d", errno);
    fclose(f);
    if (unlink(b) != 0) FAIL("[path]: unlink errno=%d", errno);
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: path PASSED\n");
    return true;
}

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

// ── Hazard guards: memory smash must never compress ────────────────────
//
// Each case waits until a CONTROL region mapped and filled at the same time
// has compressed (so enough cold ticks have passed), then checks the guarded
// region was never compressed and still holds its data.

// Fill a control region and the guarded one, wait for the control.
bool waitControl(const char* tag, void** ctl_out) {
    void* ctl = mapAnon(kLen);
    fill(ctl, kLen, 40);
    Region r{ctl, kLen, tag};
    *ctl_out = ctl;
    return waitCompressed(&r, 1);
}

// MAP_PRIVATE|MAP_ANON|PROT_EXEC (a JIT code arena): never tracked. smash
// restores PROT_RW on decompress, which would strip PROT_EXEC.
bool testExec() {
    void* x = mmap(nullptr, kLen, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (x == MAP_FAILED) {
        fprintf(stderr, "external_patterns_test: exec SKIPPED (RWX mmap errno=%d)\n", errno);
        return true;
    }
    fill(x, kLen, 41);
    void* ctl = nullptr;
    if (!waitControl("exec-control", &ctl)) FAIL("[exec]: control never compressed");
    if (size_t n = compressedPages(x, kLen); n != 0) FAIL("[exec]: %zu PROT_EXEC pages compressed", n);
    if (check(x, kLen, 41) != SIZE_MAX) FAIL("[exec]: RWX region corrupted");
    munmap(x, kLen);
    munmap(ctl, kLen);
    fprintf(stderr, "external_patterns_test: exec PASSED\n");
    return true;
}

#ifdef MAP_LOCKED
// mmap(MAP_LOCKED): locked at creation, never tracked.
bool testMapLocked() {
    rlimit rl{};
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 2 * kLen) {
        fprintf(stderr, "external_patterns_test: maplocked SKIPPED (RLIMIT_MEMLOCK=%llu)\n",
                (unsigned long long)rl.rlim_cur);
        return true;
    }
    void* l = mmap(nullptr, kLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_LOCKED, -1, 0);
    if (l == MAP_FAILED) FAIL("[maplocked]: mmap errno=%d", errno);
    fill(l, kLen, 42);
    void* ctl = nullptr;
    if (!waitControl("maplocked-control", &ctl)) FAIL("[maplocked]: control never compressed");
    if (size_t n = compressedPages(l, kLen); n != 0) FAIL("[maplocked]: %zu MAP_LOCKED pages compressed", n);
    if (check(l, kLen, 42) != SIZE_MAX) FAIL("[maplocked]: corrupted");
    munmap(l, kLen);
    munmap(ctl, kLen);
    fprintf(stderr, "external_patterns_test: maplocked PASSED\n");
    return true;
}
#endif

#ifdef __linux__
// madvise(MADV_WIPEONFORK) (OpenSSL / glibc arc4random keep RNG state in
// such pages): a fork child must read zeros. A page smash holds compressed
// used to come back in the child decompressed from the PARENT's blob.
bool testWipeOnFork() {
    void* p = mapAnon(kLen);
    fill(p, kLen, 43);
    Region r{p, kLen, "wipeonfork"};
    if (!waitCompressed(&r, 1)) FAIL("[wipeonfork]: never compressed");
    if (madvise(p, kLen, 18 /* MADV_WIPEONFORK */) != 0) {
        munmap(p, kLen);
        fprintf(stderr, "external_patterns_test: wipeonfork SKIPPED (errno=%d)\n", errno);
        return true;
    }
    pid_t pid = fork();
    if (pid == 0) _exit(allZero(p, kLen) ? 0 : 3);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        FAIL("[wipeonfork]: child status 0x%x (3 = child saw the parent's data)", st);
    if (check(p, kLen, 43) != SIZE_MAX) FAIL("[wipeonfork]: parent corrupted");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: wipeonfork PASSED\n");
    return true;
}

// madvise(MADV_DONTFORK) (RDMA / DMA buffers): the child has no mapping
// there. The child's compressor must not touch those pages.
bool testDontFork() {
    void* p = mapAnon(kLen);
    fill(p, kLen, 44);
    Region r{p, kLen, "dontfork"};
    if (!waitCompressed(&r, 1)) FAIL("[dontfork]: never compressed");
    if (madvise(p, kLen, 10 /* MADV_DONTFORK */) != 0) FAIL("[dontfork]: madvise errno=%d", errno);
    pid_t pid = fork();
    if (pid == 0) {
        // Give the child's compressor several cold timeouts over the
        // inherited page table entries.
        void* q = mapAnon(kLen);
        fill(q, kLen, 45);
        sleep(4);
        _exit(check(q, kLen, 45) == SIZE_MAX ? 0 : 3);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) FAIL("[dontfork]: child status 0x%x", st);
    if (check(p, kLen, 44) != SIZE_MAX) FAIL("[dontfork]: parent corrupted");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: dontfork PASSED\n");
    return true;
}

// madvise(MADV_POPULATE_WRITE) of a compressed range must succeed (the
// kernel's prefault fails on a MISSING or PROT_NONE page).
bool testPopulate() {
    {
        void* probe = mapAnon(g_page);
        const int rc = madvise(probe, g_page, 23 /* MADV_POPULATE_WRITE */);
        munmap(probe, g_page);
        if (rc != 0) {
            fprintf(stderr, "external_patterns_test: populate SKIPPED (errno=%d, kernel < 5.14)\n", errno);
            return true;
        }
    }
    void* p = mapAnon(kLen);
    fill(p, kLen, 46);
    Region r{p, kLen, "populate"};
    if (!waitCompressed(&r, 1)) FAIL("[populate]: never compressed");
    if (madvise(p, kLen, 23 /* MADV_POPULATE_WRITE */) != 0)
        FAIL("[populate]: MADV_POPULATE_WRITE on compressed pages errno=%d", errno);
    if (check(p, kLen, 46) != SIZE_MAX) FAIL("[populate]: corrupted");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: populate PASSED\n");
    return true;
}

// madvise(MADV_GUARD_INSTALL) (Linux 6.13 lightweight guard pages): the
// range must fault for the app afterwards, not be served by smash.
sigjmp_buf g_guard_jmp;
void guardSegv(int) { siglongjmp(g_guard_jmp, 1); }
bool testGuardInstall() {
    char* p = static_cast<char*>(mapAnon(kLen));
    fill(p, kLen, 47);
    Region r{p, kLen, "guardinstall"};
    if (!waitCompressed(&r, 1)) FAIL("[guardinstall]: never compressed");
    if (madvise(p, g_page, 102 /* MADV_GUARD_INSTALL */) != 0) {
        munmap(p, kLen);
        fprintf(stderr, "external_patterns_test: guardinstall SKIPPED (errno=%d, kernel < 6.13)\n", errno);
        return true;
    }
    // Rest of the range intact.
    if (check(p + g_page, kLen - g_page, 47 + 1) != SIZE_MAX) FAIL("[guardinstall]: rest corrupted");
    // The guard page faults (SIGSEGV to the app), it is not decompressed.
    // smash's handler forwards a fault it does not own to the previous
    // handler, which is ours here.
    struct sigaction sa{}, old{};
    sa.sa_handler = guardSegv;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old);
    volatile bool faulted = false;
    if (sigsetjmp(g_guard_jmp, 1) == 0) {
        (void)*reinterpret_cast<volatile char*>(p);
    } else {
        faulted = true;
    }
    sigaction(SIGSEGV, &old, nullptr);
    if (!faulted) FAIL("[guardinstall]: guard page readable");
    munmap(p, kLen);
    fprintf(stderr, "external_patterns_test: guardinstall PASSED\n");
    return true;
}

// An application that uses userfaultfd itself. (1) It registers a range
// smash has compressed (through smash's own uffd, where uffd is on): the
// REGISTER must succeed and the data survive; smash never compresses the
// range again. (2) It registered a range before smash saw it (raw ioctl,
// i.e. not through libsmash's ioctl()): smash's own registration fails
// with EBUSY and the range must never be compressed.
long appUffd() {
    long fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0) fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    // SIGBUS on a MISSING fault: a page smash left MISSING under the app's
    // registration kills the test visibly instead of blocking it forever.
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_SIGBUS;
    if (ioctl(static_cast<int>(fd), UFFDIO_API, &api) != 0) { close(static_cast<int>(fd)); return -1; }
    return fd;
}
bool testAppUffd() {
    long ufd = appUffd();
    if (ufd < 0) {
        fprintf(stderr, "external_patterns_test: appuffd SKIPPED (userfaultfd unavailable)\n");
        return true;
    }
    // (1)
    void* p = mapAnon(kLen);
    fill(p, kLen, 48);
    Region r{p, kLen, "appuffd"};
    if (!waitCompressed(&r, 1)) FAIL("[appuffd]: never compressed");
    uffdio_register reg{};
    reg.range.start = reinterpret_cast<uint64_t>(p);
    reg.range.len = kLen;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(static_cast<int>(ufd), UFFDIO_REGISTER, &reg) != 0)
        FAIL("[appuffd]: app's UFFDIO_REGISTER of a range smash compressed errno=%d", errno);
    if (check(p, kLen, 48) != SIZE_MAX) FAIL("[appuffd]: data lost under the app's registration");
    // (2) raw ioctl: libsmash never sees it. Fill first: the app's MISSING
    // registration only covers pages that are not present.
    void* q = mapAnon(kLen);
    fill(q, kLen, 49);
    uffdio_register reg2{};
    reg2.range.start = reinterpret_cast<uint64_t>(q);
    reg2.range.len = kLen;
    reg2.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (syscall(SYS_ioctl, ufd, UFFDIO_REGISTER, &reg2) != 0) FAIL("[appuffd]: raw REGISTER errno=%d", errno);
    void* ctl = nullptr;
    if (!waitControl("appuffd-control", &ctl)) FAIL("[appuffd]: control never compressed");
    if (size_t n = compressedPages(p, kLen); n != 0) FAIL("[appuffd]: %zu pages of the app's range compressed", n);
    if (size_t n = compressedPages(q, kLen); n != 0)
        FAIL("[appuffd]: %zu pages of a range the app registered first were compressed", n);
    if (check(p, kLen, 48) != SIZE_MAX || check(q, kLen, 49) != SIZE_MAX) FAIL("[appuffd]: corrupted");
    close(static_cast<int>(ufd));
    munmap(p, kLen);
    munmap(q, kLen);
    munmap(ctl, kLen);
    fprintf(stderr, "external_patterns_test: appuffd PASSED\n");
    return true;
}

// Fork near vm.max_map_count with scattered compressed pages. In uffd mode
// a compressed page is MISSING, which the child (no uffd) would read as
// zeros, so before fork smash protects each one PROT_NONE. Near the cap that
// mprotect fails; the page must then be decompressed in place, not left
// MISSING.
bool testForkCap() {
    const long cap = readLongFile("/proc/sys/vm/max_map_count");
    if (cap <= 0 || cap > (1L << 20)) {
        fprintf(stderr, "external_patterns_test: forkcap SKIPPED (max_map_count=%ld)\n", cap);
        return true;
    }
    constexpr size_t kRegion = 16 * 1024 * 1024;
    char* p = static_cast<char*>(mapAnon(kRegion));
    fill(p, kRegion, 50);
    const size_t pages = kRegion / g_page;
    // Odd pages hot, even pages age into compression.
    double t0 = nowSec();
    while (nowSec() - t0 < 30) {
        for (size_t i = 1; i < pages; i += 2) p[i * g_page] = static_cast<char>(((i + 50) & 0xff) | 1);
        if (compressedPages(p, kRegion) * 4 >= pages) break;
        usleep(100 * 1000);
    }
    const size_t comp = compressedPages(p, kRegion);
    // Use up VMAs to just below the cap.
    const long need = cap - 200 - vmaCount();
    char* filler = nullptr;
    size_t filler_len = 0;
    if (need > 0) {
        filler_len = static_cast<size_t>(need + 2) * g_page;
        filler = static_cast<char*>(mmap(nullptr, filler_len, PROT_NONE,
                                         MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0));
        if (filler == MAP_FAILED) FAIL("[forkcap]: filler mmap errno=%d", errno);
        for (long k = 0; k < need; k += 2)
            if (mprotect(filler + k * g_page, g_page, PROT_READ) != 0) break;
    }
    pid_t pid = fork();
    if (pid == 0) _exit(check(p, kRegion, 50) == SIZE_MAX ? 0 : 3);
    int st = 0;
    waitpid(pid, &st, 0);
    if (filler) munmap(filler, filler_len);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        FAIL("[forkcap]: child status 0x%x (3 = a page read wrong in the child; %zu compressed at fork)", st, comp);
    if (check(p, kRegion, 50) != SIZE_MAX) FAIL("[forkcap]: parent corrupted");
    munmap(p, kRegion);
    fprintf(stderr, "external_patterns_test: forkcap PASSED (%zu pages compressed at fork)\n", comp);
    return true;
}
#endif

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
    if (want("sigmask") && !testSigmask()) return 1;
    if (want("mlock") && !testMlock()) return 1;
    if (want("path") && !testPathArgs()) return 1;
    if (want("noreserve") && !testNoreserve()) return 1;
#ifdef MADV_HUGEPAGE
    if (want("thp") && !testThp()) return 1;
#endif
#ifdef __linux__
    if (want("sbrk") && !testSbrk()) return 1;
    if (want("ucstack") && !testCoroutineStack()) return 1;
    if (want("vmacap") && !testVmaCap()) return 1;
#endif
    if (want("exec") && !testExec()) return 1;
#ifdef MAP_LOCKED
    if (want("maplocked") && !testMapLocked()) return 1;
#endif
#ifdef __linux__
    if (want("wipeonfork") && !testWipeOnFork()) return 1;
    if (want("dontfork") && !testDontFork()) return 1;
    if (want("populate") && !testPopulate()) return 1;
    if (want("guardinstall") && !testGuardInstall()) return 1;
    if (want("appuffd") && !testAppUffd()) return 1;
    if (want("forkcap") && !testForkCap()) return 1;
#endif
    if (want("churn") && !testChurn()) return 1;
    if (g_failures) return 1;
    fprintf(stderr, "external_patterns_test: ALL PASSED\n");
    return 0;
}
