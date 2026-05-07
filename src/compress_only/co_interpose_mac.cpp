// smash/src/compress_only/co_interpose_mac.cpp - macOS DYLD interposition
//
// Uses __DATA,__interpose to replace malloc/free and syscalls.
// Forwards allocations to system malloc, tracks pages, runs compression.

#ifdef __APPLE__

#include "co_common.h"

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

// ── VM region scanning (macOS) ──────────────────────────────────────────────
// Enumerate Mach VM regions to find malloc-managed anonymous RW pages.
// System malloc uses vm_allocate (Mach trap) for heap zones, which can't
// be intercepted by __DATA,__interpose.

namespace co {

void scanVmRegions() {
    auto& bootstrap = smash::BootstrapAlloc::instance();
    size_t new_pages = 0;

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

        if (info.is_submap) { depth++; continue; }

        bool is_rw = (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) ==
                     (VM_PROT_READ | VM_PROT_WRITE);
        unsigned tag = info.user_tag;
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
                if (idx > 0 && g_states.get(idx) == smash::PageState::EMPTY) {
                    g_states.set(idx, smash::PageState::ACTIVE);
                    new_pages++;
                }
                if (g_vm.committedPages() >= g_vm.totalPages() - 100)
                    goto done;
            }
        }
        addr += vmsize;
    }
done:
    if (new_pages > 0)
        g_track_count.fetch_add(new_pages, std::memory_order_relaxed);
}

} // namespace co

// ── malloc/free interposition ───────────────────────────────────────────────

using malloc_fn   = void*(*)(size_t);
using free_fn     = void(*)(void*);
using calloc_fn   = void*(*)(size_t, size_t);
using realloc_fn  = void*(*)(void*, size_t);
using memalign_fn = int(*)(void**, size_t, size_t);
using mmap_fn_t   = void*(*)(void*, size_t, int, int, int, off_t);

extern "C" void* co_malloc(size_t size);
CO_INTERPOSE(co_malloc, malloc);
extern "C" void* co_malloc(size_t size) {
    void* ptr = CO_ORIG(malloc_fn, co_malloc)(size);
    co::trackMalloc(ptr, size);
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
    if (ptr && co::g_inited.load(std::memory_order_relaxed))
        co::trackAllocation(ptr, count * size);
    return ptr;
}

extern "C" void* co_realloc(void* old_ptr, size_t size);
CO_INTERPOSE(co_realloc, realloc);
extern "C" void* co_realloc(void* old_ptr, size_t size) {
    void* ptr = CO_ORIG(realloc_fn, co_realloc)(old_ptr, size);
    if (ptr && co::g_inited.load(std::memory_order_relaxed))
        co::trackAllocation(ptr, size);
    return ptr;
}

extern "C" int co_posix_memalign(void** memptr, size_t alignment, size_t size);
CO_INTERPOSE(co_posix_memalign, posix_memalign);
extern "C" int co_posix_memalign(void** memptr, size_t alignment, size_t size) {
    int ret = CO_ORIG(memalign_fn, co_posix_memalign)(memptr, alignment, size);
    if (ret == 0 && *memptr && co::g_inited.load(std::memory_order_relaxed))
        co::trackAllocation(*memptr, size);
    return ret;
}

extern "C" void* co_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
CO_INTERPOSE(co_mmap, mmap);
extern "C" void* co_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    void* ret = CO_ORIG(mmap_fn_t, co_mmap)(addr, len, prot, flags, fd, offset);
    if (ret != MAP_FAILED && co::g_inited.load(std::memory_order_relaxed)) {
        if ((flags & MAP_ANON) && (prot & PROT_WRITE))
            co::trackAllocation(ret, len);
    }
    return ret;
}

// ── Syscall interposition ───────────────────────────────────────────────────

using read_fn_t = ssize_t(*)(int, void*, size_t);
using write_fn_t = ssize_t(*)(int, const void*, size_t);
using pread_fn_t = ssize_t(*)(int, void*, size_t, off_t);
using pwrite_fn_t = ssize_t(*)(int, const void*, size_t, off_t);
using readv_fn_t = ssize_t(*)(int, const struct iovec*, int);
using writev_fn_t = ssize_t(*)(int, const struct iovec*, int);
using recv_fn_t = ssize_t(*)(int, void*, size_t, int);
using send_fn_t = ssize_t(*)(int, const void*, size_t, int);
using poll_fn_t = int(*)(struct pollfd*, nfds_t, int);
using kevent_fn_t = int(*)(int, const struct kevent*, int, struct kevent*, int, const struct timespec*);

// Helper macros for common warm+pin+call+unpin pattern
#define CO_SYSCALL_RW(name, fn_type, ...) \
    extern "C" auto co_##name(__VA_ARGS__); \
    CO_INTERPOSE(co_##name, name);

extern "C" ssize_t co_read(int fd, void* buf, size_t count);
CO_INTERPOSE(co_read, read);
extern "C" ssize_t co_read(int fd, void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = CO_ORIG(read_fn_t, co_read)(fd, buf, count);
    return ret;
}

extern "C" ssize_t co_write(int fd, const void* buf, size_t count);
CO_INTERPOSE(co_write, write);
extern "C" ssize_t co_write(int fd, const void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) smash::vm::warmPages(buf, count, vm);
    ssize_t ret = CO_ORIG(write_fn_t, co_write)(fd, buf, count);
    return ret;
}

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

extern "C" ssize_t co_readv(int fd, const struct iovec* iov, int iovcnt);
CO_INTERPOSE(co_readv, readv);
extern "C" ssize_t co_readv(int fd, const struct iovec* iov, int iovcnt) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) co::warmIovec(iov, iovcnt, vm);
    ssize_t ret = CO_ORIG(readv_fn_t, co_readv)(fd, iov, iovcnt);
    return ret;
}

extern "C" ssize_t co_writev(int fd, const struct iovec* iov, int iovcnt);
CO_INTERPOSE(co_writev, writev);
extern "C" ssize_t co_writev(int fd, const struct iovec* iov, int iovcnt) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) co::warmIovec(iov, iovcnt, vm);
    ssize_t ret = CO_ORIG(writev_fn_t, co_writev)(fd, iov, iovcnt);
    return ret;
}

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

extern "C" int co_kevent(int kq, const struct kevent* changelist, int nchanges,
                          struct kevent* eventlist, int nevents,
                          const struct timespec* timeout);
CO_INTERPOSE(co_kevent, kevent);
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
    int ret = CO_ORIG(kevent_fn_t, co_kevent)(kq, changelist, nchanges, eventlist, nevents, timeout);
    return ret;
}

// ── Buffered I/O interposition ──────────────────────────────────────────────

static inline void warmFileBuffer(FILE* stream, smash::VmRegion* vm) {
    if (!stream) return;
    void* base = stream->_bf._base;
    int size = stream->_bf._size;
    if (base && size > 0) {
        smash::vm::warmPages(base, size, vm);
    }
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
    }
    return ret;
}

extern "C" int co_fgetc(FILE* stream);
CO_INTERPOSE(co_fgetc, fgetc);
extern "C" int co_fgetc(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = CO_ORIG(fgetc_fn_t, co_fgetc)(stream);
    return ret;
}

extern "C" int co_getc(FILE* stream);
CO_INTERPOSE(co_getc, getc);
extern "C" int co_getc(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = CO_ORIG(fgetc_fn_t, co_getc)(stream);
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
    return ret;
}

extern "C" int co_fflush(FILE* stream);
CO_INTERPOSE(co_fflush, fflush);
extern "C" int co_fflush(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = CO_ORIG(fflush_fn_t, co_fflush)(stream);
    return ret;
}

// ── Library initialization ──────────────────────────────────────────────────

__attribute__((constructor))
static void co_init() {
    co::init();
}

#endif // __APPLE__
