// SMASH_TRACK_EXTERNAL unset ("auto"): memory the program maps itself is
// compressed iff smash reclaims through userfaultfd (Linux 6.8+, SMASH_UFFD
// not 0); on the classic path (macOS, older kernels, SMASH_UFFD=0) it is not.
//
//   te_auto_test [on|off|detect]   (default detect: on iff a userfaultfd is
//                                   open in this process, i.e. smash's)
//
// Run under the preloaded libsmash with SMASH_TRACK_EXTERNAL unset. Two
// mmap'd regions: one mapped from an early constructor (before main, right
// after libsmash's own initialisation), one in main. A malloc'd control
// region (>= 1 MiB: managed in both modes) must compress either way, so the
// "off" verdict is not just "nothing compressed yet". Exits 77 when libsmash
// is not preloaded.
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {

size_t g_page = 4096;
constexpr size_t kLen = 4u << 20;
char* g_early = nullptr;

void fill(char* p, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; ++i) p[i] = static_cast<char>(((i / 128) * 3 + seed) | 1);
}
bool check(const char* p, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; ++i)
        if (p[i] != static_cast<char>(((i / 128) * 3 + seed) | 1)) return false;
    return true;
}

__attribute__((constructor(101))) void early() {
    void* m = mmap(nullptr, kLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return;
    g_early = static_cast<char*>(m);
    fill(g_early, kLen, 1);
}

// Pages of a filled region smash holds compressed: not resident (Linux
// pagemap: uffd reclaim or classic decommit) or PROT_NONE.
size_t compressedPages(const void* base, size_t len) {
    size_t n = 0;
#ifdef __linux__
    static int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    std::vector<uint64_t> e(len / g_page);
    if (fd >= 0 && pread(fd, e.data(), e.size() * 8,
                         static_cast<off_t>(reinterpret_cast<uintptr_t>(base) / g_page * 8)) ==
                       static_cast<ssize_t>(e.size() * 8))
        // Not present and not swapped, or a uffd-wp marker (reported as a
        // swap entry with bit 57: the arena's reclaimed pages can carry one).
        for (uint64_t v : e) n += !(v & (3ULL << 62)) || ((v >> 62) == 1 && ((v >> 57) & 1));
    // PROT_NONE but still resident (classic, deferred madvise)
    FILE* f = fopen("/proc/self/maps", "r");
    if (f) {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(base), hi = lo + len;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            unsigned long s = 0, z = 0;
            char perms[8] = {};
            if (sscanf(line, "%lx-%lx %7s", &s, &z, perms) == 3 && perms[0] == '-' && perms[1] == '-') {
                uintptr_t a = s > lo ? s : lo, b = z < hi ? z : hi;
                for (uintptr_t p = a; p < b; p += g_page) {
                    const size_t k = (p - lo) / g_page;
                    if (k < e.size() && (e[k] & (1ULL << 63))) ++n;  // resident but PROT_NONE
                }
            }
        }
        fclose(f);
    }
#else
    mach_vm_address_t addr = reinterpret_cast<uintptr_t>(base);
    const uintptr_t hi = addr + len;
    while (addr < hi) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        mach_vm_address_t a = addr;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) != KERN_SUCCESS || a >= hi)
            break;
        const uintptr_t s = a > addr ? a : addr, z = a + size < hi ? a + size : hi;
        if (info.protection == VM_PROT_NONE && s < z) n += (z - s) / g_page;
        addr = a + size;
    }
#endif
    return n;
}

bool uffdOpen() {
#ifdef __linux__
    char path[64], link[128];
    for (int fd = 0; fd < 1024; ++fd) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, link, sizeof(link) - 1);
        if (n <= 0) continue;
        link[n] = 0;
        if (std::strstr(link, "userfaultfd")) return true;
    }
#endif
    return false;
}

double nowSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

}  // namespace

int main(int argc, char** argv) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) g_page = static_cast<size_t>(ps);
    if (!dlsym(RTLD_DEFAULT, "smash_get_stats")) {
        fprintf(stderr, "te_auto_test: libsmash not preloaded; SKIPPED\n");
        return 77;
    }
    if (getenv("SMASH_TRACK_EXTERNAL")) {
        fprintf(stderr, "te_auto_test: SMASH_TRACK_EXTERNAL is set; this test needs it unset\n");
        return 2;
    }
    const char* how = argc > 1 ? argv[1] : "detect";
    const bool expect_on = std::strcmp(how, "on") == 0 || (std::strcmp(how, "detect") == 0 && uffdOpen());
    if (!g_early) { fprintf(stderr, "te_auto_test: early mmap failed\n"); return 2; }
    char* late = static_cast<char*>(mmap(nullptr, kLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    char* ctl = static_cast<char*>(malloc(kLen));
    if (late == MAP_FAILED || !ctl) return 2;
    fill(late, kLen, 2);
    fill(ctl, kLen, 3);
    const size_t np = kLen / g_page;
    size_t ce = 0, cl = 0, cc = 0;
    const double t0 = nowSec();
    for (;;) {
        ce = compressedPages(g_early, kLen);
        cl = compressedPages(late, kLen);
        cc = compressedPages(ctl, kLen);
        const bool ctl_done = cc * 2 >= np;
        if (expect_on && ctl_done && ce * 2 >= np && cl * 2 >= np) break;
        if (!expect_on && ctl_done && nowSec() - t0 > 5) break;  // a few more cold timeouts
        if (nowSec() - t0 > 60) break;
        usleep(100 * 1000);
    }
    fprintf(stderr, "te_auto_test: expect %s (uffd open: %d); compressed pages of %zu: early mmap %zu, "
                    "late mmap %zu, malloc control %zu\n",
            expect_on ? "on" : "off", uffdOpen() ? 1 : 0, np, ce, cl, cc);
    bool ok = cc * 2 >= np;
    if (!ok) fprintf(stderr, "FAIL: the malloc'd control never compressed\n");
    if (expect_on && (ce * 2 < np || cl * 2 < np)) { fprintf(stderr, "FAIL: external mappings not compressed\n"); ok = false; }
    if (!expect_on && (ce || cl)) { fprintf(stderr, "FAIL: external mappings compressed with tracking off\n"); ok = false; }
    if (!check(g_early, kLen, 1) || !check(late, kLen, 2) || !check(ctl, kLen, 3)) {
        fprintf(stderr, "FAIL: data\n");
        ok = false;
    }
    fprintf(stderr, "te_auto_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
