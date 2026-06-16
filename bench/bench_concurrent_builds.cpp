// bench_concurrent_builds.cpp - Multi-tenant build host scenario
//
// Simulates concurrent compilation jobs that each allocate a large heap
// (representing IR/AST data), work on a hot subset, then go idle while
// later phases run. Measures total RSS across all "jobs" and the latency
// when cold IR pages are re-accessed (link phase touching earlier compilation
// outputs).
//
// Usage (via LD_PRELOAD):
//   LD_PRELOAD=./libsmash.so SMASH_COLD_TIMEOUT_SEC=1 ./bench_concurrent_builds
//   ./bench_concurrent_builds  # baseline
//
// Reports: total peak RSS, post-cool RSS, cold-access latency per job

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <algorithm>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#endif

static size_t getCurrentRSSBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
#elif defined(__linux__)
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char* p = strstr(buf, "VmRSS:");
    if (!p) return 0;
    size_t kb = 0;
    sscanf(p, "VmRSS: %zu kB", &kb);
    return kb * 1024;
#else
    return 0;
#endif
}

static double nowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Simulated compiler IR: structured data with repetitive patterns
// (type tags, instruction opcodes, register indices — highly compressible)
struct IRNode {
    uint32_t opcode;       // limited set of values (0-255)
    uint32_t type_tag;     // limited set (0-63)
    uint32_t operand[4];   // register indices (0-1023)
    uint32_t source_loc;   // line number
    uint32_t flags;        // sparse
};
static_assert(sizeof(IRNode) == 32, "");

struct BuildJob {
    IRNode* ir_buffer;
    size_t num_nodes;
    size_t size_bytes;
    size_t hot_start;  // active working region during "compile" phase
    size_t hot_count;
};

int main(int argc, char** argv) {
    int num_jobs = 8;
    size_t job_size_mb = 128;  // per job
    double hot_fraction = 0.15;  // 15% of each job is the "active compilation unit"
    int cool_sec = 15;
    int work_sec = 10;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc)
            num_jobs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--job-size") == 0 && i + 1 < argc)
            job_size_mb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cool") == 0 && i + 1 < argc)
            cool_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--work") == 0 && i + 1 < argc)
            work_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--quick") == 0) {
            num_jobs = 4;
            job_size_mb = 64;
            cool_sec = 5;
            work_sec = 5;
        }
    }

    size_t nodes_per_job = (job_size_mb * 1024 * 1024) / sizeof(IRNode);
    size_t total_mb = (size_t)num_jobs * job_size_mb;

    fprintf(stdout, "=== Concurrent Builds Benchmark ===\n");
    fprintf(stdout, "Jobs: %d × %zu MiB = %zu MiB total\n",
            num_jobs, job_size_mb, total_mb);
    fprintf(stdout, "Hot fraction: %.0f%% per job, Cool: %ds, Work: %ds\n\n",
            hot_fraction * 100, cool_sec, work_sec);

    // Phase 1: "Parsing" — allocate all jobs (simulates frontend output)
    std::vector<BuildJob> jobs(num_jobs);
    std::mt19937 rng(12345);

    for (int j = 0; j < num_jobs; ++j) {
        jobs[j].num_nodes = nodes_per_job;
        jobs[j].size_bytes = nodes_per_job * sizeof(IRNode);
        jobs[j].ir_buffer = (IRNode*)malloc(jobs[j].size_bytes);
        if (!jobs[j].ir_buffer) {
            fprintf(stderr, "Failed to allocate job %d (%zu MiB)\n", j, job_size_mb);
            return 1;
        }

        // Fill with realistic IR patterns
        std::uniform_int_distribution<uint32_t> opcode_dist(0, 127);
        std::uniform_int_distribution<uint32_t> type_dist(0, 31);
        std::uniform_int_distribution<uint32_t> reg_dist(0, 511);

        for (size_t i = 0; i < nodes_per_job; ++i) {
            jobs[j].ir_buffer[i].opcode = opcode_dist(rng);
            jobs[j].ir_buffer[i].type_tag = type_dist(rng);
            for (int k = 0; k < 4; ++k)
                jobs[j].ir_buffer[i].operand[k] = reg_dist(rng);
            jobs[j].ir_buffer[i].source_loc = (uint32_t)(i / 10);  // monotonic
            jobs[j].ir_buffer[i].flags = 0;
        }

        // Each job's hot region is a different slice (simulates different
        // compilation units being optimized concurrently)
        jobs[j].hot_start = (size_t)(j * hot_fraction * nodes_per_job) % nodes_per_job;
        jobs[j].hot_count = (size_t)(hot_fraction * nodes_per_job);

        if ((j + 1) % 2 == 0 || j == num_jobs - 1)
            fprintf(stdout, "  Allocated %d/%d jobs...\n", j + 1, num_jobs);
    }

    double peak_rss_mb = getCurrentRSSBytes() / (1024.0 * 1024.0);
    fprintf(stdout, "\nPeak RSS after allocation: %.1f MiB\n", peak_rss_mb);

    // Phase 2: "Optimization" — each job works only on its hot slice
    // (simulating per-function optimization passes that don't touch the
    // whole IR). The cold 85% of each job should compress.
    fprintf(stdout, "\nWork phase: %d seconds (each job touches %.0f%% of its IR)\n",
            work_sec, hot_fraction * 100);

    volatile uint32_t sink = 0;
    size_t total_ops = 0;
    double work_start = nowSec();
    double min_rss_mb = peak_rss_mb;

    while (nowSec() - work_start < work_sec) {
        for (int j = 0; j < num_jobs; ++j) {
            // Simulate optimization: read and modify nodes in the hot slice
            for (int iter = 0; iter < 100; ++iter) {
                size_t idx = jobs[j].hot_start +
                    (rng() % jobs[j].hot_count);
                IRNode& node = jobs[j].ir_buffer[idx];
                sink += node.opcode + node.operand[0];
                node.flags |= 1;  // write to keep it hot
                total_ops++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        double rss = getCurrentRSSBytes() / (1024.0 * 1024.0);
        min_rss_mb = std::min(min_rss_mb, rss);
    }

    double work_elapsed = nowSec() - work_start;
    double ops_per_sec = total_ops / work_elapsed;

    // Phase 3: "Cooling" — all jobs idle (simulates waiting for link phase)
    fprintf(stdout, "\nCooling phase: %d seconds (all jobs idle)\n", cool_sec);

    double cool_start = nowSec();
    while (nowSec() - cool_start < cool_sec) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        double rss = getCurrentRSSBytes() / (1024.0 * 1024.0);
        min_rss_mb = std::min(min_rss_mb, rss);

        double elapsed = nowSec() - cool_start;
        if ((int)elapsed % 5 == 0 && elapsed - (int)elapsed < 0.55)
            fprintf(stdout, "  t=%ds: RSS=%.1f MiB\n", (int)elapsed, rss);
    }

    double post_cool_rss_mb = getCurrentRSSBytes() / (1024.0 * 1024.0);
    fprintf(stdout, "Post-cool RSS: %.1f MiB (min=%.1f MiB)\n",
            post_cool_rss_mb, min_rss_mb);

    // Phase 4: "Link" — re-access cold IR from all jobs (simulates linker
    // reading symbol tables and relocations from all compilation units)
    fprintf(stdout, "\nLink phase: re-accessing cold regions from all %d jobs\n",
            num_jobs);

    std::vector<double> cold_latencies;
    cold_latencies.reserve(num_jobs * 500);

    for (int j = 0; j < num_jobs; ++j) {
        // Access cold region (NOT the hot slice)
        size_t cold_start = (jobs[j].hot_start + jobs[j].hot_count) % jobs[j].num_nodes;
        for (int i = 0; i < 500; ++i) {
            size_t idx = cold_start + (rng() % (jobs[j].num_nodes - jobs[j].hot_count));
            idx = idx % jobs[j].num_nodes;

            auto t0 = std::chrono::steady_clock::now();
            sink += jobs[j].ir_buffer[idx].opcode;
            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            cold_latencies.push_back(us);
        }
    }

    std::sort(cold_latencies.begin(), cold_latencies.end());
    double cold_p50 = cold_latencies[cold_latencies.size() / 2];
    double cold_p99 = cold_latencies[(size_t)(cold_latencies.size() * 0.99)];
    double cold_max = cold_latencies.back();

    double final_rss_mb = getCurrentRSSBytes() / (1024.0 * 1024.0);

    // Summary
    fprintf(stdout, "\n=== Results ===\n");
    fprintf(stdout, "Total allocated: %zu MiB (%d jobs × %zu MiB)\n",
            total_mb, num_jobs, job_size_mb);
    fprintf(stdout, "Peak RSS: %.1f MiB\n", peak_rss_mb);
    fprintf(stdout, "Min RSS (work+cool): %.1f MiB\n", min_rss_mb);
    fprintf(stdout, "RSS reduction: %.1f%%\n",
            (1.0 - min_rss_mb / peak_rss_mb) * 100.0);
    fprintf(stdout, "Work-phase ops/s: %.0f\n", ops_per_sec);
    fprintf(stdout, "Link cold-access p50: %.2f us\n", cold_p50);
    fprintf(stdout, "Link cold-access p99: %.2f us\n", cold_p99);
    fprintf(stdout, "Link cold-access max: %.2f us\n", cold_max);
    fprintf(stdout, "Final RSS (after link): %.1f MiB\n", final_rss_mb);

    // METRIC lines
    fprintf(stdout, "\nMETRIC peak_rss_mb %.1f\n", peak_rss_mb);
    fprintf(stdout, "METRIC min_rss_mb %.1f\n", min_rss_mb);
    fprintf(stdout, "METRIC rss_reduction_pct %.1f\n",
            (1.0 - min_rss_mb / peak_rss_mb) * 100.0);
    fprintf(stdout, "METRIC ops_per_sec %.0f\n", ops_per_sec);
    fprintf(stdout, "METRIC cold_p50_us %.2f\n", cold_p50);
    fprintf(stdout, "METRIC cold_p99_us %.2f\n", cold_p99);
    fprintf(stdout, "METRIC cold_max_us %.2f\n", cold_max);

    // Cleanup
    for (auto& j : jobs)
        free(j.ir_buffer);

    (void)sink;
    return 0;
}
