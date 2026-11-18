// Target process for test_smash_estimate: known idle and hot regions, each its
// own VMA (PROT_NONE gaps stop the kernel merging them), so every estimator
// method (per-page idle / soft-dirty, per-mapping referenced) can be checked.
//   cold_comp  64 MiB  compressible, written once, then never touched
//   cold_rand  16 MiB  random (incompressible), written once
//   hot_write  16 MiB  one byte per page rewritten every 50 ms
//   hot_read   16 MiB  one byte per page read every 50 ms, never rewritten
//   zero_read  16 MiB  read once, never written: every PTE maps the shared
//                      zero page (present in pagemap, not in RSS)
//   hot_grow   grows cold_comp's VMA by 512 KiB every 50 ms (10 MiB/s, like a
//              brk heap), each grown page rewritten every 50 ms: hot, and
//              Referenced in smaps although it was not present when the
//              estimate started (regression guard for the growth bias)
// argv[1] == "thp": MADV_HUGEPAGE on every region, so they are backed by
// transparent huge pages (2 MiB folios) unless THP is disabled.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t MiB = 1 << 20;
    const size_t sizes[5] = {64 * MiB, 16 * MiB, 16 * MiB, 16 * MiB, 16 * MiB};
    const size_t grow_max = 256 * MiB, grow_step = MiB / 2;
    size_t total = grow_max;
    for (size_t s : sizes) total += s + page;
    char* base = static_cast<char*>(mmap(nullptr, total, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    char* r[5];
    char* p = base;
    for (int i = 0; i < 5; ++i) {
        r[i] = p;
        p += sizes[i];
        // gap: separate VMAs; after cold_comp, the PROT_NONE reserve it grows into
        const size_t gap = i == 0 ? grow_max + page : page;
        mprotect(p, gap, PROT_NONE);
        p += gap;
    }
#ifdef MADV_HUGEPAGE
    if (argc > 1 && strcmp(argv[1], "thp") == 0)
        for (int i = 0; i < 4; ++i) madvise(r[i], sizes[i], MADV_HUGEPAGE);
    // zero_read stays 4 KiB pages: a read fault on a THP-enabled range would
    // install the huge zero page instead.
    madvise(r[4], sizes[4], MADV_NOHUGEPAGE);
#else
    (void)argc; (void)argv;
#endif
    char* grow = r[0] + sizes[0];
    size_t grown = 0;
    // compressible: repeating 64-byte records with a per-page key
    for (size_t i = 0; i < sizes[0]; ++i)
        r[0][i] = static_cast<char>((i % 64) < 8 ? (i / page) : (i % 64));
    uint64_t x = 88172645463325252ULL;
    for (int k = 1; k < 4; ++k)
        for (size_t i = 0; i < sizes[k]; i += 8) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            memcpy(r[k] + i, &x, 8);
        }
    volatile uint64_t sink = 0;
    for (size_t off = 0; off < sizes[4]; off += page) sink = sink + static_cast<unsigned char>(r[4][off]);
    for (unsigned gen = 0;; ++gen) {
        for (size_t off = 0; off < sizes[2]; off += page) r[2][off] = static_cast<char>(gen);
        for (size_t off = 0; off < sizes[3]; off += page) sink = sink + static_cast<unsigned char>(r[3][off]);
        if (grown + grow_step <= grow_max) {
            mprotect(grow + grown, grow_step, PROT_READ | PROT_WRITE);  // extends cold_comp's VMA
            for (size_t i = 0; i < grow_step; ++i)
                grow[grown + i] = static_cast<char>((i % 64) < 8 ? (i / page) : (i % 64));
            grown += grow_step;
        }
        for (size_t off = 0; off < grown; off += page) grow[off] = static_cast<char>(gen);
        usleep(50 * 1000);
    }
}
