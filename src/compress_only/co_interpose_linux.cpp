// smash/src/compress_only/co_interpose_linux.cpp — Linux LD_PRELOAD measurement
//
// Minimal implementation: tracks pages via raw syscall-allocated hash,
// reports compression ratios at exit. No BootstrapAlloc, no VmRegion,
// no mprotect — avoids all bootstrapping recursion.

#ifndef __APPLE__

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <lz4.h>
#include <zstd.h>

// ── Raw mmap (bypasses our interposition) ────────────────────────────────────

static void* raw_mmap(size_t size) {
    long r = syscall(SYS_mmap, nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, (off_t)0);
    return (r < 0 && r > -4096) ? MAP_FAILED : (void*)r;
}

// ── Page tracker: open-addressed hash set ────────────────────────────────────

static constexpr size_t kPageSize = 4096;
static constexpr size_t kTrackCap = 2 * 1024 * 1024;  // 2M slots (16 MiB)

struct PageTracker {
    std::atomic<uintptr_t>* slots = nullptr;
    std::atomic<size_t> count{0};

    bool init() {
        void* mem = raw_mmap(kTrackCap * sizeof(std::atomic<uintptr_t>));
        if (mem == MAP_FAILED) return false;
        slots = static_cast<std::atomic<uintptr_t>*>(mem);
        return true;
    }

    void track(uintptr_t page_addr) {
        if (!slots || !page_addr) return;
        size_t idx = (page_addr >> 12) % kTrackCap;
        for (size_t probe = 0; probe < 128; ++probe) {
            size_t i = (idx + probe) % kTrackCap;
            uintptr_t cur = slots[i].load(std::memory_order_relaxed);
            if (cur == page_addr) return;
            if (cur == 0) {
                uintptr_t expected = 0;
                if (slots[i].compare_exchange_weak(expected, page_addr,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (expected == page_addr) return;
            }
        }
    }
};

// ── Globals ──────────────────────────────────────────────────────────────────

static PageTracker g_tracker;
static std::atomic<bool> g_inited{false};
static std::atomic<bool> g_in_dlsym{false};

static char g_calloc_buf[8192];
static std::atomic<size_t> g_calloc_off{0};

using malloc_fn  = void*(*)(size_t);
using free_fn    = void(*)(void*);
using calloc_fn  = void*(*)(size_t, size_t);
using realloc_fn = void*(*)(void*, size_t);

static malloc_fn  g_real_malloc  = nullptr;
static free_fn    g_real_free    = nullptr;
static calloc_fn  g_real_calloc  = nullptr;
static realloc_fn g_real_realloc = nullptr;

static void resolve() {
    g_in_dlsym.store(true, std::memory_order_relaxed);
    g_real_malloc  = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
    g_real_free    = (free_fn)dlsym(RTLD_NEXT, "free");
    g_real_calloc  = (calloc_fn)dlsym(RTLD_NEXT, "calloc");
    g_real_realloc = (realloc_fn)dlsym(RTLD_NEXT, "realloc");
    g_in_dlsym.store(false, std::memory_order_relaxed);
}

static inline void track_pages(void* ptr, size_t size) {
    if (!ptr || !size || !g_inited.load(std::memory_order_relaxed)) return;
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~(uintptr_t)(kPageSize - 1);
    uintptr_t end = (reinterpret_cast<uintptr_t>(ptr) + size - 1) & ~(uintptr_t)(kPageSize - 1);
    for (uintptr_t p = start; p <= end; p += kPageSize)
        g_tracker.track(p);
}

// ── Interposition ────────────────────────────────────────────────────────────

extern "C" void* malloc(size_t size) {
    if (!g_real_malloc) resolve();
    void* ptr = g_real_malloc(size);
    track_pages(ptr, size);
    return ptr;
}

extern "C" void free(void* ptr) {
    if (ptr >= g_calloc_buf && ptr < g_calloc_buf + sizeof(g_calloc_buf)) return;
    if (!g_real_free) resolve();
    g_real_free(ptr);
}

extern "C" void* calloc(size_t count, size_t size) {
    if (g_in_dlsym.load(std::memory_order_relaxed) || !g_real_calloc) {
        size_t total = count * size;
        size_t off = g_calloc_off.fetch_add(total, std::memory_order_relaxed);
        if (off + total <= sizeof(g_calloc_buf)) {
            __builtin_memset(g_calloc_buf + off, 0, total);
            return g_calloc_buf + off;
        }
        return nullptr;
    }
    void* ptr = g_real_calloc(count, size);
    track_pages(ptr, count * size);
    return ptr;
}

extern "C" void* realloc(void* old, size_t size) {
    if (!g_real_realloc) resolve();
    void* ptr = g_real_realloc(old, size);
    track_pages(ptr, size);
    return ptr;
}

// ── At-exit report ───────────────────────────────────────────────────────────

static void report_ratios() {
    if (!g_tracker.slots) return;
    size_t tracked = g_tracker.count.load(std::memory_order_relaxed);
    if (tracked == 0) return;

    int mem_fd = open("/proc/self/mem", O_RDONLY);
    if (mem_fd < 0) return;

    int lz4_bound = LZ4_compressBound(kPageSize);
    size_t zstd_bound = ZSTD_compressBound(kPageSize);
    size_t comp_sz = ((size_t)lz4_bound > zstd_bound) ? (size_t)lz4_bound : zstd_bound;
    char* page_buf = (char*)raw_mmap(kPageSize);
    char* comp_buf = (char*)raw_mmap(comp_sz);
    if (page_buf == MAP_FAILED || comp_buf == MAP_FAILED) { close(mem_fd); return; }

    size_t pages_ok = 0, total_bytes = 0, lz4_bytes = 0, zstd1_bytes = 0;

    for (size_t i = 0; i < kTrackCap && pages_ok < 100000; ++i) {
        uintptr_t addr = g_tracker.slots[i].load(std::memory_order_relaxed);
        if (!addr) continue;

        ssize_t rd = pread(mem_fd, page_buf, kPageSize, (off_t)addr);
        if (rd != (ssize_t)kPageSize) continue;

        // Skip all-zero pages
        bool zero = true;
        for (size_t j = 0; j < kPageSize; j += 8)
            if (*(uint64_t*)(page_buf + j)) { zero = false; break; }
        if (zero) continue;

        pages_ok++;
        total_bytes += kPageSize;

        int lz4_sz = LZ4_compress_default(page_buf, comp_buf, kPageSize, lz4_bound);
        lz4_bytes += (lz4_sz > 0) ? (size_t)lz4_sz : kPageSize;

        size_t z1 = ZSTD_compress(comp_buf, zstd_bound, page_buf, kPageSize, 1);
        zstd1_bytes += ZSTD_isError(z1) ? kPageSize : z1;
    }

    munmap(page_buf, kPageSize);
    munmap(comp_buf, comp_sz);
    close(mem_fd);

    if (total_bytes > 0) {
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
            "[compress-only] tracked=%zu nonzero=%zu data=%.1fMiB "
            "lz4=%.2fx zstd1=%.2fx\n",
            tracked, pages_ok, total_bytes / (1024.0 * 1024.0),
            (double)total_bytes / lz4_bytes,
            (double)total_bytes / zstd1_bytes);
        (void)!write(STDERR_FILENO, msg, n);
    }
}

// ── Init/fini ────────────────────────────────────────────────────────────────

__attribute__((constructor))
static void co_init() {
    resolve();
    if (!g_tracker.init()) {
        const char m[] = "[co] tracker init FAILED\n";
        (void)!write(STDERR_FILENO, m, sizeof(m)-1);
        return;
    }
    g_inited.store(true, std::memory_order_release);
    const char m[] = "[co] init OK\n";
    (void)!write(STDERR_FILENO, m, sizeof(m)-1);
}

__attribute__((destructor))
static void co_fini() {
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "[co] fini tracked=%zu\n",
                     g_tracker.count.load(std::memory_order_relaxed));
    (void)!write(STDERR_FILENO, msg, n);
    report_ratios();
}

#endif // !__APPLE__
