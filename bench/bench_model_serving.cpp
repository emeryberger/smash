// bench_model_serving.cpp - ML model serving scenario
//
// Simulates multiple loaded models where only a subset actively serves traffic.
// Allocates N "models" (large contiguous buffers of structured float data),
// serves traffic to K active models (random reads within their buffers),
// lets the rest go cold, then measures RSS reduction and re-activation latency.
//
// Usage (via LD_PRELOAD):
//   LD_PRELOAD=./libsmash.so SMASH_COLD_TIMEOUT_SEC=1 ./bench_model_serving
//   ./bench_model_serving  # baseline (no compression)
//
// Reports: peak RSS, post-cool RSS, serve-phase ops/s, cold-model-access p50/p99

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

struct Model {
    float* weights;
    size_t num_params;
    size_t size_bytes;
};

int main(int argc, char** argv) {
    int num_models = 8;
    int active_models = 2;
    size_t model_size_mb = 128;  // per model
    int cool_sec = 15;
    int serve_sec = 10;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--models") == 0 && i + 1 < argc)
            num_models = atoi(argv[++i]);
        else if (strcmp(argv[i], "--active") == 0 && i + 1 < argc)
            active_models = atoi(argv[++i]);
        else if (strcmp(argv[i], "--model-size") == 0 && i + 1 < argc)
            model_size_mb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cool") == 0 && i + 1 < argc)
            cool_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--serve") == 0 && i + 1 < argc)
            serve_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--quick") == 0) {
            num_models = 4;
            model_size_mb = 32;
            cool_sec = 5;
            serve_sec = 5;
        }
    }

    size_t params_per_model = (model_size_mb * 1024 * 1024) / sizeof(float);
    size_t total_mb = (size_t)num_models * model_size_mb;

    fprintf(stdout, "=== ML Model Serving Benchmark ===\n");
    fprintf(stdout, "Models: %d × %zu MiB = %zu MiB total\n",
            num_models, model_size_mb, total_mb);
    fprintf(stdout, "Active models: %d, Cool: %ds, Serve: %ds\n\n",
            active_models, cool_sec, serve_sec);

    // Allocate models — fill with structured float data (realistic weight
    // patterns: small values near zero with occasional outliers, like a
    // normal distribution quantized to float16 range)
    std::vector<Model> models(num_models);
    std::mt19937 rng(42);
    std::normal_distribution<float> weight_dist(0.0f, 0.02f);

    for (int m = 0; m < num_models; ++m) {
        models[m].num_params = params_per_model;
        models[m].size_bytes = params_per_model * sizeof(float);
        models[m].weights = (float*)malloc(models[m].size_bytes);
        if (!models[m].weights) {
            fprintf(stderr, "Failed to allocate model %d (%zu MiB)\n",
                    m, model_size_mb);
            return 1;
        }
        for (size_t i = 0; i < params_per_model; ++i)
            models[m].weights[i] = weight_dist(rng);

        if ((m + 1) % 2 == 0 || m == num_models - 1)
            fprintf(stdout, "  Loaded %d/%d models...\n", m + 1, num_models);
    }

    double peak_rss_mb = getCurrentRSSBytes() / (1024.0 * 1024.0);
    fprintf(stdout, "\nPeak RSS after load: %.1f MiB\n", peak_rss_mb);

    // Cool phase: only access active_models, let the rest go cold
    fprintf(stdout, "\nCooling phase: %d seconds (only accessing models 0..%d)\n",
            cool_sec, active_models - 1);

    std::uniform_int_distribution<size_t> param_dist(0, params_per_model - 1);
    volatile float sink = 0;

    double cool_start = nowSec();
    double min_rss_mb = peak_rss_mb;

    while (nowSec() - cool_start < cool_sec) {
        // Touch active models to keep them hot
        for (int m = 0; m < active_models; ++m) {
            for (int j = 0; j < 1000; ++j) {
                size_t idx = param_dist(rng);
                sink += models[m].weights[idx];
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        double rss = getCurrentRSSBytes() / (1024.0 * 1024.0);
        min_rss_mb = std::min(min_rss_mb, rss);

        double elapsed = nowSec() - cool_start;
        if ((int)elapsed % 5 == 0 && elapsed - (int)elapsed < 0.15)
            fprintf(stdout, "  t=%ds: RSS=%.1f MiB\n", (int)elapsed, rss);
    }

    double post_cool_rss_mb = getCurrentRSSBytes() / (1024.0 * 1024.0);
    fprintf(stdout, "Post-cool RSS: %.1f MiB (min=%.1f MiB)\n",
            post_cool_rss_mb, min_rss_mb);

    // Serve phase: hot-set inference on active models only
    fprintf(stdout, "\nServe phase: %d seconds (inference on models 0..%d)\n",
            serve_sec, active_models - 1);

    size_t serve_ops = 0;
    double serve_start = nowSec();

    while (nowSec() - serve_start < serve_sec) {
        for (int m = 0; m < active_models; ++m) {
            // Simulate a forward pass: sequential read of a layer slice
            size_t layer_start = param_dist(rng);
            size_t layer_size = std::min((size_t)4096, params_per_model - layer_start);
            float sum = 0;
            for (size_t i = layer_start; i < layer_start + layer_size; ++i)
                sum += models[m].weights[i];
            sink += sum;
            serve_ops++;
        }
    }

    double serve_elapsed = nowSec() - serve_start;
    double serve_ops_per_sec = serve_ops / serve_elapsed;

    // Cold-model re-access: simulate traffic shift to a previously-cold model
    fprintf(stdout, "\nCold model re-access: reading model %d (was cold for %ds)\n",
            num_models - 1, cool_sec + serve_sec);

    std::vector<double> cold_latencies;
    cold_latencies.reserve(1000);
    int cold_model = num_models - 1;

    for (int i = 0; i < 1000; ++i) {
        size_t idx = param_dist(rng);
        auto t0 = std::chrono::steady_clock::now();
        sink += models[cold_model].weights[idx];
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        cold_latencies.push_back(us);
    }

    std::sort(cold_latencies.begin(), cold_latencies.end());
    double cold_p50 = cold_latencies[cold_latencies.size() / 2];
    double cold_p99 = cold_latencies[(size_t)(cold_latencies.size() * 0.99)];
    double cold_max = cold_latencies.back();

    double final_rss_mb = getCurrentRSSBytes() / (1024.0 * 1024.0);

    // Summary
    fprintf(stdout, "\n=== Results ===\n");
    fprintf(stdout, "Peak RSS: %.1f MiB\n", peak_rss_mb);
    fprintf(stdout, "Min RSS (during cool): %.1f MiB\n", min_rss_mb);
    fprintf(stdout, "RSS reduction: %.1f%%\n",
            (1.0 - min_rss_mb / peak_rss_mb) * 100.0);
    fprintf(stdout, "Serve ops/s: %.0f\n", serve_ops_per_sec);
    fprintf(stdout, "Cold-access p50: %.2f us\n", cold_p50);
    fprintf(stdout, "Cold-access p99: %.2f us\n", cold_p99);
    fprintf(stdout, "Cold-access max: %.2f us\n", cold_max);
    fprintf(stdout, "Final RSS (after re-access): %.1f MiB\n", final_rss_mb);

    // METRIC lines for automated parsing
    fprintf(stdout, "\nMETRIC peak_rss_mb %.1f\n", peak_rss_mb);
    fprintf(stdout, "METRIC min_rss_mb %.1f\n", min_rss_mb);
    fprintf(stdout, "METRIC rss_reduction_pct %.1f\n",
            (1.0 - min_rss_mb / peak_rss_mb) * 100.0);
    fprintf(stdout, "METRIC ops_per_sec %.0f\n", serve_ops_per_sec);
    fprintf(stdout, "METRIC cold_p50_us %.2f\n", cold_p50);
    fprintf(stdout, "METRIC cold_p99_us %.2f\n", cold_p99);
    fprintf(stdout, "METRIC cold_max_us %.2f\n", cold_max);

    // Cleanup
    for (auto& m : models)
        free(m.weights);

    (void)sink;
    return 0;
}
