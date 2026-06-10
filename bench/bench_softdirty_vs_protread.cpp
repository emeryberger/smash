// bench_softdirty_vs_protread.cpp
//
// Compare per-tick cost of two write-tracking schemes Smash uses:
//
//   (a) PROT_READ scheme: mprotect every page to PROT_READ; rely on the
//       SIGSEGV handler to clear the bit on first write.
//   (b) Soft-dirty scheme: read /proc/self/pagemap once, write
//       /proc/self/clear_refs once at tick end.
//
// We don't need Smash's full machinery — we just need to measure the syscall
// cost on a representative-size region. Run with --pages=N to vary the size.
//
// Build: g++ -O2 -std=c++20 bench_softdirty_vs_protread.cpp -o bench_sd
// Run:   ./bench_sd --pages=1000000 --ticks=20

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr size_t kPageSize = 4096;

double now_us() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::micro>>(
               steady_clock::now().time_since_epoch()).count();
}

// ── PROT_READ scheme ─────────────────────────────────────────────────────────
// Mimic Smash's Phase 3 mprotect storm. Walk ACTIVE pages, set them to
// PROT_READ. Smash chunks consecutive pages into one mprotect call when
// possible; we replicate that here (one call per contiguous run).
double protread_phase(void* base, size_t pages) {
    double t0 = now_us();
    if (mprotect(base, pages * kPageSize, PROT_READ) != 0) {
        perror("mprotect(PROT_READ)");
        return -1;
    }
    return now_us() - t0;
}

double protread_restore(void* base, size_t pages) {
    double t0 = now_us();
    if (mprotect(base, pages * kPageSize, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect(PROT_RW)");
        return -1;
    }
    return now_us() - t0;
}

// ── Soft-dirty scheme ────────────────────────────────────────────────────────
double softdirty_read(int pagemap_fd, void* base, size_t pages,
                      uint64_t* buf, size_t batch_size) {
    double t0 = now_us();
    uint64_t base_pfn = (reinterpret_cast<uintptr_t>(base) / kPageSize) *
                        sizeof(uint64_t);
    size_t i = 0;
    size_t dirty_count = 0;
    while (i < pages) {
        size_t batch = pages - i;
        if (batch > batch_size) batch = batch_size;
        ssize_t got = pread(pagemap_fd, buf, batch * sizeof(uint64_t),
                            base_pfn + i * sizeof(uint64_t));
        if (got <= 0) { perror("pread pagemap"); return -1; }
        size_t got_pages = static_cast<size_t>(got) / sizeof(uint64_t);
        for (size_t k = 0; k < got_pages; ++k) {
            if (buf[k] & (1ULL << 55)) ++dirty_count;
        }
        i += got_pages;
        if (got_pages < batch) break;
    }
    double dt = now_us() - t0;
    fprintf(stderr, "  (soft-dirty saw %zu dirty pages)\n", dirty_count);
    return dt;
}

double softdirty_clear(int clear_refs_fd) {
    double t0 = now_us();
    const char buf[3] = {'4', '\n', 0};
    ssize_t w = write(clear_refs_fd, buf, 2);
    if (w != 2) { perror("write clear_refs"); return -1; }
    return now_us() - t0;
}

// ── Workload: simulate app writes between ticks ──────────────────────────────
// Touch every Nth page so both schemes have something to clear.
void simulate_writes(volatile char* base, size_t pages, size_t stride) {
    for (size_t i = 0; i < pages; i += stride) {
        base[i * kPageSize] = static_cast<char>(i & 0xFF);
    }
}

// ── PROT_READ scheme that ACTUALLY faults like Smash does ────────────────────
// Smash sets pages to PROT_READ; the next write traps to SIGSEGV; the handler
// calls mprotect(PROT_RW) on JUST THAT PAGE; that splits the VMA. So a sparse
// write pattern (1 in N) produces N/stride per-page mprotects across the tick,
// not one big call. Measure that path.
double protread_with_faults(volatile char* base, size_t pages, size_t stride) {
    double t0 = now_us();
    // Mark whole region read-only (one big call).
    if (mprotect(const_cast<char*>(base), pages * kPageSize, PROT_READ) != 0) {
        perror("mprotect(PROT_READ)");
        return -1;
    }
    // Now perform sparse writes — each one will fault into the SIGSEGV handler
    // which restores PROT_RW for that single page (= one mprotect per write).
    for (size_t i = 0; i < pages; i += stride) {
        base[i * kPageSize] = static_cast<char>(i & 0xFF);
    }
    return now_us() - t0;
}

// Count VMAs in /proc/self/maps for the test region.
size_t count_vmas_in_region(void* base, size_t pages) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    uintptr_t lo = reinterpret_cast<uintptr_t>(base);
    uintptr_t hi = lo + pages * kPageSize;
    size_t count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) == 2) {
            if (a < hi && b > lo) ++count;
        }
    }
    fclose(f);
    return count;
}

// ── SIGSEGV handler for PROT_READ scheme ─────────────────────────────────────
std::atomic<size_t> g_segv_count{0};
volatile char* g_base = nullptr;
size_t g_pages = 0;

void segv_handler(int /*sig*/, siginfo_t* info, void* /*ctx*/) {
    void* fault_addr = info->si_addr;
    void* page_addr = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(fault_addr) & ~(kPageSize - 1));
    if (mprotect(page_addr, kPageSize, PROT_READ | PROT_WRITE) != 0) _exit(1);
    g_segv_count.fetch_add(1, std::memory_order_relaxed);
}

void install_segv_handler() {
    struct sigaction sa{};
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    size_t pages = 1'000'000;       // ~4 GiB region by default
    int    ticks = 20;
    size_t write_stride = 1000;      // 1 in every 1000 pages dirtied per tick

    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--pages=", 8) == 0) pages = atoll(argv[i] + 8);
        else if (strncmp(argv[i], "--ticks=", 8) == 0) ticks = atoi(argv[i] + 8);
        else if (strncmp(argv[i], "--stride=", 9) == 0) write_stride = atoll(argv[i] + 9);
    }

    fprintf(stderr, "pages=%zu (%.2f GiB), ticks=%d, write_stride=%zu (%.2f%% writes/tick)\n",
            pages, pages * 4.0 / 1024 / 1024, ticks, write_stride,
            100.0 / write_stride);

    // Reserve & commit a region.
    void* base = mmap(nullptr, pages * kPageSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    g_base = static_cast<volatile char*>(base);
    g_pages = pages;

    // Touch every page so they're really resident (otherwise pagemap won't
    // have entries). Single sequential pass.
    fprintf(stderr, "Faulting in pages...\n");
    {
        volatile char* p = static_cast<volatile char*>(base);
        for (size_t i = 0; i < pages; ++i) p[i * kPageSize] = 1;
    }

    install_segv_handler();

    // ── Soft-dirty scheme ────────────────────────────────────────────────────
    int clear_refs_fd = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
    int pagemap_fd    = open("/proc/self/pagemap",    O_RDONLY | O_CLOEXEC);
    if (clear_refs_fd < 0 || pagemap_fd < 0) {
        perror("open /proc/self/{clear_refs,pagemap}");
        return 1;
    }

    constexpr size_t kBatchPages = 4096;
    uint64_t* buf = static_cast<uint64_t*>(
        mmap(nullptr, kBatchPages * sizeof(uint64_t),
             PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (buf == MAP_FAILED) { perror("mmap buf"); return 1; }

    fprintf(stderr, "\n=== Scheme A: soft-dirty ===\n");
    double sd_total = 0;
    double sd_read_total = 0, sd_clear_total = 0;
    softdirty_clear(clear_refs_fd);  // start fresh
    for (int t = 0; t < ticks; ++t) {
        simulate_writes(static_cast<volatile char*>(base), pages, write_stride);
        double r = softdirty_read(pagemap_fd, base, pages, buf, kBatchPages);
        double c = softdirty_clear(clear_refs_fd);
        sd_read_total += r;
        sd_clear_total += c;
        sd_total += r + c;
        fprintf(stderr, "  tick %2d: read=%.0fus clear=%.0fus total=%.0fus\n",
                t, r, c, r + c);
    }

    fprintf(stderr, "\n=== Scheme B: PROT_READ (whole region only, no faults) ===\n");
    g_segv_count.store(0);
    double pr_total = 0;
    for (int t = 0; t < ticks; ++t) {
        // Ensure RW first.
        if (mprotect(base, pages * kPageSize, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect RW pre-tick"); return 1;
        }
        simulate_writes(static_cast<volatile char*>(base), pages, write_stride);
        double p = protread_phase(base, pages);
        pr_total += p;
        fprintf(stderr, "  tick %2d: mprotect(PROT_READ)=%.0fus\n", t, p);
    }
    size_t segvs = g_segv_count.load();
    fprintf(stderr, "  total SIGSEGVs handled: %zu\n", segvs);

    fprintf(stderr, "\n=== Scheme C: PROT_READ + SIGSEGV faults (Smash's actual behaviour) ===\n");
    g_segv_count.store(0);
    // Restore the whole region to RW first.
    if (mprotect(base, pages * kPageSize, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect RW reset"); return 1;
    }
    double prf_total = 0;
    size_t initial_vmas = count_vmas_in_region(base, pages);
    for (int t = 0; t < ticks; ++t) {
        // Each tick: the protect happens; then writes fault one by one, each
        // taking SIGSEGV → handler → mprotect(PROT_RW) per page.
        double p = protread_with_faults(static_cast<volatile char*>(base), pages,
                                         write_stride);
        prf_total += p;
        fprintf(stderr, "  tick %2d: protect+faults=%.0fus\n", t, p);
    }
    size_t segvs_c = g_segv_count.load();
    size_t final_vmas = count_vmas_in_region(base, pages);
    fprintf(stderr, "  total SIGSEGVs handled: %zu (across %d ticks)\n", segvs_c, ticks);
    fprintf(stderr, "  VMAs covering region: %zu -> %zu (%zu added by per-page mprotects)\n",
            initial_vmas, final_vmas,
            final_vmas > initial_vmas ? final_vmas - initial_vmas : 0);

    // ── Summary ──────────────────────────────────────────────────────────────
    fprintf(stderr, "\n=== Summary ===\n");
    fprintf(stderr, "Scheme                            | per-tick avg | total\n");
    fprintf(stderr, "(A) soft-dirty                    | %10.0fus | %10.0fus  (read=%.0fus clear=%.0fus)\n",
            sd_total / ticks, sd_total, sd_read_total / ticks, sd_clear_total / ticks);
    fprintf(stderr, "(B) PROT_READ only (no faults)    | %10.0fus | %10.0fus  (best case for mprotect)\n",
            pr_total / ticks, pr_total);
    fprintf(stderr, "(C) PROT_READ + per-page mprotect | %10.0fus | %10.0fus  (Smash's actual path)\n",
            prf_total / ticks, prf_total);
    fprintf(stderr, "\nsoft-dirty vs (B) best-case mprotect:        %.2fx (>1 = soft-dirty wins)\n",
            pr_total / sd_total);
    fprintf(stderr, "soft-dirty vs (C) realistic Smash mprotect:  %.2fx (>1 = soft-dirty wins)\n",
            prf_total / sd_total);

    munmap(buf, kBatchPages * sizeof(uint64_t));
    munmap(base, pages * kPageSize);
    close(pagemap_fd);
    close(clear_refs_fd);
    return 0;
}
