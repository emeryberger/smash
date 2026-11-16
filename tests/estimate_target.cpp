// Target process for test_smash_estimate: known idle and hot regions, each its
// own VMA (PROT_NONE gaps stop the kernel merging them), so every estimator
// method (per-page idle / soft-dirty, per-mapping referenced) can be checked.
//   cold_comp  64 MiB  compressible, written once, then never touched
//   cold_rand  16 MiB  random (incompressible), written once
//   hot_write  16 MiB  one byte per page rewritten every 50 ms
//   hot_read   16 MiB  one byte per page read every 50 ms, never rewritten
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t MiB = 1 << 20;
    const size_t sizes[4] = {64 * MiB, 16 * MiB, 16 * MiB, 16 * MiB};
    size_t total = 0;
    for (size_t s : sizes) total += s + page;
    char* base = static_cast<char*>(mmap(nullptr, total, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    char* r[4];
    char* p = base;
    for (int i = 0; i < 4; ++i) {
        r[i] = p;
        p += sizes[i];
        mprotect(p, page, PROT_NONE);  // gap: separate VMAs
        p += page;
    }
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
    for (unsigned gen = 0;; ++gen) {
        for (size_t off = 0; off < sizes[2]; off += page) r[2][off] = static_cast<char>(gen);
        for (size_t off = 0; off < sizes[3]; off += page) sink = sink + static_cast<unsigned char>(r[3][off]);
        usleep(50 * 1000);
    }
}
