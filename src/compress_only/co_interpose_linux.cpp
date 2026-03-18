// smash/src/compress_only/co_interpose_linux.cpp - Linux LD_PRELOAD interposition
//
// Uses LD_PRELOAD with dlsym(RTLD_NEXT) to intercept malloc/free and syscalls.
// Forwards allocations to glibc malloc, tracks pages, runs compression.

#ifndef __APPLE__

#include "co_common.h"
#include <dlfcn.h>
#include <sys/epoll.h>

// ── dlsym helper ────────────────────────────────────────────────────────────

#define CO_ORIG_DECL(fn_type, name) \
    static fn_type orig_##name = nullptr; \
    if (!orig_##name) { \
        orig_##name = reinterpret_cast<fn_type>(dlsym(RTLD_NEXT, #name)); \
    }

// ── VM region scanning (Linux) ──────────────────────────────────────────────
// Parse /proc/self/maps to find anonymous RW regions (heap + mmap'd).
// glibc malloc uses brk() for small heaps and mmap() for large allocations;
// both show up as anonymous RW mappings.

namespace co {

void scanVmRegions() {
    auto& bootstrap = smash::BootstrapAlloc::instance();
    size_t new_pages = 0;

    // Use dlsym to get the real fopen/fgets/fclose to avoid recursion
    using fopen_fn_t = FILE*(*)(const char*, const char*);
    using fgets_fn_t = char*(*)(char*, int, FILE*);
    using fclose_fn_t = int(*)(FILE*);
    static fopen_fn_t real_fopen = reinterpret_cast<fopen_fn_t>(dlsym(RTLD_NEXT, "fopen"));
    static fgets_fn_t real_fgets = reinterpret_cast<fgets_fn_t>(dlsym(RTLD_NEXT, "fgets"));
    static fclose_fn_t real_fclose = reinterpret_cast<fclose_fn_t>(dlsym(RTLD_NEXT, "fclose"));

    if (!real_fopen || !real_fgets || !real_fclose) return;

    FILE* f = real_fopen("/proc/self/maps", "r");
    if (!f) return;

    char line[512];
    while (real_fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        char perms[8];
        unsigned long offset, inode;
        int major, minor;
        int n = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu",
                       &start, &end, perms, &offset, &major, &minor, &inode);
        if (n < 7) continue;

        // Only anonymous RW regions
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (inode != 0) continue;

        // Skip stack, vdso, vvar, vsyscall
        char* bracket = strchr(line, '[');
        if (bracket && (strstr(bracket, "[stack") || strstr(bracket, "[vdso") ||
                        strstr(bracket, "[vsyscall") || strstr(bracket, "[vvar"))) continue;

        size_t region_size = end - start;
        if (region_size < smash::kPageSize) continue;
        if (bootstrap.owns(reinterpret_cast<void*>(start))) continue;

        for (uintptr_t p = start; p < end; p += smash::kPageSize) {
            size_t idx = g_vm.trackPage(p);
            if (idx > 0 && g_states.get(idx) == smash::PageState::EMPTY) {
                g_states.set(idx, smash::PageState::ACTIVE);
                new_pages++;
            }
            if (g_vm.committedPages() >= g_vm.totalPages() - 100) {
                real_fclose(f);
                goto done;
            }
        }
    }
    real_fclose(f);

done:
    if (new_pages > 0)
        g_track_count.fetch_add(new_pages, std::memory_order_relaxed);
}

} // namespace co

// ── calloc bootstrap ────────────────────────────────────────────────────────
// dlsym itself may call calloc during symbol resolution. Provide a static
// buffer fallback to break the recursion.

static char g_calloc_buf[4096];
static std::atomic<bool> g_in_dlsym{false};

// ── malloc/free interposition ───────────────────────────────────────────────

using malloc_fn   = void*(*)(size_t);
using free_fn     = void(*)(void*);
using calloc_fn   = void*(*)(size_t, size_t);
using realloc_fn  = void*(*)(void*, size_t);
using memalign_fn = int(*)(void**, size_t, size_t);
using mmap_fn_t   = void*(*)(void*, size_t, int, int, int, off_t);

extern "C" void* malloc(size_t size) {
    CO_ORIG_DECL(malloc_fn, malloc);
    void* ptr = orig_malloc(size);
    co::trackMalloc(ptr, size);
    return ptr;
}

extern "C" void free(void* ptr) {
    if (ptr >= g_calloc_buf && ptr < g_calloc_buf + sizeof(g_calloc_buf))
        return;
    CO_ORIG_DECL(free_fn, free);
    orig_free(ptr);
}

extern "C" void* calloc(size_t count, size_t size) {
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
    if (ptr && co::g_inited.load(std::memory_order_relaxed))
        co::trackAllocation(ptr, count * size);
    return ptr;
}

extern "C" void* realloc(void* old_ptr, size_t size) {
    CO_ORIG_DECL(realloc_fn, realloc);
    void* ptr = orig_realloc(old_ptr, size);
    if (ptr && co::g_inited.load(std::memory_order_relaxed))
        co::trackAllocation(ptr, size);
    return ptr;
}

extern "C" int posix_memalign(void** memptr, size_t alignment, size_t size) {
    CO_ORIG_DECL(memalign_fn, posix_memalign);
    int ret = orig_posix_memalign(memptr, alignment, size);
    if (ret == 0 && *memptr && co::g_inited.load(std::memory_order_relaxed))
        co::trackAllocation(*memptr, size);
    return ret;
}

extern "C" void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    CO_ORIG_DECL(mmap_fn_t, mmap);
    void* ret = orig_mmap(addr, len, prot, flags, fd, offset);
    if (ret != MAP_FAILED && co::g_inited.load(std::memory_order_relaxed)) {
        if ((flags & MAP_ANONYMOUS) && (prot & PROT_WRITE))
            co::trackAllocation(ret, len);
    }
    return ret;
}

// ── Syscall interposition ───────────────────────────────────────────────────
// On Linux with LD_PRELOAD, we export functions with the original names.
// dlsym(RTLD_NEXT) finds the real glibc implementation.

using read_fn_t = ssize_t(*)(int, void*, size_t);
using write_fn_t = ssize_t(*)(int, const void*, size_t);
using pread_fn_t = ssize_t(*)(int, void*, size_t, off_t);
using pwrite_fn_t = ssize_t(*)(int, const void*, size_t, off_t);
using readv_fn_t = ssize_t(*)(int, const struct iovec*, int);
using writev_fn_t = ssize_t(*)(int, const struct iovec*, int);
using recv_fn_t = ssize_t(*)(int, void*, size_t, int);
using send_fn_t = ssize_t(*)(int, const void*, size_t, int);
using poll_fn_t = int(*)(struct pollfd*, nfds_t, int);
using epoll_wait_fn_t = int(*)(int, struct epoll_event*, int, int);

extern "C" ssize_t read(int fd, void* buf, size_t count) {
    CO_ORIG_DECL(read_fn_t, read);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = orig_read(fd, buf, count);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t write(int fd, const void* buf, size_t count) {
    CO_ORIG_DECL(write_fn_t, write);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = orig_write(fd, buf, count);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    CO_ORIG_DECL(pread_fn_t, pread);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = orig_pread(fd, buf, count, offset);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    CO_ORIG_DECL(pwrite_fn_t, pwrite);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = orig_pwrite(fd, buf, count, offset);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    CO_ORIG_DECL(readv_fn_t, readv);
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) { co::warmIovec(iov, iovcnt, vm); smash::vm::pinIovec(iov, iovcnt, vm); }
    ssize_t ret = orig_readv(fd, iov, iovcnt);
    if (vm && iov && iovcnt > 0) smash::vm::unpinIovec(iov, iovcnt, vm);
    return ret;
}

extern "C" ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    CO_ORIG_DECL(writev_fn_t, writev);
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) { co::warmIovec(iov, iovcnt, vm); smash::vm::pinIovec(iov, iovcnt, vm); }
    ssize_t ret = orig_writev(fd, iov, iovcnt);
    if (vm && iov && iovcnt > 0) smash::vm::unpinIovec(iov, iovcnt, vm);
    return ret;
}

extern "C" ssize_t recv(int s, void* buf, size_t len, int flags) {
    CO_ORIG_DECL(recv_fn_t, recv);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = orig_recv(s, buf, len, flags);
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

extern "C" ssize_t send(int s, const void* buf, size_t len, int flags) {
    CO_ORIG_DECL(send_fn_t, send);
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = orig_send(s, buf, len, flags);
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

extern "C" int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    CO_ORIG_DECL(poll_fn_t, poll);
    auto* vm = smash::g_smash_vm_region;
    if (vm && fds && nfds > 0) {
        smash::vm::warmPages(fds, nfds * sizeof(struct pollfd), vm);
        smash::vm::pinPages(fds, nfds * sizeof(struct pollfd), vm);
    }
    int ret = orig_poll(fds, nfds, timeout);
    if (vm && fds && nfds > 0)
        smash::vm::unpinPages(fds, nfds * sizeof(struct pollfd), vm);
    return ret;
}

extern "C" int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    CO_ORIG_DECL(epoll_wait_fn_t, epoll_wait);
    auto* vm = smash::g_smash_vm_region;
    if (vm && events && maxevents > 0) {
        smash::vm::warmPages(events, maxevents * sizeof(struct epoll_event), vm);
        smash::vm::pinPages(events, maxevents * sizeof(struct epoll_event), vm);
    }
    int ret = orig_epoll_wait(epfd, events, maxevents, timeout);
    if (vm && events && maxevents > 0)
        smash::vm::unpinPages(events, maxevents * sizeof(struct epoll_event), vm);
    return ret;
}

// Note: On Linux, LD_PRELOAD intercepts read/write calls from glibc's
// buffered I/O (fread, fgets, etc.) as well, so separate fread/fwrite
// interposition is not needed.

// ── Library initialization ──────────────────────────────────────────────────

__attribute__((constructor))
static void co_init() {
    // dlsym may call calloc; set flag to enable static buffer fallback
    g_in_dlsym.store(true, std::memory_order_relaxed);
    (void)dlsym(RTLD_NEXT, "malloc");
    (void)dlsym(RTLD_NEXT, "free");
    (void)dlsym(RTLD_NEXT, "calloc");
    g_in_dlsym.store(false, std::memory_order_relaxed);

    co::init();
}

#endif // !__APPLE__
