// End-to-end kill-switch test, run under LD_PRELOAD / DYLD_INSERT_LIBRARIES in
// both modes (SMASH_LARGE_ONLY=0/1) and for each trigger:
//   kill_switch_test api     smash_disable() / smash_enable()
//   kill_switch_test signal  SMASH_DISABLE_SIGNAL / SMASH_ENABLE_SIGNAL
//   kill_switch_test file    SMASH_CONTROL_FILE "disable" / "enable"
//
// Sequence:
//   1. allocate compressible large (>= 1 MiB: compressible in both modes) and
//      small chunks, each with its own byte pattern; wait until pages compress.
//   2. trigger the disable WITHOUT touching the data (so the drain, not the
//      fault handler, must restore every page); wait for "inert".
//   3. assert: compressed_pages == 0; every page of every chunk is mapped
//      PROT_READ|PROT_WRITE by the kernel (/proc/self/maps on Linux,
//      mach_vm_region on macOS); the data is byte-exact; a full read+write
//      pass plus 3 more compressor ticks induce zero smash faults and no page
//      compresses again.
//   4. re-enable with the same trigger; assert pages compress again and read
//      back byte-exact through the fault path.
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {

struct Status {  // mirrors SmashSafetyStatus (include/smash/smash.h)
    int disabled, inert;
    unsigned long long compressed_pages, reclaimed_pages, faults, decompress_faults, decompress_failures,
        watchdog_trips;
    unsigned cold_floor_ticks, slo_multiplier_x256;
    unsigned long long fault_rate, fault_p99_us;
    char reason[128];
};
using status_fn = int (*)(Status*);
using toggle_fn = int (*)();
status_fn g_status;
toggle_fn g_disable, g_enable;

constexpr size_t kPage = 4096;
constexpr size_t kLargeBytes = 2 * 1024 * 1024;
constexpr size_t kNumLarge = 16;
constexpr size_t kSmallBytes = 4096;
constexpr size_t kNumSmall = 256;

unsigned char* g_large[kNumLarge];
unsigned char* g_small[kNumSmall];

unsigned char pat(size_t chunk, size_t off) {
    // Compressible (runs of 64), never all-zero, distinct per chunk.
    return static_cast<unsigned char>(1 + ((chunk * 7 + off / 64) % 251));
}
void fill(unsigned char* p, size_t n, size_t id) {
    for (size_t j = 0; j < n; ++j) p[j] = pat(id, j);
}
bool check(const unsigned char* p, size_t n, size_t id, const char* what) {
    for (size_t j = 0; j < n; ++j)
        if (p[j] != pat(id, j)) {
            fprintf(stderr, "FAIL: %s chunk %zu byte %zu = 0x%02x want 0x%02x\n", what, id, j,
                    p[j], pat(id, j));
            return false;
        }
    return true;
}
bool checkAll(const char* when) {
    for (size_t i = 0; i < kNumLarge; ++i)
        if (!check(g_large[i], kLargeBytes, i, when)) return false;
    for (size_t i = 0; i < kNumSmall; ++i)
        if (!check(g_small[i], kSmallBytes, 1000 + i, when)) return false;
    return true;
}
// Rewrite every byte with its own value: a write to every page.
void writeAll() {
    for (size_t i = 0; i < kNumLarge; ++i)
        for (size_t j = 0; j < kLargeBytes; j += kPage)
            *reinterpret_cast<volatile unsigned char*>(g_large[i] + j) = pat(i, j);
    for (size_t i = 0; i < kNumSmall; ++i)
        *reinterpret_cast<volatile unsigned char*>(g_small[i]) = pat(1000 + i, 0);
}

Status status() {
    Status s{};
    if (g_status(&s) != 0) {
        fprintf(stderr, "FAIL: smash_safety_status failed\n");
        exit(1);
    }
    return s;
}

// Is [p, p+n) entirely mapped read+write?
bool rangeIsRw(const void* p, size_t n) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(p) & ~(kPage - 1);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(p) + n;
#if defined(__linux__)
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    uintptr_t covered = lo;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s = 0, e = 0;
        char perms[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3) continue;
        if (e <= lo || s >= hi) continue;
        if (perms[0] != 'r' || perms[1] != 'w') {
            fprintf(stderr, "  non-RW VMA %lx-%lx %s overlaps [%lx,%lx)\n", s, e, perms,
                    (unsigned long)lo, (unsigned long)hi);
            ok = false;
        }
        if (s <= covered && e > covered) covered = e;
    }
    fclose(f);
    return ok && covered >= hi;
#elif defined(__APPLE__)
    mach_vm_address_t addr = lo;
    while (addr < hi) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        mach_vm_address_t a = addr;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) != KERN_SUCCESS)
            return false;
        if (a > addr) return false;  // hole
        if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) {
            fprintf(stderr, "  non-RW region at %llx prot=%d\n", (unsigned long long)a,
                    info.protection);
            return false;
        }
        addr = a + size;
    }
    return true;
#else
    (void)lo; (void)hi;
    return true;
#endif
}

bool allRw() {
    for (size_t i = 0; i < kNumLarge; ++i)
        if (!rangeIsRw(g_large[i], kLargeBytes)) { fprintf(stderr, "  large %zu\n", i); return false; }
    for (size_t i = 0; i < kNumSmall; ++i)
        if (!rangeIsRw(g_small[i], kSmallBytes)) { fprintf(stderr, "  small %zu\n", i); return false; }
    return true;
}

void writeControl(const char* word) {
    const char* path = getenv("SMASH_CONTROL_FILE");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "FAIL: open %s\n", path); exit(1); }
    if (write(fd, word, strlen(word)) < 0) { fprintf(stderr, "FAIL: write\n"); exit(1); }
    close(fd);
}

bool waitFor(const char* what, int sec, bool (*pred)(const Status&)) {
    for (int ms = 0; ms < sec * 1000; ms += 50) {
        if (pred(status())) return true;
        usleep(50 * 1000);
    }
    fprintf(stderr, "FAIL: timed out after %d s waiting for %s\n", sec, what);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mech = argc > 1 ? argv[1] : "api";
    const char* lo = getenv("SMASH_LARGE_ONLY");
    const char* mode = lo && lo[0] == '1' ? "large-only" : "full";
    fprintf(stderr, "kill_switch_test: mechanism=%s mode=%s\n", mech.c_str(), mode);

    g_status = reinterpret_cast<status_fn>(dlsym(RTLD_DEFAULT, "smash_safety_status"));
    g_disable = reinterpret_cast<toggle_fn>(dlsym(RTLD_DEFAULT, "smash_disable"));
    g_enable = reinterpret_cast<toggle_fn>(dlsym(RTLD_DEFAULT, "smash_enable"));
    if (!g_status || !g_disable || !g_enable) {
        fprintf(stderr, "FAIL: smash safety API not exported (not running under libsmash?)\n");
        return 1;
    }
    if (mech == "file") writeControl("enable");

    for (size_t i = 0; i < kNumLarge; ++i) {
        g_large[i] = static_cast<unsigned char*>(malloc(kLargeBytes));
        fill(g_large[i], kLargeBytes, i);
    }
    for (size_t i = 0; i < kNumSmall; ++i) {
        g_small[i] = static_cast<unsigned char*>(malloc(kSmallBytes));
        fill(g_small[i], kSmallBytes, 1000 + i);
    }
    for (int i = 0; i < 5000; ++i) free(malloc(64));  // past any warmup gate

    // Wait for RECLAIMED pages (frame dropped, PROT_NONE / uffd-MISSING), not
    // just shadow copies: the drain must exercise real decompress-in-place.
    if (!waitFor("pages to be reclaimed", 40,
                 [](const Status& s) { return s.reclaimed_pages >= 1024; }))
        return 1;
    Status s0 = status();
    fprintf(stderr, "kill_switch_test: compressed=%llu (reclaimed=%llu) before disable\n",
            s0.compressed_pages, s0.reclaimed_pages);

    // 2. Disable. No data access until the drain has finished.
    if (mech == "api") {
        int rc = g_disable();
        if (rc != 0) { fprintf(stderr, "FAIL: smash_disable() = %d\n", rc); return 1; }
    } else if (mech == "signal") {
        kill(getpid(), SIGUSR1);
    } else {
        writeControl("disable");
    }
    if (!waitFor("inert", 30, [](const Status& s) { return s.disabled && s.inert; })) return 1;
    Status s1 = status();
    fprintf(stderr, "kill_switch_test: disabled (reason=\"%s\") compressed=%llu faults=%llu\n",
            s1.reason, s1.compressed_pages, s1.faults);
    if (s1.compressed_pages != 0) {
        fprintf(stderr, "FAIL: %llu pages still compressed after the drain\n", s1.compressed_pages);
        return 1;
    }
    const char* want = mech == "api" ? "api" : mech == "signal" ? "signal" : "control-file";
    if (std::strncmp(s1.reason, want, std::strlen(want)) != 0) {
        fprintf(stderr, "FAIL: reason \"%s\", want \"%s\"\n", s1.reason, want);
        return 1;
    }
    // 3. Inert: kernel protections are plain RW, data is exact, no faults.
    if (!allRw()) { fprintf(stderr, "FAIL: protected page left after the drain\n"); return 1; }
    if (!checkAll("after drain")) return 1;
    writeAll();
    sleep(3);  // >= 3 compressor ticks while disabled
    if (!checkAll("after disabled ticks")) return 1;
    writeAll();
    Status s2 = status();
    if (s2.faults != s1.faults) {
        fprintf(stderr, "FAIL: %llu smash faults induced while disabled\n", s2.faults - s1.faults);
        return 1;
    }
    if (s2.compressed_pages != 0 || !s2.disabled) {
        fprintf(stderr, "FAIL: compressed=%llu disabled=%d while disabled\n", s2.compressed_pages,
                s2.disabled);
        return 1;
    }
    if (!allRw()) { fprintf(stderr, "FAIL: protection re-applied while disabled\n"); return 1; }
    fprintf(stderr, "kill_switch_test: inert OK (0 faults across a full read+write pass + 3 s)\n");

    // 4. Re-enable: compression resumes and the data survives it.
    if (mech == "api") {
        int rc = g_enable();
        if (rc != 0) { fprintf(stderr, "FAIL: smash_enable() = %d\n", rc); return 1; }
    } else if (mech == "signal") {
        kill(getpid(), SIGUSR2);
    } else {
        writeControl("enable");
    }
    if (!waitFor("re-enable", 10, [](const Status& s) { return !s.disabled; })) return 1;
    if (!waitFor("pages to be reclaimed again", 40,
                 [](const Status& s) { return s.reclaimed_pages >= 1024; }))
        return 1;
    Status s3 = status();
    if (!checkAll("after re-enable")) return 1;
    Status s4 = status();
    fprintf(stderr, "kill_switch_test: re-enabled, reclaimed=%llu, read-back faults=%llu\n",
            s3.reclaimed_pages, s4.faults - s3.faults);
    if (s4.decompress_faults == s3.decompress_faults) {
        fprintf(stderr, "FAIL: read-back of reclaimed pages took no decompress fault\n");
        return 1;
    }
    if (s4.decompress_failures != 0) {
        fprintf(stderr, "FAIL: %llu decompression failures\n", s4.decompress_failures);
        return 1;
    }
    fprintf(stderr, "kill_switch_test: PASS (%s, %s)\n", mech.c_str(), mode);
    return 0;
}
