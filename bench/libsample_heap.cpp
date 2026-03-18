// libsample_heap.cpp - Preloadable library that samples heap pages
//
// When loaded via DYLD_INSERT_LIBRARIES (macOS) or LD_PRELOAD (Linux),
// samples the process's own heap pages and prints compression statistics
// when SIGUSR2 is received.
//
// Usage:
//   # macOS:
//   DYLD_INSERT_LIBRARIES=./libsample_heap.dylib SAMPLE_LABEL=redis redis-server ...
//   # Linux:
//   LD_PRELOAD=./libsample_heap.so SAMPLE_LABEL=redis redis-server ...
//   # Then: kill -USR2 <pid>  to trigger sampling

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <vector>

#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

static const size_t kPageSize =
#if defined(__aarch64__) && defined(__APPLE__)
    16384;
#else
    4096;
#endif

struct PageStats {
    double ratio_lz4;
    double ratio_zstd1;
    double ratio_zstd3;
    double ratio_zstd9;
};

static PageStats compressPage(const void* page) {
    PageStats s{};
    // Use static buffer to avoid malloc in signal handler context
    static char comp_buf[kPageSize * 2];

    int lz4_sz = LZ4_compress_default(
        static_cast<const char*>(page), comp_buf,
        static_cast<int>(kPageSize), static_cast<int>(sizeof(comp_buf)));
    s.ratio_lz4 = lz4_sz > 0 ? static_cast<double>(lz4_sz) / kPageSize : 1.0;

    size_t z1 = ZSTD_compress(comp_buf, sizeof(comp_buf), page, kPageSize, 1);
    s.ratio_zstd1 = !ZSTD_isError(z1) ? static_cast<double>(z1) / kPageSize : 1.0;

    size_t z3 = ZSTD_compress(comp_buf, sizeof(comp_buf), page, kPageSize, 3);
    s.ratio_zstd3 = !ZSTD_isError(z3) ? static_cast<double>(z3) / kPageSize : 1.0;

    size_t z9 = ZSTD_compress(comp_buf, sizeof(comp_buf), page, kPageSize, 9);
    s.ratio_zstd9 = !ZSTD_isError(z9) ? static_cast<double>(z9) / kPageSize : 1.0;

    return s;
}

// ── Page enumeration ────────────────────────────────────────────────

#ifdef __APPLE__
static std::vector<uintptr_t> enumerateResidentPages() {
    std::vector<uintptr_t> pages;
    mach_port_t task = mach_task_self();
    vm_address_t addr = 0;
    vm_size_t size = 0;
    natural_t depth = 1;

    while (true) {
        struct vm_region_submap_info_64 info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;

        kern_return_t kr = vm_region_recurse_64(
            task, &addr, &size, &depth,
            (vm_region_info_64_t)&info, &count);
        if (kr != KERN_SUCCESS)
            break;

        if (info.is_submap) {
            depth++;
            continue;
        }

        bool writable = (info.protection & VM_PROT_WRITE) != 0;
        bool readable = (info.protection & VM_PROT_READ) != 0;
        if (writable && readable && size >= kPageSize) {
            for (vm_address_t p = addr; p + kPageSize <= addr + size; p += kPageSize) {
                char vec[1];
                if (mincore(reinterpret_cast<void*>(p), kPageSize, vec) == 0 && (vec[0] & 1)) {
                    pages.push_back(p);
                }
            }
        }
        addr += size;
    }
    return pages;
}
#else // Linux
static std::vector<uintptr_t> enumerateResidentPages() {
    std::vector<uintptr_t> pages;

    // Read /proc/self/maps using raw syscalls to avoid malloc
    // (we're called from a signal handler context)
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return pages;

    // Read entire maps file into static buffer
    static char buf[1024 * 1024];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';

    // Parse each line: start-end perms offset dev inode pathname
    char* line = buf;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        uintptr_t start = 0, end = 0;
        char perms[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3) {
            // Only include read-write anonymous/heap regions
            // (rw-p with no file, or [heap])
            bool rw = (perms[0] == 'r' && perms[1] == 'w');
            bool is_private = (perms[3] == 'p');
            size_t region_size = end - start;

            if (rw && is_private && region_size >= kPageSize) {
                // Check residency in batches
                size_t npages = region_size / kPageSize;
                // Limit batch size for mincore
                const size_t kBatch = 4096;
                for (size_t off = 0; off < npages; off += kBatch) {
                    size_t count = std::min(kBatch, npages - off);
                    unsigned char vec[kBatch];
                    uintptr_t base = start + off * kPageSize;
                    if (mincore(reinterpret_cast<void*>(base),
                                count * kPageSize, vec) == 0) {
                        for (size_t i = 0; i < count; i++) {
                            if (vec[i] & 1)
                                pages.push_back(base + i * kPageSize);
                        }
                    }
                }
            }
        }

        line = nl ? nl + 1 : nullptr;
    }
    return pages;
}
#endif

static void sampleAndReport(int) {
    const char* label = getenv("SAMPLE_LABEL");
    if (!label) label = "unknown";

    const char* outpath = getenv("SAMPLE_OUTPUT");
    FILE* out = outpath ? fopen(outpath, "w") : stderr;
    if (!out) out = stderr;

    std::vector<uintptr_t> pages = enumerateResidentPages();

    // Sample up to 10000
    if (pages.size() > 10000) {
        size_t step = pages.size() / 10000;
        std::vector<uintptr_t> sampled;
        for (size_t i = 0; i < pages.size(); i += step) {
            sampled.push_back(pages[i]);
            if (sampled.size() >= 10000) break;
        }
        pages = std::move(sampled);
    }

    std::vector<double> r_lz4, r_zstd1, r_zstd3, r_zstd9;

    for (uintptr_t paddr : pages) {
        const auto* p = reinterpret_cast<const uint64_t*>(paddr);
        bool all_zero = true;
        for (size_t i = 0; i < kPageSize / 8; i++) {
            if (p[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) continue;

        PageStats s = compressPage(reinterpret_cast<const void*>(paddr));
        r_lz4.push_back(s.ratio_lz4);
        r_zstd1.push_back(s.ratio_zstd1);
        r_zstd3.push_back(s.ratio_zstd3);
        r_zstd9.push_back(s.ratio_zstd9);
    }

    if (r_lz4.empty()) {
        fprintf(out, "METRIC app %s\nMETRIC error no_pages\n", label);
        if (out != stderr) fclose(out);
        return;
    }

    // Compute stats
    std::sort(r_lz4.begin(), r_lz4.end());
    std::sort(r_zstd1.begin(), r_zstd1.end());
    std::sort(r_zstd3.begin(), r_zstd3.end());
    std::sort(r_zstd9.begin(), r_zstd9.end());

    auto avg = [](const std::vector<double>& v) {
        double s = 0; for (double x : v) s += x; return s / v.size();
    };
    auto med = [](const std::vector<double>& v) { return v[v.size()/2]; };

    fprintf(out, "METRIC app %s\n", label);
    fprintf(out, "METRIC page_size %zu\n", kPageSize);
    fprintf(out, "METRIC pages_sampled %zu\n", r_lz4.size());
    fprintf(out, "METRIC avg_lz4 %.1f\n", avg(r_lz4) * 100);
    fprintf(out, "METRIC avg_zstd1 %.1f\n", avg(r_zstd1) * 100);
    fprintf(out, "METRIC avg_zstd3 %.1f\n", avg(r_zstd3) * 100);
    fprintf(out, "METRIC avg_zstd9 %.1f\n", avg(r_zstd9) * 100);
    fprintf(out, "METRIC median_lz4 %.1f\n", med(r_lz4) * 100);
    fprintf(out, "METRIC median_zstd1 %.1f\n", med(r_zstd1) * 100);
    fprintf(out, "METRIC median_zstd3 %.1f\n", med(r_zstd3) * 100);
    fprintf(out, "METRIC median_zstd9 %.1f\n", med(r_zstd9) * 100);

    fprintf(stderr, "\n[libsample_heap] %s: %zu pages sampled (page_size=%zu)\n",
            label, r_lz4.size(), kPageSize);
    fprintf(stderr, "%-10s %8s %8s %8s %8s\n", "Metric", "LZ4", "zstd-1", "zstd-3", "zstd-9");
    fprintf(stderr, "%-10s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", "Average",
            avg(r_lz4)*100, avg(r_zstd1)*100, avg(r_zstd3)*100, avg(r_zstd9)*100);
    fprintf(stderr, "%-10s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", "Median",
            med(r_lz4)*100, med(r_zstd1)*100, med(r_zstd3)*100, med(r_zstd9)*100);

    if (out != stderr) fclose(out);
}

__attribute__((constructor))
static void installHandler() {
    signal(SIGUSR2, sampleAndReport);
}
