// smash/src/compress_only/co_interpose_mac.cpp — macOS measurement-only
//
// Uses __DATA,__interpose to intercept malloc/free, tracks pages in a
// minimal hash table (no BootstrapAlloc), reports compression ratios at exit
// via mach_vm_read. Same design as the Linux version but using macOS APIs.

#ifdef __APPLE__

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>

#include <lz4.h>
#include <zstd.h>

// ── DYLD interpose infrastructure ───────────────────────────────────────────

typedef struct {
    void* replacement;
    void* original;
} co_interpose_t;

#define CO_INTERPOSE(replacement, original) \
    __attribute__((used)) \
    static const co_interpose_t co_ip_##replacement \
    __attribute__((section("__DATA, __interpose"))) = { \
        reinterpret_cast<void*>(replacement), \
        reinterpret_cast<void*>(original) \
    }

#define CO_ORIG(fn_type, wrapper) reinterpret_cast<fn_type>(co_ip_##wrapper.original)

// ── Page tracker (same as Linux version) ─────────────────────────────────────

static constexpr size_t kPageSize = 16384;  // macOS ARM64
static constexpr size_t kTrackCap = 2 * 1024 * 1024;

struct PageTracker {
    std::atomic<uintptr_t>* slots = nullptr;
    std::atomic<size_t> count{0};

    bool init() {
        void* mem = mmap(nullptr, kTrackCap * sizeof(std::atomic<uintptr_t>),
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return false;
        slots = static_cast<std::atomic<uintptr_t>*>(mem);
        return true;
    }

    void track(uintptr_t page_addr) {
        if (!slots || !page_addr) return;
        size_t idx = (page_addr >> 14) % kTrackCap;  // 14 = log2(16384)
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

using malloc_fn  = void*(*)(size_t);
using free_fn    = void(*)(void*);
using calloc_fn  = void*(*)(size_t, size_t);
using realloc_fn = void*(*)(void*, size_t);

static inline void track_pages(void* ptr, size_t size) {
    if (!ptr || !size || !g_inited.load(std::memory_order_relaxed)) return;
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~(uintptr_t)(kPageSize - 1);
    uintptr_t end = (reinterpret_cast<uintptr_t>(ptr) + size - 1) & ~(uintptr_t)(kPageSize - 1);
    for (uintptr_t p = start; p <= end; p += kPageSize)
        g_tracker.track(p);
}

// ── malloc/free/calloc/realloc wrappers ──────────────────────────────────────

extern "C" void* co_malloc(size_t size);
CO_INTERPOSE(co_malloc, malloc);
extern "C" void* co_malloc(size_t size) {
    void* ptr = CO_ORIG(malloc_fn, co_malloc)(size);
    track_pages(ptr, size);
    return ptr;
}

extern "C" void co_free(void* ptr);
CO_INTERPOSE(co_free, free);
extern "C" void co_free(void* ptr) {
    CO_ORIG(free_fn, co_free)(ptr);
}

extern "C" void* co_calloc(size_t count, size_t size);
CO_INTERPOSE(co_calloc, calloc);
extern "C" void* co_calloc(size_t count, size_t size) {
    void* ptr = CO_ORIG(calloc_fn, co_calloc)(count, size);
    track_pages(ptr, count * size);
    return ptr;
}

extern "C" void* co_realloc(void* old_ptr, size_t size);
CO_INTERPOSE(co_realloc, realloc);
extern "C" void* co_realloc(void* old_ptr, size_t size) {
    void* ptr = CO_ORIG(realloc_fn, co_realloc)(old_ptr, size);
    track_pages(ptr, size);
    return ptr;
}

using posix_memalign_fn = int(*)(void**, size_t, size_t);
using valloc_fn = void*(*)(size_t);

extern "C" int co_posix_memalign(void** memptr, size_t alignment, size_t size);
CO_INTERPOSE(co_posix_memalign, posix_memalign);
extern "C" int co_posix_memalign(void** memptr, size_t alignment, size_t size) {
    int ret = CO_ORIG(posix_memalign_fn, co_posix_memalign)(memptr, alignment, size);
    if (ret == 0 && *memptr) track_pages(*memptr, size);
    return ret;
}

extern "C" void* co_valloc(size_t size);
CO_INTERPOSE(co_valloc, valloc);
extern "C" void* co_valloc(size_t size) {
    void* ptr = CO_ORIG(valloc_fn, co_valloc)(size);
    track_pages(ptr, size);
    return ptr;
}

using reallocf_fn = void*(*)(void*, size_t);
using strdup_fn = char*(*)(const char*);

extern "C" void* co_reallocf(void* old_ptr, size_t size);
CO_INTERPOSE(co_reallocf, reallocf);
extern "C" void* co_reallocf(void* old_ptr, size_t size) {
    void* ptr = CO_ORIG(reallocf_fn, co_reallocf)(old_ptr, size);
    track_pages(ptr, size);
    return ptr;
}

extern "C" char* co_strdup(const char* s);
CO_INTERPOSE(co_strdup, strdup);
extern "C" char* co_strdup(const char* s) {
    char* ptr = CO_ORIG(strdup_fn, co_strdup)(s);
    if (ptr) track_pages(ptr, __builtin_strlen(s) + 1);
    return ptr;
}

// ── At-exit compression ratio report ─────────────────────────────────────────

static void report_ratios() {
    if (!g_tracker.slots) return;
    size_t tracked = g_tracker.count.load(std::memory_order_relaxed);
    if (tracked == 0) return;

    mach_port_t task = mach_task_self();
    int lz4_bound = LZ4_compressBound(kPageSize);
    size_t zstd_bound = ZSTD_compressBound(kPageSize);
    size_t comp_sz = ((size_t)lz4_bound > zstd_bound) ? (size_t)lz4_bound : zstd_bound;
    char* page_buf = (char*)mmap(nullptr, kPageSize, PROT_READ|PROT_WRITE,
                                  MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    char* comp_buf = (char*)mmap(nullptr, comp_sz, PROT_READ|PROT_WRITE,
                                  MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (page_buf == MAP_FAILED || comp_buf == MAP_FAILED) return;

    size_t pages_ok = 0, total_bytes = 0, lz4_bytes = 0, zstd1_bytes = 0;

    for (size_t i = 0; i < kTrackCap && pages_ok < 100000; ++i) {
        uintptr_t addr = g_tracker.slots[i].load(std::memory_order_relaxed);
        if (!addr) continue;

        // Read page via mach_vm_read
        vm_size_t out_size = 0;
        vm_offset_t data_out = 0;
        kern_return_t kr = vm_read(task, (vm_address_t)addr, kPageSize,
                                   &data_out, (mach_msg_type_number_t*)&out_size);
        if (kr != KERN_SUCCESS || out_size != kPageSize) continue;
        memcpy(page_buf, (void*)data_out, kPageSize);
        vm_deallocate(task, data_out, out_size);

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
    if (!g_tracker.init()) return;
    g_inited.store(true, std::memory_order_release);
}

__attribute__((destructor))
static void co_fini() {
    report_ratios();
}

#endif // __APPLE__
