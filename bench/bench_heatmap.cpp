// bench_heatmap.cpp - Microbenchmark for coldness × compressibility heatmap
//
// Sweeps two axes:
//   1. Hot fraction: 0% (all cold) to 100% (all hot)
//   2. Compressibility: 0% (random) to 100% (zeros)
//
// Outputs JSON for generating a heatmap of Smash effectiveness.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif

// ── Configuration ───────────────────────────────────────────────────────────

static size_t g_total_mb = 512;        // Total data size in MB
static size_t g_chunk_size = 4096;     // Allocation size (one page)
static int g_cool_sec = 15;            // Cooling period
static int g_serve_sec = 10;           // Serve period (accessing hot data)
static bool g_json_output = false;
static bool g_quick = false;

// ── RSS measurement ─────────────────────────────────────────────────────────

static size_t getCurrentRSSBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#elif defined(__linux__)
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return 0;
    char buf[128];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    long sz_pages = 0, resident_pages = 0;
    if (sscanf(buf, "%ld %ld", &sz_pages, &resident_pages) >= 2) {
        return static_cast<size_t>(resident_pages) * 4096;
    }
    return 0;
#else
    return 0;
#endif
}

static double getRSSMB() {
    return getCurrentRSSBytes() / (1024.0 * 1024.0);
}

// ── Data generation with controlled compressibility ─────────────────────────

// Compressibility levels:
//   0%:   Pure random bytes (incompressible)
//   25%:  Random with 25% zeros
//   50%:  Alternating random/zero blocks
//   75%:  Mostly zeros with 25% random
//   100%: All zeros (maximally compressible)

static void fillChunk(void* ptr, size_t size, int compressibility_pct, std::mt19937& rng) {
    uint8_t* data = static_cast<uint8_t*>(ptr);

    if (compressibility_pct >= 100) {
        // All zeros
        memset(data, 0, size);
    } else if (compressibility_pct <= 0) {
        // Pure random
        for (size_t i = 0; i < size; i += 4) {
            uint32_t r = rng();
            memcpy(data + i, &r, std::min(size - i, size_t(4)));
        }
    } else {
        // Mix: first fill with zeros, then overwrite random fraction
        memset(data, 0, size);
        size_t random_bytes = size * (100 - compressibility_pct) / 100;

        // Scatter random bytes throughout the chunk
        std::uniform_int_distribution<size_t> pos_dist(0, size - 1);
        for (size_t i = 0; i < random_bytes; ++i) {
            size_t pos = pos_dist(rng);
            data[pos] = static_cast<uint8_t>(rng() & 0xFF);
        }
    }
}

// ── Benchmark runner ────────────────────────────────────────────────────────

struct BenchResult {
    int hot_pct;
    int compress_pct;
    double peak_rss_mb;
    double min_rss_mb;
    double steady_rss_mb;
    double auc_mb_sec;
    double reduction_pct;
};

static BenchResult runBenchmark(int hot_pct, int compress_pct) {
    BenchResult result = {hot_pct, compress_pct, 0, 0, 0, 0, 0};

    size_t num_chunks = (g_total_mb * 1024 * 1024) / g_chunk_size;
    std::vector<void*> chunks(num_chunks);

    std::mt19937 rng(42 + hot_pct * 1000 + compress_pct);  // Deterministic seed

    // Phase 1: Allocate and fill
    for (size_t i = 0; i < num_chunks; ++i) {
        chunks[i] = malloc(g_chunk_size);
        if (!chunks[i]) {
            fprintf(stderr, "malloc failed at chunk %zu\n", i);
            exit(1);
        }
        fillChunk(chunks[i], g_chunk_size, compress_pct, rng);
    }

    result.peak_rss_mb = getRSSMB();

    // Phase 2: Cool - let pages go cold
    double min_rss = result.peak_rss_mb;
    double auc = 0;
    for (int t = 0; t < g_cool_sec; ++t) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        double rss = getRSSMB();
        min_rss = std::min(min_rss, rss);
        auc += rss;
    }
    result.min_rss_mb = min_rss;

    // Phase 3: Serve - access hot fraction only
    size_t hot_start = num_chunks * (100 - hot_pct) / 100;  // Hot chunks at the end
    volatile uint8_t sink = 0;

    auto serve_start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - serve_start).count() < g_serve_sec) {
        // Access each hot chunk once per second
        for (size_t i = hot_start; i < num_chunks; ++i) {
            uint8_t* data = static_cast<uint8_t*>(chunks[i]);
            sink ^= data[0] ^ data[g_chunk_size/2] ^ data[g_chunk_size-1];
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double rss = getRSSMB();
        auc += rss * 0.1;  // 100ms intervals
    }

    result.steady_rss_mb = getRSSMB();
    result.auc_mb_sec = auc;

    if (result.peak_rss_mb > 0) {
        result.reduction_pct = 100.0 * (result.peak_rss_mb - result.min_rss_mb) / result.peak_rss_mb;
    }

    // Cleanup
    for (auto p : chunks) {
        free(p);
    }

    (void)sink;  // Prevent optimization
    return result;
}

// ── Main ────────────────────────────────────────────────────────────────────

static void printUsage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --size MB       Total data size (default: 512)\n");
    fprintf(stderr, "  --chunk BYTES   Chunk size (default: 4096)\n");
    fprintf(stderr, "  --cool SEC      Cooling period (default: 15)\n");
    fprintf(stderr, "  --serve SEC     Serve period (default: 10)\n");
    fprintf(stderr, "  --json          Output JSON format\n");
    fprintf(stderr, "  --quick         Quick mode (256MB, 5s cool, 5s serve)\n");
    fprintf(stderr, "  --single HOT,COMP  Run single point only\n");
}

int main(int argc, char** argv) {
    int single_hot = -1, single_comp = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            g_total_mb = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            g_chunk_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cool") == 0 && i + 1 < argc) {
            g_cool_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--serve") == 0 && i + 1 < argc) {
            g_serve_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            g_json_output = true;
        } else if (strcmp(argv[i], "--quick") == 0) {
            g_quick = true;
            g_total_mb = 256;
            g_cool_sec = 5;
            g_serve_sec = 5;
        } else if (strcmp(argv[i], "--single") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%d,%d", &single_hot, &single_comp);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Sweep parameters
    std::vector<int> hot_fractions = {0, 10, 25, 50, 75, 90, 100};
    std::vector<int> compressibilities = {0, 25, 50, 75, 100};

    if (single_hot >= 0 && single_comp >= 0) {
        hot_fractions = {single_hot};
        compressibilities = {single_comp};
    }

    std::vector<BenchResult> results;

    if (!g_json_output) {
        fprintf(stderr, "Heatmap benchmark: %zuMB data, %zu-byte chunks, %ds cool, %ds serve\n",
                g_total_mb, g_chunk_size, g_cool_sec, g_serve_sec);
        fprintf(stderr, "Running %zu x %zu = %zu configurations...\n\n",
                hot_fractions.size(), compressibilities.size(),
                hot_fractions.size() * compressibilities.size());
    }

    for (int compress_pct : compressibilities) {
        for (int hot_pct : hot_fractions) {
            if (!g_json_output) {
                fprintf(stderr, "  hot=%3d%% compress=%3d%% ... ", hot_pct, compress_pct);
                fflush(stderr);
            }

            BenchResult r = runBenchmark(hot_pct, compress_pct);
            results.push_back(r);

            if (!g_json_output) {
                fprintf(stderr, "peak=%.0fMB min=%.0fMB reduction=%.1f%%\n",
                        r.peak_rss_mb, r.min_rss_mb, r.reduction_pct);
            }
        }
    }

    // Output results
    if (g_json_output) {
        printf("{\n");
        printf("  \"config\": {\n");
        printf("    \"total_mb\": %zu,\n", g_total_mb);
        printf("    \"chunk_size\": %zu,\n", g_chunk_size);
        printf("    \"cool_sec\": %d,\n", g_cool_sec);
        printf("    \"serve_sec\": %d\n", g_serve_sec);
        printf("  },\n");
        printf("  \"results\": [\n");
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            printf("    {\"hot_pct\": %d, \"compress_pct\": %d, "
                   "\"peak_rss_mb\": %.1f, \"min_rss_mb\": %.1f, "
                   "\"steady_rss_mb\": %.1f, \"auc_mb_sec\": %.1f, "
                   "\"reduction_pct\": %.2f}%s\n",
                   r.hot_pct, r.compress_pct,
                   r.peak_rss_mb, r.min_rss_mb,
                   r.steady_rss_mb, r.auc_mb_sec,
                   r.reduction_pct,
                   (i + 1 < results.size()) ? "," : "");
        }
        printf("  ]\n");
        printf("}\n");
    } else {
        // Print ASCII heatmap
        printf("\nRSS Reduction Heatmap (%%)\n");
        printf("                    Compressibility\n");
        printf("Hot %%    ");
        for (int c : compressibilities) {
            printf("%6d%%", c);
        }
        printf("\n");
        printf("        ");
        for (size_t i = 0; i < compressibilities.size(); ++i) {
            printf("-------");
        }
        printf("\n");

        size_t idx = 0;
        for (int c : compressibilities) {
            for (int h : hot_fractions) {
                if (results[idx].compress_pct == c) {
                    if (results[idx].hot_pct == hot_fractions[0]) {
                        // First in row - print header would be here but we print by compress first
                    }
                }
                idx++;
            }
        }

        // Reorganize for display (hot on rows, compress on cols)
        for (int h : hot_fractions) {
            printf("%5d%%  |", h);
            for (int c : compressibilities) {
                // Find matching result
                for (const auto& r : results) {
                    if (r.hot_pct == h && r.compress_pct == c) {
                        printf("%6.1f%%", r.reduction_pct);
                        break;
                    }
                }
            }
            printf("\n");
        }

        printf("\nKey: Higher %% = more RSS reduction = Smash more effective\n");
        printf("     Expect high reduction when: low hot%% (cold data) AND high compress%%\n");
    }

    return 0;
}
