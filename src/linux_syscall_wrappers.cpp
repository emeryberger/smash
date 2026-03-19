// smash/src/linux_syscall_wrappers.cpp - Linux LD_PRELOAD syscall interposition
//
// On Linux, kernel syscalls access userspace buffers directly without
// triggering SIGSEGV. When Smash marks pages PROT_NONE (compressed) or
// PROT_READ (monitoring), kernel syscalls on those buffers return EFAULT.
//
// We interpose on syscalls that read/write userspace buffers and warm+pin
// each Smash-managed page before calling the real syscall.
//
// Real function pointers are resolved lazily via dlsym(RTLD_NEXT) on
// first call. This avoids the early-init crash where glibc calls
// read/write/poll before constructors run. A recursion guard falls
// back to raw syscall() if dlsym itself triggers our wrappers — safe
// because Smash hasn't started yet during early init.

#ifdef __linux__

#include "smash_heap.h"
#include "vm/syscall_compat.h"

#include <atomic>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <poll.h>
#include <unistd.h>

// Recursion guard: if dlsym triggers one of our wrappers, skip the
// dlsym attempt and let the wrapper fall through to raw syscall().
// This is safe because during early init no pages are compressed.
static std::atomic<bool> g_resolving_syscalls{false};

// Lazy-resolve a real function pointer via dlsym(RTLD_NEXT).
// If we're in a dlsym recursion, leaves the pointer as nullptr
// so the caller falls through to its raw syscall() path.
#define SMASH_LAZY_RESOLVE(fn_type, name) \
    static fn_type real_##name = nullptr; \
    if (__builtin_expect(!real_##name, 0)) { \
        if (!g_resolving_syscalls.load(std::memory_order_relaxed)) { \
            g_resolving_syscalls.store(true, std::memory_order_relaxed); \
            real_##name = reinterpret_cast<fn_type>(dlsym(RTLD_NEXT, #name)); \
            g_resolving_syscalls.store(false, std::memory_order_relaxed); \
        } \
    }

// Check if ANY part of a buffer range is in Smash-managed memory.
// Only return true if the buffer actually needs warming.
static inline bool bufferInHeap(const void* buf, size_t len, smash::VmRegion* vm) {
    if (!vm || !buf || !len) return false;
    auto base = reinterpret_cast<uintptr_t>(buf);
    // Check first and last page of buffer
    return vm->contains(base) || vm->contains(base + len - 1);
}

// Helper: check if any iovec buffer is in Smash-managed memory
static inline bool iovecInHeap(const struct iovec* iov, int iovcnt, smash::VmRegion* vm) {
    if (!vm || !iov || iovcnt <= 0) return false;
    for (int i = 0; i < iovcnt; ++i) {
        if (bufferInHeap(iov[i].iov_base, iov[i].iov_len, vm))
            return true;
    }
    return false;
}

// Helper: warm iovec array buffers (only those in heap)
static inline void warmIovecLinux(const struct iovec* iov, int iovcnt, smash::VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len && bufferInHeap(iov[i].iov_base, iov[i].iov_len, vm))
            smash::vm::warmPages(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

#pragma GCC visibility push(default)
extern "C" {

ssize_t read(int fd, void* buf, size_t count) {
    using fn_t = ssize_t(*)(int, void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, read);
    if (!real_read) return syscall(SYS_read, fd, buf, count);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, count, vm);
    if (in_heap) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = real_read(fd, buf, count);
    if (in_heap) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

ssize_t write(int fd, const void* buf, size_t count) {
    using fn_t = ssize_t(*)(int, const void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, write);
    if (!real_write) return syscall(SYS_write, fd, buf, count);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, count, vm);
    if (in_heap) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = real_write(fd, buf, count);
    if (in_heap) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    using fn_t = ssize_t(*)(int, void*, size_t, off_t);
    SMASH_LAZY_RESOLVE(fn_t, pread);
    if (!real_pread) return syscall(SYS_pread64, fd, buf, count, offset);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, count, vm);
    if (in_heap) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = real_pread(fd, buf, count, offset);
    if (in_heap) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    using fn_t = ssize_t(*)(int, const void*, size_t, off_t);
    SMASH_LAZY_RESOLVE(fn_t, pwrite);
    if (!real_pwrite) return syscall(SYS_pwrite64, fd, buf, count, offset);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, count, vm);
    if (in_heap) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = real_pwrite(fd, buf, count, offset);
    if (in_heap) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int);
    SMASH_LAZY_RESOLVE(fn_t, readv);
    if (!real_readv) return syscall(SYS_readv, fd, iov, iovcnt);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = iovecInHeap(iov, iovcnt, vm);
    if (in_heap) { warmIovecLinux(iov, iovcnt, vm); smash::vm::pinIovec(iov, iovcnt, vm); }
    ssize_t ret = real_readv(fd, iov, iovcnt);
    if (in_heap) smash::vm::unpinIovec(iov, iovcnt, vm);
    return ret;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int);
    SMASH_LAZY_RESOLVE(fn_t, writev);
    if (!real_writev) return syscall(SYS_writev, fd, iov, iovcnt);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = iovecInHeap(iov, iovcnt, vm);
    if (in_heap) { warmIovecLinux(iov, iovcnt, vm); smash::vm::pinIovec(iov, iovcnt, vm); }
    ssize_t ret = real_writev(fd, iov, iovcnt);
    if (in_heap) smash::vm::unpinIovec(iov, iovcnt, vm);
    return ret;
}

ssize_t recv(int s, void* buf, size_t len, int flags) {
    using fn_t = ssize_t(*)(int, void*, size_t, int);
    SMASH_LAZY_RESOLVE(fn_t, recv);
    if (!real_recv) return syscall(SYS_recvfrom, s, buf, len, flags, nullptr, nullptr);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, len, vm);
    if (in_heap) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = real_recv(s, buf, len, flags);
    if (in_heap) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

ssize_t send(int s, const void* buf, size_t len, int flags) {
    using fn_t = ssize_t(*)(int, const void*, size_t, int);
    SMASH_LAZY_RESOLVE(fn_t, send);
    if (!real_send) return syscall(SYS_sendto, s, buf, len, flags, nullptr, 0);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, len, vm);
    if (in_heap) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = real_send(s, buf, len, flags);
    if (in_heap) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

ssize_t recvfrom(int s, void* buf, size_t len, int flags,
                 struct sockaddr* from, socklen_t* fromlen) {
    using fn_t = ssize_t(*)(int, void*, size_t, int, struct sockaddr*, socklen_t*);
    SMASH_LAZY_RESOLVE(fn_t, recvfrom);
    if (!real_recvfrom) return syscall(SYS_recvfrom, s, buf, len, flags, from, fromlen);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, len, vm);
    if (in_heap) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = real_recvfrom(s, buf, len, flags, from, fromlen);
    if (in_heap) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

ssize_t sendto(int s, const void* buf, size_t len, int flags,
               const struct sockaddr* to, socklen_t tolen) {
    using fn_t = ssize_t(*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
    SMASH_LAZY_RESOLVE(fn_t, sendto);
    if (!real_sendto) return syscall(SYS_sendto, s, buf, len, flags, to, tolen);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = bufferInHeap(buf, len, vm);
    if (in_heap) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = real_sendto(s, buf, len, flags, to, tolen);
    if (in_heap) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

ssize_t recvmsg(int s, struct msghdr* msg, int flags) {
    using fn_t = ssize_t(*)(int, struct msghdr*, int);
    SMASH_LAZY_RESOLVE(fn_t, recvmsg);
    if (!real_recvmsg) return syscall(SYS_recvmsg, s, msg, flags);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = msg && iovecInHeap(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    if (in_heap) {
        warmIovecLinux(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
        smash::vm::pinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    }
    ssize_t ret = real_recvmsg(s, msg, flags);
    if (in_heap)
        smash::vm::unpinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    return ret;
}

ssize_t sendmsg(int s, const struct msghdr* msg, int flags) {
    using fn_t = ssize_t(*)(int, const struct msghdr*, int);
    SMASH_LAZY_RESOLVE(fn_t, sendmsg);
    if (!real_sendmsg) return syscall(SYS_sendmsg, s, msg, flags);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = msg && iovecInHeap(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    if (in_heap) {
        warmIovecLinux(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
        smash::vm::pinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    }
    ssize_t ret = real_sendmsg(s, msg, flags);
    if (in_heap)
        smash::vm::unpinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    return ret;
}

// Note: poll() is NOT intercepted because:
// 1. pollfd arrays are typically small and stack-allocated
// 2. Intercepting poll/epoll causes issues with libevent-based apps

// Note: epoll_wait/epoll_pwait are NOT intercepted because:
// 1. Event buffers are typically small and stack-allocated or in libevent's mmap'd memory
// 2. They are not allocated through malloc, so not Smash-managed
// 3. Intercepting them causes issues with libevent-based apps (memcached, redis)
// The kernel will return EFAULT if it can't access the buffer, which is handled by the app.

} // extern "C"
#pragma GCC visibility pop

#endif // __linux__
