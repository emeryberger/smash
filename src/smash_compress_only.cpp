// smash/src/smash_compress_only.cpp - Compression-only interposition library
//
// Interposes malloc/free and forwards to the REAL system allocator.
// Tracks allocated pages and runs Smash's compression pipeline on them.
// This lets us measure Smash compression effectiveness when the underlying
// allocator is NOT Smash's slab allocator (no external metadata, no arenas,
// no zero-on-free).
//
// Build with -DSMASH_COMPRESS_ONLY=1. Links alloc8::headers (not interpose).
// macOS: produces libsmash_compress_only.dylib (uses __DATA,__interpose)
// Linux: produces libsmash_compress_only.so (uses LD_PRELOAD + dlsym)

#include "smash/config.h"
#include "core/bootstrap_alloc.h"
#include "vm/vm_region.h"
#include "vm/page_state.h"
#include "vm/platform_mem.h"
#include "vm/fault_handler.h"
#include "vm/syscall_compat.h"
#include "compress/compress_store.h"
#include "compress/compress_engine.h"
#include "compress/compressor_thread.h"
#include "util/spinlock.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <mach/mach.h>
#else
#include <dlfcn.h>
#include <sys/epoll.h>
#include <cstdio>
#endif

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <lz4.h>
#include <zstd.h>

// ── Platform-specific interposition macros ────────────────────────────────────

#ifdef __APPLE__

// macOS: DYLD __DATA,__interpose — replaces function across all dylibs
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

// Access the original function pointer via the interpose struct
#define CO_ORIG(fn_type, wrapper) reinterpret_cast<fn_type>(co_ip_##wrapper.original)

#else

// Linux: LD_PRELOAD — use dlsym(RTLD_NEXT) to get the real function
// Functions are exported with the original name (no co_ prefix in the symbol table).
// The CO_INTERPOSE macro is a no-op; originals are resolved lazily via dlsym.

#define CO_INTERPOSE(replacement, original) /* no-op on Linux */

// Lazy-init original function pointer via dlsym(RTLD_NEXT, name)
#define CO_ORIG_DECL(fn_type, name) \
    static fn_type orig_##name = nullptr; \
    if (!orig_##name) { \
        orig_##name = reinterpret_cast<fn_type>(dlsym(RTLD_NEXT, #name)); \
    }

#endif

// ── Global compression state ────────────────────────────────────────────────

namespace {

smash::VmRegion g_vm;
smash::PageStateTable g_states;
smash::PageLockTable g_locks;
smash::CompressStore g_store;
smash::CompressEngine g_engine;
smash::CompressorThread g_compressor;
smash::vm::FaultHandler g_fault_handler;

std::atomic<bool> g_inited{false};
std::atomic<bool> g_compression_started{false};
std::atomic<int> g_warmup_count{0};

std::atomic<size_t> g_track_count{0};
std::atomic<size_t> g_malloc_count{0};
std::atomic<size_t> g_malloc_bytes{0};

bool faultCallback(uintptr_t fault_addr, void* /*ctx*/) {
    return g_compressor.handleFault(fault_addr);
}

void scanVmRegions();  // forward declaration

// SIGUSR2: set flag for the measurement thread to report ratios.
std::atomic<bool> g_report_requested{false};

void sigusr2Handler(int) {
    g_report_requested.store(true, std::memory_order_relaxed);
}

// Measurement thread: waits for SIGUSR2 flag, then scans all tracked pages
// via /proc/self/mem and reports compression ratios to stderr.
void* measurementThread(void*) {
    // Pre-allocate buffers
    int lz4_bound = LZ4_compressBound(smash::kPageSize);
    size_t zstd_bound = ZSTD_compressBound(smash::kPageSize);
    size_t buf_sz = (size_t)lz4_bound > zstd_bound ? (size_t)lz4_bound : zstd_bound;
    char* page_buf = (char*)::mmap(nullptr, smash::kPageSize, PROT_READ|PROT_WRITE,
                                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    char* comp_buf = (char*)::mmap(nullptr, buf_sz, PROT_READ|PROT_WRITE,
                                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (page_buf == MAP_FAILED || comp_buf == MAP_FAILED) return nullptr;

    while (true) {
        usleep(100000);  // poll 10× per second
        if (!g_report_requested.load(std::memory_order_relaxed)) continue;
        g_report_requested.store(false, std::memory_order_relaxed);

        scanVmRegions();
        size_t committed = g_vm.committedPages();

        int mem_fd = open("/proc/self/mem", O_RDONLY);
        if (mem_fd < 0) continue;

        size_t pages_ok = 0;
        size_t total_bytes = 0, lz4_bytes = 0, zstd1_bytes = 0;

        for (size_t i = 1; i < committed && pages_ok < 100000; ++i) {
            smash::PageState st = g_states.get(i);
            if (st != smash::PageState::ACTIVE) continue;
            void* addr = g_vm.pageAddress(i);
            if (!addr) continue;

            ssize_t rd = pread(mem_fd, page_buf, smash::kPageSize, (off_t)(uintptr_t)addr);
            if (rd != (ssize_t)smash::kPageSize) continue;

            pages_ok++;
            total_bytes += smash::kPageSize;
            int lz4_sz = LZ4_compress_default(page_buf, comp_buf, smash::kPageSize, lz4_bound);
            lz4_bytes += (lz4_sz > 0) ? (size_t)lz4_sz : smash::kPageSize;
            size_t z1 = ZSTD_compress(comp_buf, zstd_bound, page_buf, smash::kPageSize, 1);
            zstd1_bytes += ZSTD_isError(z1) ? smash::kPageSize : z1;
        }
        close(mem_fd);

        char msg[256];
        int n;
        if (total_bytes > 0) {
            n = snprintf(msg, sizeof(msg),
                "compressed=%zu pages=%.1fMiB lz4=%.2fx zstd1=%.2fx\n",
                pages_ok, total_bytes / (1024.0*1024.0),
                (double)total_bytes / lz4_bytes,
                (double)total_bytes / zstd1_bytes);
        } else {
            n = snprintf(msg, sizeof(msg), "compressed=0 (no tracked pages)\n");
        }
        (void)!write(STDERR_FILENO, msg, n);
    }
    return nullptr;
}

void startCompression() {
    bool expected = false;
    if (!g_compression_started.compare_exchange_strong(expected, true))
        return;
    // Don't start the compressor thread or fault handler — mprotect on
    // system-allocator pages causes SIGSEGV. Instead, scan pages for
    // tracking and start the measurement thread which reports compression
    // ratios after a delay.
    scanVmRegions();

    pthread_t meas_thread;
    pthread_create(&meas_thread, nullptr, measurementThread, nullptr);
    pthread_detach(meas_thread);

    // Auto-trigger ratio report after delay
    static const int delay = []() {
        const char* v = getenv("SMASH_REPORT_DELAY_SEC");
        return v ? atoi(v) : 8;
    }();
    if (delay > 0) {
        struct TimerArg { int sec; };
        auto* ta = static_cast<TimerArg*>(
            smash::BootstrapAlloc::instance().allocate(sizeof(TimerArg), 8));
        ta->sec = delay;
        pthread_t timer;
        pthread_create(&timer, nullptr, [](void* arg) -> void* {
            auto* a = static_cast<TimerArg*>(arg);
            sleep(a->sec);
            g_report_requested.store(true, std::memory_order_relaxed);
            return nullptr;
        }, ta);
        pthread_detach(timer);
    }
}

// Track pages covered by an allocation
void trackAllocation(void* ptr, size_t size) {
    if (!ptr || size == 0) return;
    uintptr_t start_page = reinterpret_cast<uintptr_t>(ptr) & ~(smash::kPageSize - 1);
    uintptr_t end_page = (reinterpret_cast<uintptr_t>(ptr) + size - 1) & ~(smash::kPageSize - 1);

    for (uintptr_t p = start_page; p <= end_page; p += smash::kPageSize) {
        size_t idx = g_vm.trackPage(p);
        if (idx > 0) {
            smash::PageState st = g_states.get(idx);
            if (st == smash::PageState::EMPTY)
                g_states.set(idx, smash::PageState::ACTIVE);
        }
    }
}

// ── VM region scanning ──────────────────────────────────────────────────────
// Discover anonymous RW heap pages that weren't seen through malloc
// interposition. On macOS, system malloc uses vm_allocate (Mach VM API)
// internally; on Linux, glibc malloc uses brk/mmap. Both are invisible
// to our malloc wrapper for per-page discovery.

void scanVmRegions() {
    auto& bootstrap = smash::BootstrapAlloc::instance();
    size_t new_pages = 0;

#ifdef __APPLE__
    // macOS: enumerate VM regions, filter by malloc user tags
    mach_port_t task = mach_task_self();
    vm_address_t addr = 0;
    vm_size_t vmsize = 0;
    uint32_t depth = 1;
    struct vm_region_submap_info_64 info;
    mach_msg_type_number_t count;

    while (true) {
        count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = vm_region_recurse_64(task, &addr, &vmsize, &depth,
            reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
        if (kr != KERN_SUCCESS) break;

        if (info.is_submap) {
            depth++;
            continue;
        }

        bool is_rw = (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) ==
                     (VM_PROT_READ | VM_PROT_WRITE);
        unsigned tag = info.user_tag;
        // VM_MEMORY_MALLOC=1, MALLOC_SMALL=2, MALLOC_LARGE=3, MALLOC_HUGE=4,
        // MALLOC_TINY=7, MALLOC_LARGE_REUSABLE=8, MALLOC_LARGE_REUSED=9,
        // MALLOC_NANO=12, MALLOC_MEDIUM=23
        bool is_malloc = (tag == 1 || tag == 2 || tag == 3 || tag == 4 ||
                          tag == 7 || tag == 8 || tag == 9 || tag == 12 || tag == 23);

        if (is_rw && is_malloc && vmsize >= smash::kPageSize) {
            uintptr_t region_end = addr + vmsize;
            if (bootstrap.owns(reinterpret_cast<void*>(addr))) {
                addr = region_end;
                continue;
            }
            for (uintptr_t p = addr; p < region_end; p += smash::kPageSize) {
                size_t idx = g_vm.trackPage(p);
                if (idx > 0) {
                    smash::PageState st = g_states.get(idx);
                    if (st == smash::PageState::EMPTY) {
                        g_states.set(idx, smash::PageState::ACTIVE);
                        new_pages++;
                    }
                }
                if (g_vm.committedPages() >= g_vm.totalPages() - 100)
                    goto done;
            }
        }
        addr += vmsize;
    }

#else
    // Linux: parse /proc/self/maps for anonymous RW regions (heap + mmap'd)
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        char perms[8];
        unsigned long offset, inode;
        int major, minor;
        // Format: start-end perms offset dev inode [pathname]
        int n = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu",
                       &start, &end, perms, &offset, &major, &minor, &inode);
        if (n < 7) continue;

        // Only track anonymous RW regions (inode == 0, rw-p)
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (inode != 0) continue;  // file-backed, skip

        // Skip regions labeled [stack], [vdso], [vsyscall], etc.
        char* bracket = strchr(line, '[');
        if (bracket && strstr(bracket, "[stack")) continue;
        if (bracket && strstr(bracket, "[vdso")) continue;
        if (bracket && strstr(bracket, "[vsyscall")) continue;
        if (bracket && strstr(bracket, "[vvar")) continue;

        size_t region_size = end - start;
        if (region_size < smash::kPageSize) continue;
        if (bootstrap.owns(reinterpret_cast<void*>(start))) continue;

        for (uintptr_t p = start; p < end; p += smash::kPageSize) {
            size_t idx = g_vm.trackPage(p);
            if (idx > 0) {
                smash::PageState st = g_states.get(idx);
                if (st == smash::PageState::EMPTY) {
                    g_states.set(idx, smash::PageState::ACTIVE);
                    new_pages++;
                }
            }
            if (g_vm.committedPages() >= g_vm.totalPages() - 100) {
                fclose(f);
                goto done;
            }
        }
    }
    fclose(f);
#endif

done:
    if (new_pages > 0)
        g_track_count.fetch_add(new_pages, std::memory_order_relaxed);
}

} // anonymous namespace

// ── Global VmRegion pointer (used by syscall wrappers) ──────────────────────
namespace smash { VmRegion* g_smash_vm_region = nullptr; }

// ── malloc/free interposition ───────────────────────────────────────────────

using malloc_fn   = void*(*)(size_t);
using free_fn     = void(*)(void*);
using calloc_fn   = void*(*)(size_t, size_t);
using realloc_fn  = void*(*)(void*, size_t);
using memalign_fn = int(*)(void**, size_t, size_t);
using mmap_fn_t   = void*(*)(void*, size_t, int, int, int, off_t);

#ifdef __APPLE__
// ── macOS: __DATA,__interpose wrappers ──────────────────────────────────────

extern "C" void* co_malloc(size_t size);
CO_INTERPOSE(co_malloc, malloc);
extern "C" void* co_malloc(size_t size) {
    void* ptr = CO_ORIG(malloc_fn, co_malloc)(size);
    if (ptr && g_inited.load(std::memory_order_relaxed)) {
        g_malloc_count.fetch_add(1, std::memory_order_relaxed);
        g_malloc_bytes.fetch_add(size, std::memory_order_relaxed);
        trackAllocation(ptr, size);
        if (!g_compression_started.load(std::memory_order_relaxed)) {
            if (g_warmup_count.fetch_add(1, std::memory_order_relaxed) >= 5000)
                startCompression();
        }
    }
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
    if (ptr && g_inited.load(std::memory_order_relaxed))
        trackAllocation(ptr, count * size);
    return ptr;
}

extern "C" void* co_realloc(void* old_ptr, size_t size);
CO_INTERPOSE(co_realloc, realloc);
extern "C" void* co_realloc(void* old_ptr, size_t size) {
    void* ptr = CO_ORIG(realloc_fn, co_realloc)(old_ptr, size);
    if (ptr && g_inited.load(std::memory_order_relaxed))
        trackAllocation(ptr, size);
    return ptr;
}

extern "C" int co_posix_memalign(void** memptr, size_t alignment, size_t size);
CO_INTERPOSE(co_posix_memalign, posix_memalign);
extern "C" int co_posix_memalign(void** memptr, size_t alignment, size_t size) {
    int ret = CO_ORIG(memalign_fn, co_posix_memalign)(memptr, alignment, size);
    if (ret == 0 && *memptr && g_inited.load(std::memory_order_relaxed))
        trackAllocation(*memptr, size);
    return ret;
}

extern "C" void* co_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
CO_INTERPOSE(co_mmap, mmap);
extern "C" void* co_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    void* ret = CO_ORIG(mmap_fn_t, co_mmap)(addr, len, prot, flags, fd, offset);
    if (ret != MAP_FAILED && g_inited.load(std::memory_order_relaxed)) {
        if ((flags & MAP_ANON) && (prot & PROT_WRITE))
            trackAllocation(ret, len);
    }
    return ret;
}

#else
// ── Linux: LD_PRELOAD wrappers (exported with original names) ───────────────

// During early init (before g_inited), calloc may be called before dlsym
// resolves the real calloc. Use a static buffer fallback.
static char g_calloc_buf[4096];
static std::atomic<bool> g_in_dlsym{false};

extern "C" void* malloc(size_t size) {
    CO_ORIG_DECL(malloc_fn, malloc);
    void* ptr = orig_malloc(size);
    if (ptr && g_inited.load(std::memory_order_relaxed)) {
        g_malloc_count.fetch_add(1, std::memory_order_relaxed);
        g_malloc_bytes.fetch_add(size, std::memory_order_relaxed);
        trackAllocation(ptr, size);
        if (!g_compression_started.load(std::memory_order_relaxed)) {
            if (g_warmup_count.fetch_add(1, std::memory_order_relaxed) >= 5000)
                startCompression();
        }
    }
    return ptr;
}

extern "C" void free(void* ptr) {
    // Don't free the static calloc buffer
    if (ptr >= g_calloc_buf && ptr < g_calloc_buf + sizeof(g_calloc_buf))
        return;
    CO_ORIG_DECL(free_fn, free);
    orig_free(ptr);
}

extern "C" void* calloc(size_t count, size_t size) {
    // dlsym itself may call calloc — provide a static buffer fallback
    if (g_in_dlsym.load(std::memory_order_relaxed)) {
        size_t total = count * size;
        if (total <= sizeof(g_calloc_buf)) {
            __builtin_memset(g_calloc_buf, 0, total);
            return g_calloc_buf;
        }
        return nullptr;
    }
    CO_ORIG_DECL(calloc_fn, calloc);
    void* ptr = orig_calloc(count, size);
    if (ptr && g_inited.load(std::memory_order_relaxed))
        trackAllocation(ptr, count * size);
    return ptr;
}

extern "C" void* realloc(void* old_ptr, size_t size) {
    CO_ORIG_DECL(realloc_fn, realloc);
    void* ptr = orig_realloc(old_ptr, size);
    if (ptr && g_inited.load(std::memory_order_relaxed))
        trackAllocation(ptr, size);
    return ptr;
}

extern "C" int posix_memalign(void** memptr, size_t alignment, size_t size) {
    CO_ORIG_DECL(memalign_fn, posix_memalign);
    int ret = orig_posix_memalign(memptr, alignment, size);
    if (ret == 0 && *memptr && g_inited.load(std::memory_order_relaxed))
        trackAllocation(*memptr, size);
    return ret;
}

extern "C" void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    CO_ORIG_DECL(mmap_fn_t, mmap);
    void* ret = orig_mmap(addr, len, prot, flags, fd, offset);
    if (ret != MAP_FAILED && g_inited.load(std::memory_order_relaxed)) {
        if ((flags & MAP_ANONYMOUS) && (prot & PROT_WRITE))
            trackAllocation(ret, len);
    }
    return ret;
}

#endif // __APPLE__ vs Linux malloc wrappers

// ── Syscall interposition (kernel buffer compatibility) ─────────────────────
// Warm and pin pages before syscalls that access userspace buffers,
// to prevent kernel EFAULT on compressed/protected pages.

static inline void warmIovec(const struct iovec* iov, int iovcnt, smash::VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len)
            smash::vm::warmPages(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

// ── read / write ─────────────────────────────────────────────────────────────

using read_fn_t = ssize_t(*)(int, void*, size_t);
using write_fn_t = ssize_t(*)(int, const void*, size_t);

#ifdef __APPLE__

extern "C" ssize_t co_read(int fd, void* buf, size_t count);
CO_INTERPOSE(co_read, read);
extern "C" ssize_t co_read(int fd, void* buf, size_t count) {
    return smash::vm::retryWith1Buf(
        [&] { return CO_ORIG(read_fn_t, co_read)(fd, buf, count); },
        buf, count);
}

extern "C" ssize_t co_write(int fd, const void* buf, size_t count);
CO_INTERPOSE(co_write, write);
extern "C" ssize_t co_write(int fd, const void* buf, size_t count) {
    return smash::vm::retryWith1Buf(
        [&] { return CO_ORIG(write_fn_t, co_write)(fd, buf, count); },
        buf, count);
}

// ── pread / pwrite ───────────────────────────────────────────────────────────

using pread_fn_t = ssize_t(*)(int, void*, size_t, off_t);
using pwrite_fn_t = ssize_t(*)(int, const void*, size_t, off_t);

extern "C" ssize_t co_pread(int fd, void* buf, size_t count, off_t offset);
CO_INTERPOSE(co_pread, pread);
extern "C" ssize_t co_pread(int fd, void* buf, size_t count, off_t offset) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = CO_ORIG(pread_fn_t, co_pread)(fd, buf, count, offset);
    return ret;
}

extern "C" ssize_t co_pwrite(int fd, const void* buf, size_t count, off_t offset);
CO_INTERPOSE(co_pwrite, pwrite);
extern "C" ssize_t co_pwrite(int fd, const void* buf, size_t count, off_t offset) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = CO_ORIG(pwrite_fn_t, co_pwrite)(fd, buf, count, offset);
    return ret;
}

// ── readv / writev ───────────────────────────────────────────────────────────

using readv_fn_t = ssize_t(*)(int, const struct iovec*, int);
using writev_fn_t = ssize_t(*)(int, const struct iovec*, int);

extern "C" ssize_t co_readv(int fd, const struct iovec* iov, int iovcnt);
CO_INTERPOSE(co_readv, readv);
extern "C" ssize_t co_readv(int fd, const struct iovec* iov, int iovcnt) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) warmIovec(iov, iovcnt, vm);
    ssize_t ret = CO_ORIG(readv_fn_t, co_readv)(fd, iov, iovcnt);
    return ret;
}

extern "C" ssize_t co_writev(int fd, const struct iovec* iov, int iovcnt);
CO_INTERPOSE(co_writev, writev);
extern "C" ssize_t co_writev(int fd, const struct iovec* iov, int iovcnt) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) warmIovec(iov, iovcnt, vm);
    ssize_t ret = CO_ORIG(writev_fn_t, co_writev)(fd, iov, iovcnt);
    return ret;
}

// ── recv / send ──────────────────────────────────────────────────────────────

using recv_fn_t = ssize_t(*)(int, void*, size_t, int);
using send_fn_t = ssize_t(*)(int, const void*, size_t, int);

extern "C" ssize_t co_recv(int s, void* buf, size_t len, int flags);
CO_INTERPOSE(co_recv, recv);
extern "C" ssize_t co_recv(int s, void* buf, size_t len, int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) smash::vm::warmPages(buf, len, vm);
    ssize_t ret = CO_ORIG(recv_fn_t, co_recv)(s, buf, len, flags);
    return ret;
}

extern "C" ssize_t co_send(int s, const void* buf, size_t len, int flags);
CO_INTERPOSE(co_send, send);
extern "C" ssize_t co_send(int s, const void* buf, size_t len, int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) smash::vm::warmPages(buf, len, vm);
    ssize_t ret = CO_ORIG(send_fn_t, co_send)(s, buf, len, flags);
    return ret;
}

// ── poll ─────────────────────────────────────────────────────────────────────

using poll_fn_t = int(*)(struct pollfd*, nfds_t, int);

extern "C" int co_poll(struct pollfd* fds, nfds_t nfds, int timeout);
CO_INTERPOSE(co_poll, poll);
extern "C" int co_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && fds && nfds > 0) {
        smash::vm::warmPages(fds, nfds * sizeof(struct pollfd), vm);
    }
    int ret = CO_ORIG(poll_fn_t, co_poll)(fds, nfds, timeout);
    return ret;
}

// ── kevent (macOS only) ──────────────────────────────────────────────────────

extern "C" int co_kevent(int kq, const struct kevent* changelist, int nchanges,
                          struct kevent* eventlist, int nevents,
                          const struct timespec* timeout);
CO_INTERPOSE(co_kevent, kevent);
using kevent_fn_t = int(*)(int, const struct kevent*, int, struct kevent*, int, const struct timespec*);
extern "C" int co_kevent(int kq, const struct kevent* changelist, int nchanges,
                          struct kevent* eventlist, int nevents,
                          const struct timespec* timeout) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) {
        if (changelist && nchanges > 0) {
            smash::vm::warmPages(changelist, nchanges * sizeof(struct kevent), vm);
        }
        if (eventlist && nevents > 0) {
            smash::vm::warmPages(eventlist, nevents * sizeof(struct kevent), vm);
        }
    }
    // Retry on EFAULT: Phase 3's per-page mprotect(PROT_READ) can race with
    // pinPages in a tiny window. On retry, warmPages triggers the fault handler
    // to restore PROT_RW, and the pin prevents Phase 3 from re-protecting.
    int ret;
    for (int attempt = 0; ; ++attempt) {
        ret = CO_ORIG(kevent_fn_t, co_kevent)(kq, changelist, nchanges, eventlist, nevents, timeout);
        if (ret != -1 || errno != EFAULT || attempt >= 3) break;
        // Re-warm to trigger fault handler (restores PROT_RW via SIGSEGV)
        if (vm) {
            if (changelist && nchanges > 0)
                smash::vm::warmPages(changelist, nchanges * sizeof(struct kevent), vm);
            if (eventlist && nevents > 0)
                smash::vm::warmPages(eventlist, nevents * sizeof(struct kevent), vm);
        }
    }
    return ret;
}

// ── Buffered I/O (fread, fgets, fgetc, getc, fwrite, fflush) ────────────────
// On macOS, FILE->_bf._base gives internal buffer. On Linux this is
// FILE->_IO_buf_base. Warm+pin these to prevent EFAULT on intra-libc
// read() calls that are invisible to DYLD interposition.

static inline void warmFileBuffer(FILE* stream, smash::VmRegion* vm) {
    if (!stream) return;
    void* base = stream->_bf._base;
    int size = stream->_bf._size;
    if (base && size > 0) {
        smash::vm::warmPages(base, size, vm);
    }
}

static inline void unpinFileBuffer(FILE* stream, smash::VmRegion* vm) {
    if (!stream) return;
    void* base = stream->_bf._base;
    int size = stream->_bf._size;
}

using fread_fn_t = size_t(*)(void*, size_t, size_t, FILE*);
using fgets_fn_t = char*(*)(char*, int, FILE*);
using fgetc_fn_t = int(*)(FILE*);
using fwrite_fn_t = size_t(*)(const void*, size_t, size_t, FILE*);
using fflush_fn_t = int(*)(FILE*);

extern "C" size_t co_fread(void* ptr, size_t size, size_t nitems, FILE* stream);
CO_INTERPOSE(co_fread, fread);
extern "C" size_t co_fread(void* ptr, size_t size, size_t nitems, FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    size_t total = size * nitems;
    if (vm) {
        if (ptr && total) smash::vm::warmPages(ptr, total, vm);
        warmFileBuffer(stream, vm);
    }
    size_t ret = CO_ORIG(fread_fn_t, co_fread)(ptr, size, nitems, stream);
    if (vm) {
        unpinFileBuffer(stream, vm);
    }
    return ret;
}

extern "C" char* co_fgets(char* str, int size, FILE* stream);
CO_INTERPOSE(co_fgets, fgets);
extern "C" char* co_fgets(char* str, int size, FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) {
        if (str && size > 0) smash::vm::warmPages(str, size, vm);
        warmFileBuffer(stream, vm);
    }
    char* ret = CO_ORIG(fgets_fn_t, co_fgets)(str, size, stream);
    if (vm) {
        unpinFileBuffer(stream, vm);
    }
    return ret;
}

extern "C" int co_fgetc(FILE* stream);
CO_INTERPOSE(co_fgetc, fgetc);
extern "C" int co_fgetc(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = CO_ORIG(fgetc_fn_t, co_fgetc)(stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

extern "C" int co_getc(FILE* stream);
CO_INTERPOSE(co_getc, getc);
extern "C" int co_getc(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = CO_ORIG(fgetc_fn_t, co_getc)(stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

extern "C" size_t co_fwrite(const void* ptr, size_t size, size_t nitems, FILE* stream);
CO_INTERPOSE(co_fwrite, fwrite);
extern "C" size_t co_fwrite(const void* ptr, size_t size, size_t nitems, FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    size_t total = size * nitems;
    if (vm) {
        if (ptr && total) smash::vm::warmPages(ptr, total, vm);
        warmFileBuffer(stream, vm);
    }
    size_t ret = CO_ORIG(fwrite_fn_t, co_fwrite)(ptr, size, nitems, stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

extern "C" int co_fflush(FILE* stream);
CO_INTERPOSE(co_fflush, fflush);
extern "C" int co_fflush(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = CO_ORIG(fflush_fn_t, co_fflush)(stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

#else
// ── Linux: LD_PRELOAD syscall wrappers ──────────────────────────────────────
// On Linux with LD_PRELOAD, we export functions with the original names.
// dlsym(RTLD_NEXT) finds the real libc implementation.

extern "C" ssize_t read(int fd, void* buf, size_t count) {
    CO_ORIG_DECL(read_fn_t, read);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = orig_read(fd, buf, count);
    return ret;
}

extern "C" ssize_t write(int fd, const void* buf, size_t count) {
    CO_ORIG_DECL(write_fn_t, write);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = orig_write(fd, buf, count);
    return ret;
}

using pread_fn_t = ssize_t(*)(int, void*, size_t, off_t);
using pwrite_fn_t = ssize_t(*)(int, const void*, size_t, off_t);

extern "C" ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    CO_ORIG_DECL(pread_fn_t, pread);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = orig_pread(fd, buf, count, offset);
    return ret;
}

extern "C" ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    CO_ORIG_DECL(pwrite_fn_t, pwrite);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = orig_pwrite(fd, buf, count, offset);
    return ret;
}

using readv_fn_t = ssize_t(*)(int, const struct iovec*, int);
using writev_fn_t = ssize_t(*)(int, const struct iovec*, int);

extern "C" ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    CO_ORIG_DECL(readv_fn_t, readv);
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) warmIovec(iov, iovcnt, vm);
    ssize_t ret = orig_readv(fd, iov, iovcnt);
    return ret;
}

extern "C" ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    CO_ORIG_DECL(writev_fn_t, writev);
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) warmIovec(iov, iovcnt, vm);
    ssize_t ret = orig_writev(fd, iov, iovcnt);
    return ret;
}

extern "C" ssize_t recv(int s, void* buf, size_t len, int flags) {
    CO_ORIG_DECL(recv_fn_t, recv);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) smash::vm::warmPages(buf, len, vm);
    ssize_t ret = orig_recv(s, buf, len, flags);
    return ret;
}

extern "C" ssize_t send(int s, const void* buf, size_t len, int flags) {
    CO_ORIG_DECL(send_fn_t, send);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) smash::vm::warmPages(buf, len, vm);
    ssize_t ret = orig_send(s, buf, len, flags);
    return ret;
}

extern "C" int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    CO_ORIG_DECL(poll_fn_t, poll);
    auto* vm = smash::g_smash_vm_region;
    if (vm && fds && nfds > 0) {
        smash::vm::warmPages(fds, nfds * sizeof(struct pollfd), vm);
    }
    int ret = orig_poll(fds, nfds, timeout);
    return ret;
}

// Linux: epoll_wait interposition
using epoll_wait_fn_t = int(*)(int, struct epoll_event*, int, int);

extern "C" int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    CO_ORIG_DECL(epoll_wait_fn_t, epoll_wait);
    auto* vm = smash::g_smash_vm_region;
    if (vm && events && maxevents > 0) {
        smash::vm::warmPages(events, maxevents * sizeof(struct epoll_event), vm);
    }
    int ret = orig_epoll_wait(epfd, events, maxevents, timeout);
    return ret;
}

// Linux: skip buffered I/O interposition (LD_PRELOAD catches read/write
// from within glibc, unlike macOS where intra-dylib calls are invisible)

#endif // __APPLE__ vs Linux syscall wrappers

// ── Library initialization ──────────────────────────────────────────────────

__attribute__((constructor, used))
void co_init() {
#ifndef __APPLE__
    // Mark that we're in init (dlsym may call calloc)
    g_in_dlsym.store(true, std::memory_order_relaxed);
    // Force resolution of malloc/free before setting g_inited
    void* p = dlsym(RTLD_NEXT, "malloc");
    (void)p;
    p = dlsym(RTLD_NEXT, "free");
    (void)p;
    p = dlsym(RTLD_NEXT, "calloc");
    (void)p;
    g_in_dlsym.store(false, std::memory_order_relaxed);
#endif

    if (!g_vm.init(0))
        return;

    g_states.init(g_vm.totalPages());
    g_locks.init(g_vm.totalPages());
    g_store.init();
    g_engine.init();

    // SMASH_COMPRESS_ONLY=1 compile define (from CMakeLists) forces
    // isCompressOnlyMode() → true at compile time, so VmRegion uses
    // tracking mode. No runtime env var needed.
    setenv("SMASH_NO_MONITOR", "1", 0);

    // page_map = nullptr: no span info available (system malloc manages objects)
    g_compressor.init(&g_vm, &g_states, &g_locks, &g_store, &g_engine,
                      nullptr, &g_fault_handler);

    smash::g_smash_vm_region = &g_vm;

    signal(SIGUSR2, sigusr2Handler);

    g_inited.store(true, std::memory_order_release);
}
