// bench_alloc_speed.cpp — Multi-threaded allocation speed benchmarks.
// Standalone binary (no smash link); test any allocator via LD_PRELOAD.
//
// Implements three classic benchmarks:
//   threadtest: per-thread malloc/free of random sizes (Hoard paper)
//   larson:     producer-consumer pattern with cross-thread frees (Larson & Krishnan)
//   linux-scalability: simple parallel malloc/free throughput (Lever & Boreham)
//
// Usage:
//   ./bench_alloc_speed [--threads N] [--duration SEC] [--bench NAME]
//   LD_PRELOAD=libsmash.so ./bench_alloc_speed --bench all

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <random>
#include <thread>
#include <vector>

static int g_num_threads = 8;
static int g_duration_sec = 5;
static int g_iterations = 1000000;

// ── Threadtest (Hoard-style) ─────────────────────────────────────────────────
// Each thread allocates N objects of random sizes [8, 64K], then frees them,
// repeating for the duration. Measures per-thread throughput.

struct ThreadtestArg {
    int id;
    std::atomic<bool>* stop;
    std::atomic<long>* total_ops;
};

static void* threadtest_worker(void* arg) {
    auto* a = static_cast<ThreadtestArg*>(arg);
    std::mt19937 rng(a->id * 12345 + 67890);
    std::uniform_int_distribution<int> size_dist(8, 65536);
    constexpr int kBatch = 1000;
    void* ptrs[kBatch];
    long ops = 0;

    while (!a->stop->load(std::memory_order_relaxed)) {
        for (int i = 0; i < kBatch; ++i)
            ptrs[i] = malloc(size_dist(rng));
        for (int i = 0; i < kBatch; ++i)
            free(ptrs[i]);
        ops += kBatch * 2;
    }
    a->total_ops->fetch_add(ops, std::memory_order_relaxed);
    return nullptr;
}

static void run_threadtest() {
    std::atomic<bool> stop{false};
    std::atomic<long> total_ops{0};
    std::vector<pthread_t> threads(g_num_threads);
    std::vector<ThreadtestArg> args(g_num_threads);

    for (int i = 0; i < g_num_threads; ++i) {
        args[i] = {i, &stop, &total_ops};
        pthread_create(&threads[i], nullptr, threadtest_worker, &args[i]);
    }

    std::this_thread::sleep_for(std::chrono::seconds(g_duration_sec));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : threads)
        pthread_join(t, nullptr);

    double mops = total_ops.load() / 1e6;
    printf("  threadtest:         %6.1f Mops/s  (%d threads, %ds)\n",
           mops / g_duration_sec, g_num_threads, g_duration_sec);
    printf("METRIC threadtest_mops %.2f\n", mops / g_duration_sec);
}

// ── Larson (producer-consumer cross-thread free) ─────────────────────────────
// Half the threads produce (malloc), half consume (free). Objects are passed
// through a shared ring buffer. Stresses cross-thread free performance.

static constexpr int kLarsonRingSize = 4096;

struct LarsonShared {
    void* ring[kLarsonRingSize];
    std::atomic<int> head{0};
    std::atomic<int> tail{0};
    std::atomic<bool> stop{false};
    std::atomic<long> produce_ops{0};
    std::atomic<long> consume_ops{0};
};

static void* larson_producer(void* arg) {
    auto* s = static_cast<LarsonShared*>(arg);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> size_dist(8, 4096);
    long ops = 0;

    while (!s->stop.load(std::memory_order_relaxed)) {
        int next = (s->head.load(std::memory_order_relaxed) + 1) % kLarsonRingSize;
        if (next == s->tail.load(std::memory_order_acquire))
            continue;  // ring full
        s->ring[s->head.load(std::memory_order_relaxed)] = malloc(size_dist(rng));
        s->head.store(next, std::memory_order_release);
        ops++;
    }
    s->produce_ops.fetch_add(ops, std::memory_order_relaxed);
    return nullptr;
}

static void* larson_consumer(void* arg) {
    auto* s = static_cast<LarsonShared*>(arg);
    long ops = 0;

    while (!s->stop.load(std::memory_order_relaxed)) {
        int t = s->tail.load(std::memory_order_relaxed);
        if (t == s->head.load(std::memory_order_acquire))
            continue;  // ring empty
        free(s->ring[t]);
        s->tail.store((t + 1) % kLarsonRingSize, std::memory_order_release);
        ops++;
    }
    // Drain remaining
    while (s->tail.load(std::memory_order_relaxed) != s->head.load(std::memory_order_acquire)) {
        int t = s->tail.load(std::memory_order_relaxed);
        free(s->ring[t]);
        s->tail.store((t + 1) % kLarsonRingSize, std::memory_order_release);
    }
    s->consume_ops.fetch_add(ops, std::memory_order_relaxed);
    return nullptr;
}

static void run_larson() {
    int n_producers = g_num_threads / 2;
    int n_consumers = g_num_threads - n_producers;
    if (n_producers < 1) n_producers = 1;
    if (n_consumers < 1) n_consumers = 1;

    LarsonShared shared{};
    std::vector<pthread_t> threads(n_producers + n_consumers);

    for (int i = 0; i < n_producers; ++i)
        pthread_create(&threads[i], nullptr, larson_producer, &shared);
    for (int i = 0; i < n_consumers; ++i)
        pthread_create(&threads[n_producers + i], nullptr, larson_consumer, &shared);

    std::this_thread::sleep_for(std::chrono::seconds(g_duration_sec));
    shared.stop.store(true, std::memory_order_relaxed);

    for (auto& t : threads)
        pthread_join(t, nullptr);

    double mops = (shared.produce_ops.load() + shared.consume_ops.load()) / 1e6;
    printf("  larson:             %6.1f Mops/s  (%d prod + %d cons, %ds)\n",
           mops / g_duration_sec, n_producers, n_consumers, g_duration_sec);
    printf("METRIC larson_mops %.2f\n", mops / g_duration_sec);
}

// ── Linux-Scalability (simple parallel throughput) ────────────────────────────
// Each thread does N malloc+free of a fixed size. Measures raw scalability
// with no cross-thread interaction.

struct ScalabilityArg {
    int id;
    int iterations;
    long ops;
};

static void* scalability_worker(void* arg) {
    auto* a = static_cast<ScalabilityArg*>(arg);
    for (int i = 0; i < a->iterations; ++i) {
        void* p = malloc(64);
        free(p);
    }
    a->ops = a->iterations * 2L;
    return nullptr;
}

static void run_scalability() {
    int per_thread = g_iterations;
    std::vector<pthread_t> threads(g_num_threads);
    std::vector<ScalabilityArg> args(g_num_threads);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < g_num_threads; ++i) {
        args[i] = {i, per_thread, 0};
        pthread_create(&threads[i], nullptr, scalability_worker, &args[i]);
    }
    for (auto& t : threads)
        pthread_join(t, nullptr);

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    long total_ops = 0;
    for (auto& a : args)
        total_ops += a.ops;
    double mops = total_ops / 1e6 / elapsed;
    printf("  linux-scalability:  %6.1f Mops/s  (%d threads, %d iters/thread)\n",
           mops, g_num_threads, per_thread);
    printf("METRIC scalability_mops %.2f\n", mops);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* bench = "all";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            g_num_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            g_duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
            g_iterations = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
            bench = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--threads N] [--duration SEC] [--iterations N] [--bench threadtest|larson|scalability|all]\n", argv[0]);
            return 0;
        }
    }

    printf("=== Allocation Speed Benchmark ===\n");
    printf("Threads: %d, Duration: %ds, Iterations: %d\n\n", g_num_threads, g_duration_sec, g_iterations);

    if (strcmp(bench, "all") == 0 || strcmp(bench, "threadtest") == 0)
        run_threadtest();
    if (strcmp(bench, "all") == 0 || strcmp(bench, "larson") == 0)
        run_larson();
    if (strcmp(bench, "all") == 0 || strcmp(bench, "scalability") == 0)
        run_scalability();

    return 0;
}
