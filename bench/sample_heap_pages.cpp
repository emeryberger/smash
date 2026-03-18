// sample_heap_pages.cpp - Read and compress heap pages from a running process
//
// Uses mach_vm_read to sample writable memory pages from a target PID,
// then compresses each with LZ4 and zstd to measure compressibility.
//
// Usage: sample_heap_pages <PID> [--label <name>]
//
// Requires: taskgw entitlement or SIP disabled for task_for_pid to work.
// Alternative: run as root, or use the self-sampling mode (PID 0 = self).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#endif

static const size_t kPageSize = 16384;

struct PageStats {
    double ratio_lz4;
    double ratio_zstd1;
    double ratio_zstd3;
    double ratio_zstd9;
};

static PageStats compressPage(const void* page) {
    PageStats s{};
    char comp_buf[kPageSize * 2];

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

struct AlgoStats {
    double avg;
    double median;
};

static AlgoStats computeStats(std::vector<double>& v) {
    if (v.empty()) return {1.0, 1.0};
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    return {sum / v.size(), v[v.size() / 2]};
}

#ifdef __APPLE__
static int sampleProcess(pid_t pid, const char* label) {
    mach_port_t task;
    kern_return_t kr;

    if (pid == 0) {
        task = mach_task_self();
    } else {
        kr = task_for_pid(mach_task_self(), pid, &task);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "task_for_pid(%d) failed: %s\n"
                    "  Try: sudo or disable SIP\n",
                    pid, mach_error_string(kr));
            return 1;
        }
    }

    // Enumerate VM regions
    std::vector<std::pair<mach_vm_address_t, mach_vm_size_t>> regions;
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;

    while (true) {
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj_name;

        kr = mach_vm_region(task, &addr, &size, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &count, &obj_name);
        if (kr != KERN_SUCCESS)
            break;

        // Only writable, non-shared regions (heap-like)
        bool writable = (info.protection & VM_PROT_WRITE) != 0;
        bool readable = (info.protection & VM_PROT_READ) != 0;
        if (writable && readable && !info.shared && size >= kPageSize) {
            regions.push_back({addr, size});
        }

        addr += size;
    }

    fprintf(stderr, "Found %zu writable regions in PID %d\n", regions.size(), pid);

    // Collect page addresses
    std::vector<mach_vm_address_t> page_addrs;
    for (auto& [raddr, rsize] : regions) {
        for (mach_vm_address_t p = raddr; p + kPageSize <= raddr + rsize; p += kPageSize) {
            page_addrs.push_back(p);
        }
    }

    fprintf(stderr, "Total potential pages: %zu\n", page_addrs.size());

    // Sample up to 10000 pages
    if (page_addrs.size() > 10000) {
        std::mt19937 rng{42};
        std::shuffle(page_addrs.begin(), page_addrs.end(), rng);
        page_addrs.resize(10000);
    }

    // Read and compress each page
    char page_buf[kPageSize];
    std::vector<double> r_lz4, r_zstd1, r_zstd3, r_zstd9;
    int read_failures = 0;

    for (mach_vm_address_t paddr : page_addrs) {
        mach_vm_size_t out_size = kPageSize;
        kr = mach_vm_read_overwrite(task, paddr, kPageSize,
                                     (mach_vm_address_t)page_buf, &out_size);
        if (kr != KERN_SUCCESS || out_size != kPageSize) {
            read_failures++;
            continue;
        }

        // Skip all-zero pages
        bool all_zero = true;
        auto* words = reinterpret_cast<const uint64_t*>(page_buf);
        for (size_t i = 0; i < kPageSize / 8; i++) {
            if (words[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) continue;

        PageStats s = compressPage(page_buf);
        r_lz4.push_back(s.ratio_lz4);
        r_zstd1.push_back(s.ratio_zstd1);
        r_zstd3.push_back(s.ratio_zstd3);
        r_zstd9.push_back(s.ratio_zstd9);
    }

    if (r_lz4.empty()) {
        fprintf(stderr, "No non-zero readable pages found\n");
        return 1;
    }

    fprintf(stderr, "Sampled %zu pages (%d read failures)\n",
            r_lz4.size(), read_failures);

    auto s_lz4 = computeStats(r_lz4);
    auto s_zstd1 = computeStats(r_zstd1);
    auto s_zstd3 = computeStats(r_zstd3);
    auto s_zstd9 = computeStats(r_zstd9);

    printf("METRIC app %s\n", label);
    printf("METRIC pages_sampled %zu\n", r_lz4.size());
    printf("METRIC avg_lz4 %.1f\n", s_lz4.avg * 100);
    printf("METRIC avg_zstd1 %.1f\n", s_zstd1.avg * 100);
    printf("METRIC avg_zstd3 %.1f\n", s_zstd3.avg * 100);
    printf("METRIC avg_zstd9 %.1f\n", s_zstd9.avg * 100);
    printf("METRIC median_lz4 %.1f\n", s_lz4.median * 100);
    printf("METRIC median_zstd1 %.1f\n", s_zstd1.median * 100);
    printf("METRIC median_zstd3 %.1f\n", s_zstd3.median * 100);
    printf("METRIC median_zstd9 %.1f\n", s_zstd9.median * 100);

    fprintf(stderr, "\n%-10s %8s %8s %8s %8s\n", "Metric", "LZ4", "zstd-1", "zstd-3", "zstd-9");
    fprintf(stderr, "%-10s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", "Average",
            s_lz4.avg*100, s_zstd1.avg*100, s_zstd3.avg*100, s_zstd9.avg*100);
    fprintf(stderr, "%-10s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", "Median",
            s_lz4.median*100, s_zstd1.median*100, s_zstd3.median*100, s_zstd9.median*100);

    return 0;
}
#endif

int main(int argc, char* argv[]) {
#ifndef __APPLE__
    fprintf(stderr, "Only supported on macOS\n");
    return 1;
#else
    pid_t pid = 0;
    const char* label = "unknown";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            label = argv[++i];
        else if (argv[i][0] != '-')
            pid = atoi(argv[i]);
        else {
            fprintf(stderr, "Usage: %s <PID> [--label <name>]\n", argv[0]);
            return 1;
        }
    }

    if (pid <= 0) {
        fprintf(stderr, "Usage: %s <PID> [--label <name>]\n", argv[0]);
        return 1;
    }

    return sampleProcess(pid, label);
#endif
}
