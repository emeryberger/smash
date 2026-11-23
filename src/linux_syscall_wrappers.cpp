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

// _GNU_SOURCE is needed for `struct statx` in <sys/stat.h>.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "smash_heap.h"
#include "vm/syscall_compat.h"
#include "external_track.h"

#include <atomic>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/random.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/xattr.h>
#include <sys/mman.h>
#include <linux/userfaultfd.h>
#include <linux/types.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <limits.h>
#include <malloc.h>

// Recursion guard: if dlsym triggers one of our wrappers, skip the
// dlsym attempt and let the wrapper fall through to raw syscall().
// This is safe because during early init no pages are compressed.
static std::atomic<bool> g_resolving_syscalls{false};

// Global disable: SMASH_NO_SYSCALL_WRAP=1 makes all wrappers pass-through
static const bool g_syscall_wrap_disabled = [] {
    const char* v = std::getenv("SMASH_NO_SYSCALL_WRAP");
    return v && v[0] == '1';
}();

// Lazy-resolve a real function pointer via dlsym(RTLD_NEXT).
// If we're in a dlsym recursion, leaves the pointer as nullptr
// so the caller falls through to its raw syscall() path.
#define SMASH_LAZY_RESOLVE(fn_type, name) \
    static fn_type real_##name = nullptr; \
    if (__builtin_expect(!real_##name, 0)) { \
        if (!g_resolving_syscalls.load(std::memory_order_relaxed) && \
            !smash::g_heap_constructing.load(std::memory_order_relaxed)) { \
            g_resolving_syscalls.store(true, std::memory_order_relaxed); \
            real_##name = reinterpret_cast<fn_type>(dlsym(RTLD_NEXT, #name)); \
            g_resolving_syscalls.store(false, std::memory_order_relaxed); \
        } \
    }

// Passthrough macro: if syscall wrapping disabled or real not resolved, call through
#define SMASH_PASSTHROUGH_IF_DISABLED(name, ...) \
    if (g_syscall_wrap_disabled && real_##name) return real_##name(__VA_ARGS__)

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

// Use explicit visibility attribute - pragma doesn't work with -fvisibility=hidden
#define SMASH_VISIBLE __attribute__((visibility("default")))

extern "C" {

SMASH_VISIBLE ssize_t read(int fd, void* buf, size_t count) {
    using fn_t = ssize_t(*)(int, void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, read);
    if (!real_read) return syscall(SYS_read, fd, buf, count);
    SMASH_PASSTHROUGH_IF_DISABLED(read, fd, buf, count);
    return smash::vm::retryWith1Buf(
        [&] { return real_read(fd, buf, count); },
        buf, count);
}

SMASH_VISIBLE ssize_t write(int fd, const void* buf, size_t count) {
    using fn_t = ssize_t(*)(int, const void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, write);
    if (!real_write) return syscall(SYS_write, fd, buf, count);
    SMASH_PASSTHROUGH_IF_DISABLED(write, fd, buf, count);
    return smash::vm::retryWith1Buf(
        [&] { return real_write(fd, buf, count); },
        buf, count);
}

SMASH_VISIBLE ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    using fn_t = ssize_t(*)(int, void*, size_t, off_t);
    SMASH_LAZY_RESOLVE(fn_t, pread);
    if (!real_pread) return syscall(SYS_pread64, fd, buf, count, offset);
    return smash::vm::retryWith1Buf(
        [&] { return real_pread(fd, buf, count, offset); },
        buf, count);
}

SMASH_VISIBLE ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    using fn_t = ssize_t(*)(int, const void*, size_t, off_t);
    SMASH_LAZY_RESOLVE(fn_t, pwrite);
    if (!real_pwrite) return syscall(SYS_pwrite64, fd, buf, count, offset);
    return smash::vm::retryWith1Buf(
        [&] { return real_pwrite(fd, buf, count, offset); },
        buf, count);
}

SMASH_VISIBLE ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int);
    SMASH_LAZY_RESOLVE(fn_t, readv);
    if (!real_readv) return syscall(SYS_readv, fd, iov, iovcnt);
    return smash::vm::retryWithIovec(
        [&] { return real_readv(fd, iov, iovcnt); },
        iov, iovcnt);
}

SMASH_VISIBLE ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int);
    SMASH_LAZY_RESOLVE(fn_t, writev);
    if (!real_writev) return syscall(SYS_writev, fd, iov, iovcnt);
    return smash::vm::retryWithIovec(
        [&] { return real_writev(fd, iov, iovcnt); },
        iov, iovcnt);
}

SMASH_VISIBLE ssize_t recv(int s, void* buf, size_t len, int flags) {
    using fn_t = ssize_t(*)(int, void*, size_t, int);
    SMASH_LAZY_RESOLVE(fn_t, recv);
    if (!real_recv) return syscall(SYS_recvfrom, s, buf, len, flags, nullptr, nullptr);
    return smash::vm::retryWith1Buf(
        [&] { return real_recv(s, buf, len, flags); },
        buf, len);
}

SMASH_VISIBLE ssize_t send(int s, const void* buf, size_t len, int flags) {
    using fn_t = ssize_t(*)(int, const void*, size_t, int);
    SMASH_LAZY_RESOLVE(fn_t, send);
    if (!real_send) return syscall(SYS_sendto, s, buf, len, flags, nullptr, 0);
    return smash::vm::retryWith1Buf(
        [&] { return real_send(s, buf, len, flags); },
        buf, len);
}

SMASH_VISIBLE ssize_t recvfrom(int s, void* buf, size_t len, int flags,
                 struct sockaddr* from, socklen_t* fromlen) {
    using fn_t = ssize_t(*)(int, void*, size_t, int, struct sockaddr*, socklen_t*);
    SMASH_LAZY_RESOLVE(fn_t, recvfrom);
    if (!real_recvfrom) return syscall(SYS_recvfrom, s, buf, len, flags, from, fromlen);
    return smash::vm::retryWith1Buf(
        [&] { return real_recvfrom(s, buf, len, flags, from, fromlen); },
        buf, len);
}

SMASH_VISIBLE ssize_t sendto(int s, const void* buf, size_t len, int flags,
               const struct sockaddr* to, socklen_t tolen) {
    using fn_t = ssize_t(*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
    SMASH_LAZY_RESOLVE(fn_t, sendto);
    if (!real_sendto) return syscall(SYS_sendto, s, buf, len, flags, to, tolen);
    return smash::vm::retryWith1Buf(
        [&] { return real_sendto(s, buf, len, flags, to, tolen); },
        buf, len);
}

SMASH_VISIBLE ssize_t recvmsg(int s, struct msghdr* msg, int flags) {
    using fn_t = ssize_t(*)(int, struct msghdr*, int);
    SMASH_LAZY_RESOLVE(fn_t, recvmsg);
    if (!real_recvmsg) return syscall(SYS_recvmsg, s, msg, flags);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = msg && iovecInHeap(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    ssize_t ret = smash::vm::retryWithDecompress(
        [&] { return real_recvmsg(s, msg, flags); },
        [&] { if (in_heap) smash::vm::walkIovecForFault(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm); });
    return ret;
}

SMASH_VISIBLE ssize_t sendmsg(int s, const struct msghdr* msg, int flags) {
    using fn_t = ssize_t(*)(int, const struct msghdr*, int);
    SMASH_LAZY_RESOLVE(fn_t, sendmsg);
    if (!real_sendmsg) return syscall(SYS_sendmsg, s, msg, flags);
    auto* vm = smash::g_smash_vm_region;
    bool in_heap = msg && iovecInHeap(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    ssize_t ret = smash::vm::retryWithDecompress(
        [&] { return real_sendmsg(s, msg, flags); },
        [&] { if (in_heap) smash::vm::walkIovecForFault(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm); });
    return ret;
}

// poll/ppoll - GLib (used by GTK/Firefox) heap-allocates pollfd arrays in its
// main loop. The kernel writes revents back into the array, so if those pages
// are PROT_NONE or PROT_READ, the kernel returns EFAULT.
SMASH_VISIBLE int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    using fn_t = int(*)(struct pollfd*, nfds_t, int);
    SMASH_LAZY_RESOLVE(fn_t, poll);
    if (!real_poll) {
#ifdef SYS_poll
        return syscall(SYS_poll, fds, nfds, timeout);
#else
        // arm64 / riscv have no SYS_poll; route via ppoll.
        struct timespec ts, *tsp = nullptr;
        if (timeout >= 0) {
            ts.tv_sec  = timeout / 1000;
            ts.tv_nsec = static_cast<long>(timeout % 1000) * 1000000;
            tsp = &ts;
        }
        return syscall(SYS_ppoll, fds, nfds, tsp, nullptr, 0);
#endif
    }
    auto* vm = smash::g_smash_vm_region;
    size_t size = static_cast<size_t>(nfds) * sizeof(struct pollfd);
    bool in_heap = bufferInHeap(fds, size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_poll(fds, nfds, timeout); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(fds, size, vm); });
    return ret;
}

SMASH_VISIBLE int ppoll(struct pollfd* fds, nfds_t nfds,
                        const struct timespec* tmo_p, const sigset_t* sigmask) {
    using fn_t = int(*)(struct pollfd*, nfds_t, const struct timespec*, const sigset_t*);
    SMASH_LAZY_RESOLVE(fn_t, ppoll);
    if (!real_ppoll) return syscall(SYS_ppoll, fds, nfds, tmo_p, sigmask);
    auto* vm = smash::g_smash_vm_region;
    size_t size = static_cast<size_t>(nfds) * sizeof(struct pollfd);
    bool in_heap = bufferInHeap(fds, size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_ppoll(fds, nfds, tmo_p, sigmask); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(fds, size, vm); });
    return ret;
}

// ── select/pselect6 ─────────────────────────────────────────────────────────
// Firefox/GLib and many event loops use select(). The kernel reads/writes
// fd_set buffers; if those pages are protected we get EFAULT.
// fd_set is typically stack-allocated but may be heap-allocated for large sets.
SMASH_VISIBLE int select(int nfds, fd_set* readfds, fd_set* writefds,
                         fd_set* exceptfds, struct timeval* timeout) {
    using fn_t = int(*)(int, fd_set*, fd_set*, fd_set*, struct timeval*);
    SMASH_LAZY_RESOLVE(fn_t, select);
    if (!real_select) {
#ifdef SYS_select
        return syscall(SYS_select, nfds, readfds, writefds, exceptfds, timeout);
#else
        // arm64 / riscv have no SYS_select; route via pselect6.
        struct timespec ts, *tsp = nullptr;
        if (timeout) {
            ts.tv_sec  = timeout->tv_sec;
            ts.tv_nsec = timeout->tv_usec * 1000;
            tsp = &ts;
        }
        return syscall(SYS_pselect6, nfds, readfds, writefds, exceptfds, tsp, nullptr);
#endif
    }
    auto* vm = smash::g_smash_vm_region;
    // fd_set size depends on nfds but is bounded by sizeof(fd_set)
    size_t set_size = sizeof(fd_set);
    bool pin_r = readfds && bufferInHeap(readfds, set_size, vm);
    bool pin_w = writefds && bufferInHeap(writefds, set_size, vm);
    bool pin_e = exceptfds && bufferInHeap(exceptfds, set_size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_select(nfds, readfds, writefds, exceptfds, timeout); },
        [&] {
            if (pin_r) smash::vm::walkPagesForFault(readfds, set_size, vm);
            if (pin_w) smash::vm::walkPagesForFault(writefds, set_size, vm);
            if (pin_e) smash::vm::walkPagesForFault(exceptfds, set_size, vm);
        });
    return ret;
}

SMASH_VISIBLE int pselect(int nfds, fd_set* readfds, fd_set* writefds,
                          fd_set* exceptfds, const struct timespec* timeout,
                          const sigset_t* sigmask) {
    using fn_t = int(*)(int, fd_set*, fd_set*, fd_set*, const struct timespec*, const sigset_t*);
    SMASH_LAZY_RESOLVE(fn_t, pselect);
    if (!real_pselect) return syscall(SYS_pselect6, nfds, readfds, writefds, exceptfds, timeout, sigmask);
    auto* vm = smash::g_smash_vm_region;
    size_t set_size = sizeof(fd_set);
    bool pin_r = readfds && bufferInHeap(readfds, set_size, vm);
    bool pin_w = writefds && bufferInHeap(writefds, set_size, vm);
    bool pin_e = exceptfds && bufferInHeap(exceptfds, set_size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_pselect(nfds, readfds, writefds, exceptfds, timeout, sigmask); },
        [&] {
            if (pin_r) smash::vm::walkPagesForFault(readfds, set_size, vm);
            if (pin_w) smash::vm::walkPagesForFault(writefds, set_size, vm);
            if (pin_e) smash::vm::walkPagesForFault(exceptfds, set_size, vm);
        });
    return ret;
}

// ── accept/accept4 ──────────────────────────────────────────────────────────
// The kernel writes the peer address into a heap-allocated sockaddr buffer.
SMASH_VISIBLE int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    using fn_t = int(*)(int, struct sockaddr*, socklen_t*);
    SMASH_LAZY_RESOLVE(fn_t, accept);
    if (!real_accept) return syscall(SYS_accept, sockfd, addr, addrlen);
    auto* vm = smash::g_smash_vm_region;
    size_t addr_size = (addr && addrlen) ? static_cast<size_t>(*addrlen) : 0;
    bool in_heap = addr_size && bufferInHeap(addr, addr_size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_accept(sockfd, addr, addrlen); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(addr, addr_size, vm); });
    return ret;
}

SMASH_VISIBLE int accept4(int sockfd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
    using fn_t = int(*)(int, struct sockaddr*, socklen_t*, int);
    SMASH_LAZY_RESOLVE(fn_t, accept4);
    if (!real_accept4) return syscall(SYS_accept4, sockfd, addr, addrlen, flags);
    auto* vm = smash::g_smash_vm_region;
    size_t addr_size = (addr && addrlen) ? static_cast<size_t>(*addrlen) : 0;
    bool in_heap = addr_size && bufferInHeap(addr, addr_size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_accept4(sockfd, addr, addrlen, flags); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(addr, addr_size, vm); });
    return ret;
}

// ── recvmmsg/sendmmsg ───────────────────────────────────────────────────────
// Batch message I/O. Each mmsghdr contains a msghdr with iovecs.
SMASH_VISIBLE int recvmmsg(int sockfd, struct mmsghdr* msgvec, unsigned int vlen,
                           int flags, struct timespec* timeout) {
    using fn_t = int(*)(int, struct mmsghdr*, unsigned int, int, struct timespec*);
    SMASH_LAZY_RESOLVE(fn_t, recvmmsg);
    if (!real_recvmmsg) return syscall(SYS_recvmmsg, sockfd, msgvec, vlen, flags, timeout);
    auto* vm = smash::g_smash_vm_region;
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_recvmmsg(sockfd, msgvec, vlen, flags, timeout); },
        [&] {
            for (unsigned i = 0; i < vlen; ++i) {
                struct msghdr* msg = &msgvec[i].msg_hdr;
                if (msg->msg_iov && msg->msg_iovlen > 0)
                    smash::vm::walkIovecForFault(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
            }
        });
    return ret;
}

SMASH_VISIBLE int sendmmsg(int sockfd, struct mmsghdr* msgvec, unsigned int vlen,
                           int flags) {
    using fn_t = int(*)(int, struct mmsghdr*, unsigned int, int);
    SMASH_LAZY_RESOLVE(fn_t, sendmmsg);
    if (!real_sendmmsg) return syscall(SYS_sendmmsg, sockfd, msgvec, vlen, flags);
    auto* vm = smash::g_smash_vm_region;
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_sendmmsg(sockfd, msgvec, vlen, flags); },
        [&] {
            for (unsigned i = 0; i < vlen; ++i) {
                struct msghdr* msg = &msgvec[i].msg_hdr;
                if (msg->msg_iov && msg->msg_iovlen > 0)
                    smash::vm::walkIovecForFault(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
            }
        });
    return ret;
}

// ── Socket address query syscalls ───────────────────────────────────────────
// getsockopt writes into a heap-allocated buffer; getsockname/getpeername
// write into a sockaddr buffer. All can EFAULT on protected pages.
SMASH_VISIBLE int getsockopt(int sockfd, int level, int optname,
                             void* optval, socklen_t* optlen) {
    using fn_t = int(*)(int, int, int, void*, socklen_t*);
    SMASH_LAZY_RESOLVE(fn_t, getsockopt);
    if (!real_getsockopt) return syscall(SYS_getsockopt, sockfd, level, optname, optval, optlen);
    auto* vm = smash::g_smash_vm_region;
    size_t val_size = (optval && optlen) ? static_cast<size_t>(*optlen) : 0;
    bool in_heap = val_size && bufferInHeap(optval, val_size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_getsockopt(sockfd, level, optname, optval, optlen); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(optval, val_size, vm); });
    return ret;
}

SMASH_VISIBLE int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    using fn_t = int(*)(int, struct sockaddr*, socklen_t*);
    SMASH_LAZY_RESOLVE(fn_t, getsockname);
    if (!real_getsockname) return syscall(SYS_getsockname, sockfd, addr, addrlen);
    auto* vm = smash::g_smash_vm_region;
    size_t addr_size = (addr && addrlen) ? static_cast<size_t>(*addrlen) : 0;
    bool in_heap = addr_size && bufferInHeap(addr, addr_size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_getsockname(sockfd, addr, addrlen); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(addr, addr_size, vm); });
    return ret;
}

SMASH_VISIBLE int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    using fn_t = int(*)(int, struct sockaddr*, socklen_t*);
    SMASH_LAZY_RESOLVE(fn_t, getpeername);
    if (!real_getpeername) return syscall(SYS_getpeername, sockfd, addr, addrlen);
    auto* vm = smash::g_smash_vm_region;
    size_t addr_size = (addr && addrlen) ? static_cast<size_t>(*addrlen) : 0;
    bool in_heap = addr_size && bufferInHeap(addr, addr_size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_getpeername(sockfd, addr, addrlen); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(addr, addr_size, vm); });
    return ret;
}

// ── fstat family / fstatfs ──────────────────────────────────────────────────
// glibc's fstat ABI is a moving target:
//   - glibc < 2.33: <sys/stat.h> declares `fstat` as a static inline that
//     calls `__fxstat(_STAT_VER, fd, buf)`. Linker only sees __fxstat.
//   - glibc ≥ 2.33: `fstat` is a real exported symbol, no inline stub.
//   - With _FILE_OFFSET_BITS=64 (Ubuntu's default for C++ programs):
//       <sys/stat.h> does `#define fstat fstat64`, so callers actually
//       link against `fstat64` (or `__fxstat64` on old glibc).
// To intercept reliably across all combinations, wrap every variant. Each
// is a one-line shell over retryWith1Buf with the appropriate struct size.

SMASH_VISIBLE int fstat(int fd, struct stat* st) {
    using fn_t = int(*)(int, struct stat*);
    SMASH_LAZY_RESOLVE(fn_t, fstat);
    if (!real_fstat) return syscall(SYS_fstat, fd, st);
    return smash::vm::retryWith1Buf(
        [&] { return real_fstat(fd, st); },
        st, sizeof(struct stat));
}

// fstat64 path: with _FILE_OFFSET_BITS=64 + LFS, glibc's preprocessor
// rewrites fstat→fstat64 at the call site. The signature must match
// glibc's declaration in <sys/stat.h>; struct stat64 has the same layout
// as struct stat on x86_64 modern glibc.
SMASH_VISIBLE int fstat64(int fd, struct stat64* st) {
    using fn_t = int(*)(int, struct stat64*);
    SMASH_LAZY_RESOLVE(fn_t, fstat64);
    if (!real_fstat64) return syscall(SYS_fstat, fd, st);
    return smash::vm::retryWith1Buf(
        [&] { return real_fstat64(fd, st); },
        st, sizeof(struct stat64));
}

// __fxstat(version, fd, buf) — glibc < 2.33 internal symbol that the old
// inline `fstat` calls into. Modern glibc still exports a deprecated stub
// for ABI compatibility.
SMASH_VISIBLE int __fxstat(int ver, int fd, struct stat* st) {
    using fn_t = int(*)(int, int, struct stat*);
    SMASH_LAZY_RESOLVE(fn_t, __fxstat);
    if (!real___fxstat) return syscall(SYS_fstat, fd, st);
    return smash::vm::retryWith1Buf(
        [&] { return real___fxstat(ver, fd, st); },
        st, sizeof(struct stat));
}

SMASH_VISIBLE int __fxstat64(int ver, int fd, struct stat64* st) {
    using fn_t = int(*)(int, int, struct stat64*);
    SMASH_LAZY_RESOLVE(fn_t, __fxstat64);
    if (!real___fxstat64) return syscall(SYS_fstat, fd, st);
    return smash::vm::retryWith1Buf(
        [&] { return real___fxstat64(ver, fd, st); },
        st, sizeof(struct stat64));
}

SMASH_VISIBLE int fstatfs(int fd, struct statfs* st) {
    using fn_t = int(*)(int, struct statfs*);
    SMASH_LAZY_RESOLVE(fn_t, fstatfs);
    if (!real_fstatfs) return syscall(SYS_fstatfs, fd, st);
    return smash::vm::retryWith1Buf(
        [&] { return real_fstatfs(fd, st); },
        st, sizeof(struct statfs));
}

// ── stat / lstat / fstatat / statx ──────────────────────────────────────────
// Same hazard as fstat: kernel writes a struct (stat / statx) into a
// userspace buffer that may be heap-allocated and compressed. Cover the
// same ABI surface — modern symbol, _64 LFS variant, and the legacy
// __xstat / __lxstat / __fxstatat pre-2.33 entry points.

// Fall back via SYS_newfstatat (works on every modern arch including
// arm64/riscv where SYS_stat doesn't exist).
SMASH_VISIBLE int stat(const char* path, struct stat* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(const char*, struct stat*);
    SMASH_LAZY_RESOLVE(fn_t, stat);
    if (!real_stat) return syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
    return smash::vm::retryWith1Buf(
        [&] { return real_stat(path, st); },
        st, sizeof(struct stat));
}

SMASH_VISIBLE int lstat(const char* path, struct stat* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(const char*, struct stat*);
    SMASH_LAZY_RESOLVE(fn_t, lstat);
    if (!real_lstat) return syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
    return smash::vm::retryWith1Buf(
        [&] { return real_lstat(path, st); },
        st, sizeof(struct stat));
}

SMASH_VISIBLE int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, const char*, struct stat*, int);
    SMASH_LAZY_RESOLVE(fn_t, fstatat);
    if (!real_fstatat) return syscall(SYS_newfstatat, dirfd, path, st, flags);
    return smash::vm::retryWith1Buf(
        [&] { return real_fstatat(dirfd, path, st, flags); },
        st, sizeof(struct stat));
}

SMASH_VISIBLE int stat64(const char* path, struct stat64* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(const char*, struct stat64*);
    SMASH_LAZY_RESOLVE(fn_t, stat64);
    if (!real_stat64) return syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
    return smash::vm::retryWith1Buf(
        [&] { return real_stat64(path, st); },
        st, sizeof(struct stat64));
}

SMASH_VISIBLE int lstat64(const char* path, struct stat64* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(const char*, struct stat64*);
    SMASH_LAZY_RESOLVE(fn_t, lstat64);
    if (!real_lstat64) return syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
    return smash::vm::retryWith1Buf(
        [&] { return real_lstat64(path, st); },
        st, sizeof(struct stat64));
}

SMASH_VISIBLE int fstatat64(int dirfd, const char* path, struct stat64* st, int flags) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, const char*, struct stat64*, int);
    SMASH_LAZY_RESOLVE(fn_t, fstatat64);
    if (!real_fstatat64) return syscall(SYS_newfstatat, dirfd, path, st, flags);
    return smash::vm::retryWith1Buf(
        [&] { return real_fstatat64(dirfd, path, st, flags); },
        st, sizeof(struct stat64));
}

// __xstat / __lxstat / __fxstatat (and _64 variants): glibc < 2.33 internal
// symbols that the old stat / lstat / fstatat inlines forwarded to.
SMASH_VISIBLE int __xstat(int ver, const char* path, struct stat* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, const char*, struct stat*);
    SMASH_LAZY_RESOLVE(fn_t, __xstat);
    if (!real___xstat) return syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
    return smash::vm::retryWith1Buf(
        [&] { return real___xstat(ver, path, st); },
        st, sizeof(struct stat));
}

SMASH_VISIBLE int __lxstat(int ver, const char* path, struct stat* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, const char*, struct stat*);
    SMASH_LAZY_RESOLVE(fn_t, __lxstat);
    if (!real___lxstat) return syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
    return smash::vm::retryWith1Buf(
        [&] { return real___lxstat(ver, path, st); },
        st, sizeof(struct stat));
}

SMASH_VISIBLE int __fxstatat(int ver, int dirfd, const char* path, struct stat* st, int flags) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, int, const char*, struct stat*, int);
    SMASH_LAZY_RESOLVE(fn_t, __fxstatat);
    if (!real___fxstatat) return syscall(SYS_newfstatat, dirfd, path, st, flags);
    return smash::vm::retryWith1Buf(
        [&] { return real___fxstatat(ver, dirfd, path, st, flags); },
        st, sizeof(struct stat));
}

SMASH_VISIBLE int __xstat64(int ver, const char* path, struct stat64* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, const char*, struct stat64*);
    SMASH_LAZY_RESOLVE(fn_t, __xstat64);
    if (!real___xstat64) return syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
    return smash::vm::retryWith1Buf(
        [&] { return real___xstat64(ver, path, st); },
        st, sizeof(struct stat64));
}

SMASH_VISIBLE int __lxstat64(int ver, const char* path, struct stat64* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, const char*, struct stat64*);
    SMASH_LAZY_RESOLVE(fn_t, __lxstat64);
    if (!real___lxstat64) return syscall(SYS_newfstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
    return smash::vm::retryWith1Buf(
        [&] { return real___lxstat64(ver, path, st); },
        st, sizeof(struct stat64));
}

SMASH_VISIBLE int __fxstatat64(int ver, int dirfd, const char* path, struct stat64* st, int flags) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, int, const char*, struct stat64*, int);
    SMASH_LAZY_RESOLVE(fn_t, __fxstatat64);
    if (!real___fxstatat64) return syscall(SYS_newfstatat, dirfd, path, st, flags);
    return smash::vm::retryWith1Buf(
        [&] { return real___fxstatat64(ver, dirfd, path, st, flags); },
        st, sizeof(struct stat64));
}

// statx — modern stat replacement (glibc 2.28+). Single-version symbol,
// no GLIBC_2.33 aliasing dance needed.
SMASH_VISIBLE int statx(int dirfd, const char* path, int flags,
                         unsigned int mask, struct statx* buf) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(int, const char*, int, unsigned int, struct statx*);
    SMASH_LAZY_RESOLVE(fn_t, statx);
    if (!real_statx) return syscall(SYS_statx, dirfd, path, flags, mask, buf);
    return smash::vm::retryWith1Buf(
        [&] { return real_statx(dirfd, path, flags, mask, buf); },
        buf, sizeof(struct statx));
}

// ── getdents64 / readlink / readlinkat ──────────────────────────────────────
// getdents64: kernel writes a sequence of struct linux_dirent64 entries into
//   a userspace buffer (typically 32 KiB). readdir() callers in glibc go
//   through __getdents64 internally (not interposable), but direct callers
//   exist. Wrap the public symbol.
// readlink / readlinkat: kernel writes the symlink target string into the
//   user buffer.

SMASH_VISIBLE ssize_t getdents64(int fd, void* dirp, size_t count) {
    using fn_t = ssize_t(*)(int, void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, getdents64);
    if (!real_getdents64) return syscall(SYS_getdents64, fd, dirp, count);
    return smash::vm::retryWith1Buf(
        [&] { return real_getdents64(fd, dirp, count); },
        dirp, count);
}

SMASH_VISIBLE ssize_t readlink(const char* path, char* buf, size_t bufsize) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = ssize_t(*)(const char*, char*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, readlink);
    if (!real_readlink) return syscall(SYS_readlinkat, AT_FDCWD, path, buf, bufsize);
    return smash::vm::retryWith1Buf(
        [&] { return real_readlink(path, buf, bufsize); },
        buf, bufsize);
}

SMASH_VISIBLE ssize_t readlinkat(int dirfd, const char* path,
                                  char* buf, size_t bufsize) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = ssize_t(*)(int, const char*, char*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, readlinkat);
    if (!real_readlinkat) return syscall(SYS_readlinkat, dirfd, path, buf, bufsize);
    return smash::vm::retryWith1Buf(
        [&] { return real_readlinkat(dirfd, path, buf, bufsize); },
        buf, bufsize);
}

// ── preadv / pwritev / preadv2 / pwritev2 ───────────────────────────────────
// Positioned vectored I/O. preadv / pwritev added in glibc 2.10 (2009);
// preadv2 / pwritev2 added in glibc 2.26 (2017). The kernel reads from /
// writes to each iovec entry; on a compressed page the syscall returns
// EFAULT exactly like read/writev.

SMASH_VISIBLE ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int, off_t);
    SMASH_LAZY_RESOLVE(fn_t, preadv);
    if (!real_preadv) return syscall(SYS_preadv, fd, iov, iovcnt, offset);
    return smash::vm::retryWithIovec(
        [&] { return real_preadv(fd, iov, iovcnt, offset); },
        iov, iovcnt);
}

SMASH_VISIBLE ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int, off_t);
    SMASH_LAZY_RESOLVE(fn_t, pwritev);
    if (!real_pwritev) return syscall(SYS_pwritev, fd, iov, iovcnt, offset);
    return smash::vm::retryWithIovec(
        [&] { return real_pwritev(fd, iov, iovcnt, offset); },
        iov, iovcnt);
}

SMASH_VISIBLE ssize_t preadv2(int fd, const struct iovec* iov, int iovcnt,
                               off_t offset, int flags) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int, off_t, int);
    SMASH_LAZY_RESOLVE(fn_t, preadv2);
    if (!real_preadv2) return syscall(SYS_preadv2, fd, iov, iovcnt, offset, flags);
    return smash::vm::retryWithIovec(
        [&] { return real_preadv2(fd, iov, iovcnt, offset, flags); },
        iov, iovcnt);
}

SMASH_VISIBLE ssize_t pwritev2(int fd, const struct iovec* iov, int iovcnt,
                                off_t offset, int flags) {
    using fn_t = ssize_t(*)(int, const struct iovec*, int, off_t, int);
    SMASH_LAZY_RESOLVE(fn_t, pwritev2);
    if (!real_pwritev2) return syscall(SYS_pwritev2, fd, iov, iovcnt, offset, flags);
    return smash::vm::retryWithIovec(
        [&] { return real_pwritev2(fd, iov, iovcnt, offset, flags); },
        iov, iovcnt);
}

// ── waitid / wait4 (process management) ─────────────────────────────────────
// waitid writes a 128-byte siginfo_t into *infop; wait4 writes int wstatus
// AND struct rusage. wstatus is usually stack (4 bytes); rusage may be heap
// for long-lived accumulating perf counters.

SMASH_VISIBLE int waitid(idtype_t idtype, id_t id, siginfo_t* info, int options) {
    using fn_t = int(*)(idtype_t, id_t, siginfo_t*, int);
    SMASH_LAZY_RESOLVE(fn_t, waitid);
    if (!real_waitid) return syscall(SYS_waitid, idtype, id, info, options, nullptr);
    return smash::vm::retryWith1Buf(
        [&] { return real_waitid(idtype, id, info, options); },
        info, sizeof(siginfo_t));
}

SMASH_VISIBLE pid_t wait4(pid_t pid, int* wstatus, int options, struct rusage* rusage) {
    using fn_t = pid_t(*)(pid_t, int*, int, struct rusage*);
    SMASH_LAZY_RESOLVE(fn_t, wait4);
    if (!real_wait4) return syscall(SYS_wait4, pid, wstatus, options, rusage);
    auto* vm = smash::g_smash_vm_region;
    return smash::vm::retryWithDecompress(
        [&] { return real_wait4(pid, wstatus, options, rusage); },
        [&] {
            if (vm) {
                if (wstatus) smash::vm::walkPagesForFault(wstatus, sizeof(int), vm);
                if (rusage) smash::vm::walkPagesForFault(rusage, sizeof(struct rusage), vm);
            }
        });
}

// ── statvfs / fstatvfs ──────────────────────────────────────────────────────
// POSIX path-form statfs. struct statvfs is similar in size to statfs (~112B);
// heap-allocated when callers track multiple filesystems.
SMASH_VISIBLE int statvfs(const char* path, struct statvfs* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = int(*)(const char*, struct statvfs*);
    SMASH_LAZY_RESOLVE(fn_t, statvfs);
    if (!real_statvfs) {
        errno = ENOSYS;
        return -1;
    }
    return smash::vm::retryWith1Buf(
        [&] { return real_statvfs(path, st); },
        st, sizeof(struct statvfs));
}

SMASH_VISIBLE int fstatvfs(int fd, struct statvfs* st) {
    using fn_t = int(*)(int, struct statvfs*);
    SMASH_LAZY_RESOLVE(fn_t, fstatvfs);
    if (!real_fstatvfs) {
        errno = ENOSYS;
        return -1;
    }
    return smash::vm::retryWith1Buf(
        [&] { return real_fstatvfs(fd, st); },
        st, sizeof(struct statvfs));
}

// ── xattr family ────────────────────────────────────────────────────────────
// getxattr / lgetxattr / fgetxattr write attribute value into user buffer.
// listxattr / llistxattr / flistxattr write a NUL-separated name list.
// Used by SELinux/AppArmor labels, cap_net_raw, mac sandbox metadata, etc.
SMASH_VISIBLE ssize_t getxattr(const char* path, const char* name,
                                void* value, size_t size) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = ssize_t(*)(const char*, const char*, void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, getxattr);
    if (!real_getxattr) return syscall(SYS_getxattr, path, name, value, size);
    return smash::vm::retryWith1Buf(
        [&] { return real_getxattr(path, name, value, size); },
        value, size);
}

SMASH_VISIBLE ssize_t lgetxattr(const char* path, const char* name,
                                 void* value, size_t size) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = ssize_t(*)(const char*, const char*, void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, lgetxattr);
    if (!real_lgetxattr) return syscall(SYS_lgetxattr, path, name, value, size);
    return smash::vm::retryWith1Buf(
        [&] { return real_lgetxattr(path, name, value, size); },
        value, size);
}

SMASH_VISIBLE ssize_t fgetxattr(int fd, const char* name,
                                 void* value, size_t size) {
    using fn_t = ssize_t(*)(int, const char*, void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, fgetxattr);
    if (!real_fgetxattr) return syscall(SYS_fgetxattr, fd, name, value, size);
    return smash::vm::retryWith1Buf(
        [&] { return real_fgetxattr(fd, name, value, size); },
        value, size);
}

SMASH_VISIBLE ssize_t listxattr(const char* path, char* list, size_t size) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = ssize_t(*)(const char*, char*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, listxattr);
    if (!real_listxattr) return syscall(SYS_listxattr, path, list, size);
    return smash::vm::retryWith1Buf(
        [&] { return real_listxattr(path, list, size); },
        list, size);
}

SMASH_VISIBLE ssize_t llistxattr(const char* path, char* list, size_t size) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    using fn_t = ssize_t(*)(const char*, char*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, llistxattr);
    if (!real_llistxattr) return syscall(SYS_llistxattr, path, list, size);
    return smash::vm::retryWith1Buf(
        [&] { return real_llistxattr(path, list, size); },
        list, size);
}

SMASH_VISIBLE ssize_t flistxattr(int fd, char* list, size_t size) {
    using fn_t = ssize_t(*)(int, char*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, flistxattr);
    if (!real_flistxattr) return syscall(SYS_flistxattr, fd, list, size);
    return smash::vm::retryWith1Buf(
        [&] { return real_flistxattr(fd, list, size); },
        list, size);
}

// ── getrusage / prlimit64 ───────────────────────────────────────────────────
// getrusage writes struct rusage; prlimit64 writes (and optionally reads)
// struct rlimit. Both can be heap-allocated when callers persist them.
SMASH_VISIBLE int getrusage(int who, struct rusage* usage) {
    using fn_t = int(*)(int, struct rusage*);
    SMASH_LAZY_RESOLVE(fn_t, getrusage);
    if (!real_getrusage) return syscall(SYS_getrusage, who, usage);
    return smash::vm::retryWith1Buf(
        [&] { return real_getrusage(who, usage); },
        usage, sizeof(struct rusage));
}

// glibc declares prlimit64's resource arg as `enum __rlimit_resource`,
// not plain `int`, so the C-linkage signature must match exactly.
SMASH_VISIBLE int prlimit64(pid_t pid, enum __rlimit_resource resource,
                             const struct rlimit64* new_limit,
                             struct rlimit64* old_limit) {
    using fn_t = int(*)(pid_t, enum __rlimit_resource,
                         const struct rlimit64*, struct rlimit64*);
    SMASH_LAZY_RESOLVE(fn_t, prlimit64);
    if (!real_prlimit64) return syscall(SYS_prlimit64, pid, resource, new_limit, old_limit);
    auto* vm = smash::g_smash_vm_region;
    return smash::vm::retryWithDecompress(
        [&] { return real_prlimit64(pid, resource, new_limit, old_limit); },
        [&] {
            if (vm) {
                if (new_limit)
                    smash::vm::walkPagesForFault(new_limit, sizeof(struct rlimit64), vm);
                if (old_limit)
                    smash::vm::walkPagesForFault(old_limit, sizeof(struct rlimit64), vm);
            }
        });
}

// ── getrandom ───────────────────────────────────────────────────────────────
// The kernel fills a userspace buffer with random bytes. NSS/OpenSSL/Firefox
// use this for cryptographic initialization.
SMASH_VISIBLE ssize_t getrandom(void* buf, size_t buflen, unsigned int flags) {
    using fn_t = ssize_t(*)(void*, size_t, unsigned int);
    SMASH_LAZY_RESOLVE(fn_t, getrandom);
    if (!real_getrandom) return syscall(SYS_getrandom, buf, buflen, flags);
    return smash::vm::retryWith1Buf(
        [&] { return real_getrandom(buf, buflen, flags); },
        buf, buflen);
}

// ── getcwd wrapper ──────────────────────────────────────────────────────────
// getcwd writes the current working directory path into a user-provided buffer.
// Uses the standard EFAULT-retry pattern like other syscall wrappers.

SMASH_VISIBLE char* getcwd(char* buf, size_t size) {
    using fn_t = char*(*)(char*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, getcwd);

    // For the buf==NULL case, glibc allocates a buffer internally that may
    // land in a smash-managed page. The kernel can't take a SIGSEGV when
    // copy_to_user hits a protected page during the syscall — it returns
    // EFAULT instead. Without a buf to walk, we can't proactively warm.
    //
    // Workaround: use a stack buffer ourselves, then strdup the result so
    // the C++ filesystem layer's getcwd(NULL,0) idiom keeps working. The
    // stack page is already PROT_RW and unmanaged by smash. Apply the
    // standard EFAULT-retry pattern in case the stack itself is somehow
    // protected (shouldn't happen, but harmless).
    auto* vm = smash::g_smash_vm_region;
    if (!buf) {
        char tmp[PATH_MAX];
        auto do_getcwd_tmp = [&]() -> char* {
            if (!real_getcwd) {
                return (syscall(SYS_getcwd, tmp, sizeof(tmp)) < 0) ? nullptr : tmp;
            }
            return real_getcwd(tmp, sizeof(tmp));
        };
        char* ret = do_getcwd_tmp();
        long backoff_ns = 1000;
        for (int attempt = 0; ret == nullptr && errno == EFAULT && attempt < 8; ++attempt) {
            if (vm) smash::vm::walkPagesForFault(tmp, sizeof(tmp), vm);
            if (attempt > 0) {
                struct timespec ts = {0, backoff_ns};
                nanosleep(&ts, nullptr);
                backoff_ns *= 2;
            }
            ret = do_getcwd_tmp();
        }
        if (!ret) return nullptr;
        return strdup(ret);
    }

    // Standard retry pattern when caller supplied a buffer.
    auto do_getcwd = [&]() -> char* {
        if (!real_getcwd) {
            return (syscall(SYS_getcwd, buf, size) < 0) ? nullptr : buf;
        }
        return real_getcwd(buf, size);
    };

    char* ret = do_getcwd();
    long backoff_ns = 1000;
    for (int attempt = 0; ret == nullptr && errno == EFAULT && attempt < 8; ++attempt) {
        if (vm && size) smash::vm::walkPagesForFault(buf, size, vm);
        if (attempt > 0) {
            struct timespec ts = {0, backoff_ns};
            nanosleep(&ts, nullptr);
            backoff_ns *= 2;
        }
        ret = do_getcwd();
    }
    return ret;
}

// Legacy getcwd hooks removed - the direct wrapper above handles everything.

// ── Buffered I/O wrappers ───────────────────────────────────────────────────
// glibc's fread/fgets/fgetc call __read() internally without going through
// the PLT, so our read() wrapper never sees those calls.  We interpose the
// higher-level functions to warm+pin the FILE's internal buffer (where the
// kernel writes via copyin/copyout) before passing through to the real impl.
//
// glibc FILE struct: _IO_buf_base .. _IO_buf_end is the reserve area.

static inline void warmGlibcFileBuffer(FILE* stream, smash::VmRegion* vm) {
    if (!stream || !vm) return;
    char* base = stream->_IO_buf_base;
    char* end  = stream->_IO_buf_end;
    if (base && end > base) {
        size_t sz = static_cast<size_t>(end - base);
        if (bufferInHeap(base, sz, vm)) {
            smash::vm::warmPages(base, sz, vm);
        }
    }
}

// The stdio buffer is malloc'd lazily on a stream's first read — INSIDE
// glibc, after the warm above — and in full mode it can land on a smash page
// that is compressed, so glibc's internal read() into it fails with EFAULT
// and the stream reports an error (Valkey: "Short read or OOM loading DB" on
// every DEBUG RELOAD in full mode, with or without SMASH_TRACK_EXTERNAL).
// When the stream had no buffer before the call and the call failed with
// EFAULT, nothing was delivered or consumed (the first fill failed): warm the
// new buffer, clear the error and retry once.
static inline bool firstFillFaulted(FILE* stream, bool had_buf) {
    if (!stream || had_buf || !stream->_IO_buf_base) return false;
    if (!ferror(stream) || errno != EFAULT) return false;
    clearerr(stream);
    return true;
}

SMASH_VISIBLE size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    using fn_t = size_t(*)(void*, size_t, size_t, FILE*);
    SMASH_LAZY_RESOLVE(fn_t, fread);
    if (!real_fread) return 0;
    auto* vm = smash::g_smash_vm_region;
    size_t total = size * nmemb;
    bool pin_buf = total && bufferInHeap(ptr, total, vm);
    if (pin_buf) smash::vm::warmPages(ptr, total, vm);
    warmGlibcFileBuffer(stream, vm);
    const bool had_buf = stream && stream->_IO_buf_base;
    size_t ret = real_fread(ptr, size, nmemb, stream);
    if (ret < nmemb && firstFillFaulted(stream, had_buf)) {
        warmGlibcFileBuffer(stream, vm);
        if (pin_buf) smash::vm::warmPages(ptr, total, vm);
        ret = real_fread(ptr, size, nmemb, stream);
    }
    return ret;
}

SMASH_VISIBLE char* fgets(char* s, int size, FILE* stream) {
    using fn_t = char*(*)(char*, int, FILE*);
    SMASH_LAZY_RESOLVE(fn_t, fgets);
    if (!real_fgets) return nullptr;
    auto* vm = smash::g_smash_vm_region;
    bool pin_s = (s && size > 0) && bufferInHeap(s, static_cast<size_t>(size), vm);
    if (pin_s) smash::vm::warmPages(s, size, vm);
    warmGlibcFileBuffer(stream, vm);
    const bool had_buf = stream && stream->_IO_buf_base;
    char* ret = real_fgets(s, size, stream);
    if (!ret && firstFillFaulted(stream, had_buf)) {
        warmGlibcFileBuffer(stream, vm);
        ret = real_fgets(s, size, stream);
    }
    return ret;
}

SMASH_VISIBLE int fgetc(FILE* stream) {
    using fn_t = int(*)(FILE*);
    SMASH_LAZY_RESOLVE(fn_t, fgetc);
    if (!real_fgetc) return EOF;
    auto* vm = smash::g_smash_vm_region;
    warmGlibcFileBuffer(stream, vm);
    const bool had_buf = stream && stream->_IO_buf_base;
    int ret = real_fgetc(stream);
    if (ret == EOF && firstFillFaulted(stream, had_buf)) {
        warmGlibcFileBuffer(stream, vm);
        ret = real_fgetc(stream);
    }
    return ret;
}

SMASH_VISIBLE int getc(FILE* stream) {
    // getc is semantically identical to fgetc
    return fgetc(stream);
}

SMASH_VISIBLE size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    using fn_t = size_t(*)(const void*, size_t, size_t, FILE*);
    SMASH_LAZY_RESOLVE(fn_t, fwrite);
    if (!real_fwrite) return 0;
    auto* vm = smash::g_smash_vm_region;
    size_t total = size * nmemb;
    if (total && bufferInHeap(ptr, total, vm))
        smash::vm::warmPages(const_cast<void*>(ptr), total, vm);
    warmGlibcFileBuffer(stream, vm);
    size_t ret = real_fwrite(ptr, size, nmemb, stream);
    return ret;
}

SMASH_VISIBLE int fflush(FILE* stream) {
    using fn_t = int(*)(FILE*);
    SMASH_LAZY_RESOLVE(fn_t, fflush);
    if (!real_fflush) return EOF;
    auto* vm = smash::g_smash_vm_region;
    warmGlibcFileBuffer(stream, vm);
    int ret = real_fflush(stream);
    return ret;
}

// NOTE: ioctl is NOT interposed. The variadic signature makes it impossible
// to know whether the third argument is a pointer or integer, and guessing
// wrong corrupts warmPages/pinPages calls. Most ioctls use stack buffers
// or non-heap memory anyway.

// epoll_wait/epoll_pwait - libevent allocates event buffers via malloc,
// so they ARE Smash-managed and need warming before kernel access.
// NOTE: Some apps (libevent) may mmap their own buffers, so we warm
// unconditionally if events is non-null to avoid EFAULT from kernel.
SMASH_VISIBLE int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    using fn_t = int(*)(int, struct epoll_event*, int, int);
    SMASH_LAZY_RESOLVE(fn_t, epoll_wait);
    if (!real_epoll_wait) {
#ifdef SYS_epoll_wait
        return syscall(SYS_epoll_wait, epfd, events, maxevents, timeout);
#else
        // arm64 / riscv have no SYS_epoll_wait; route via epoll_pwait
        // with NULL sigmask. _NSIG/8 = 8 (kernel sigset_t size).
        return syscall(SYS_epoll_pwait, epfd, events, maxevents, timeout, nullptr, 8);
#endif
    }
    auto* vm = smash::g_smash_vm_region;
    size_t size = static_cast<size_t>(maxevents) * sizeof(struct epoll_event);
    bool in_heap = bufferInHeap(events, size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_epoll_wait(epfd, events, maxevents, timeout); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(events, size, vm); });
    return ret;
}

SMASH_VISIBLE int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout, const sigset_t* sigmask) {
    using fn_t = int(*)(int, struct epoll_event*, int, int, const sigset_t*);
    SMASH_LAZY_RESOLVE(fn_t, epoll_pwait);
    if (!real_epoll_pwait) return syscall(SYS_epoll_pwait, epfd, events, maxevents, timeout, sigmask);
    auto* vm = smash::g_smash_vm_region;
    size_t size = static_cast<size_t>(maxevents) * sizeof(struct epoll_event);
    bool in_heap = bufferInHeap(events, size, vm);
    int ret = smash::vm::retryWithDecompress(
        [&] { return real_epoll_pwait(epfd, events, maxevents, timeout, sigmask); },
        [&] { if (in_heap) smash::vm::walkPagesForFault(events, size, vm); });
    return ret;
}

// Versioned symbol aliases for epoll_wait/epoll_pwait
// libevent requests epoll_wait@GLIBC_2.3.2, glibc uses 2.2.5 internally
// We export both versions pointing to the same implementation
SMASH_VISIBLE int epoll_wait_232(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    // Forward to the main implementation
    return epoll_wait(epfd, events, maxevents, timeout);
}

SMASH_VISIBLE int epoll_pwait_232(int epfd, struct epoll_event* events, int maxevents, int timeout, const sigset_t* sigmask) {
    return epoll_pwait(epfd, events, maxevents, timeout, sigmask);
}

// ── fstat versioned aliases (GLIBC_2.33) ────────────────────────────────────
// glibc 2.34 re-versioned fstat/fstat64 from GLIBC_2.2.5 to GLIBC_2.33 to
// carry y2038-safe struct stat. C++ binaries on Ubuntu 22.04+ reference
// fstat@GLIBC_2.33 specifically; without these aliases the dynamic linker
// won't match our LD_PRELOAD'd fstat (which lives at GLIBC_2.2.5) and
// goes to glibc directly.
SMASH_VISIBLE int fstat_233(int fd, struct stat* st) {
    return fstat(fd, st);
}
SMASH_VISIBLE int fstat64_233(int fd, struct stat64* st) {
    return fstat64(fd, st);
}
SMASH_VISIBLE int stat_233(const char* path, struct stat* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    return stat(path, st);
}
SMASH_VISIBLE int stat64_233(const char* path, struct stat64* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    return stat64(path, st);
}
SMASH_VISIBLE int lstat_233(const char* path, struct stat* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    return lstat(path, st);
}
SMASH_VISIBLE int lstat64_233(const char* path, struct stat64* st) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    return lstat64(path, st);
}
SMASH_VISIBLE int fstatat_233(int dirfd, const char* path, struct stat* st, int flags) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    return fstatat(dirfd, path, st, flags);
}
SMASH_VISIBLE int fstatat64_233(int dirfd, const char* path, struct stat64* st, int flags) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    return fstatat64(dirfd, path, st, flags);
}
// statx was introduced in glibc 2.28 — binaries reference statx@GLIBC_2.28.
SMASH_VISIBLE int statx_228(int dirfd, const char* path, int flags,
                             unsigned int mask, struct statx* buf) {
    smash::vm::warmString(path, smash::g_smash_vm_region);
    return statx(dirfd, path, flags, mask, buf);
}
// getdents64 userspace function added in glibc 2.30 — binaries built
// against glibc 2.30+ reference getdents64@GLIBC_2.30.
SMASH_VISIBLE ssize_t getdents64_230(int fd, void* dirp, size_t count) {
    return getdents64(fd, dirp, count);
}
// preadv / pwritev introduced in glibc 2.10.
SMASH_VISIBLE ssize_t preadv_210(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    return preadv(fd, iov, iovcnt, offset);
}
SMASH_VISIBLE ssize_t pwritev_210(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    return pwritev(fd, iov, iovcnt, offset);
}
// preadv2 / pwritev2 introduced in glibc 2.26.
SMASH_VISIBLE ssize_t preadv2_226(int fd, const struct iovec* iov, int iovcnt, off_t offset, int flags) {
    return preadv2(fd, iov, iovcnt, offset, flags);
}
SMASH_VISIBLE ssize_t pwritev2_226(int fd, const struct iovec* iov, int iovcnt, off_t offset, int flags) {
    return pwritev2(fd, iov, iovcnt, offset, flags);
}

// ── Path-argument syscalls ──────────────────────────────────────────────────
//
// A path that lives on a compressed page makes the kernel's
// strncpy_from_user fail with EFAULT; no signal is raised, so the fault
// handler never gets a chance. Warm the string first (syscall_compat.h
// warmString). These are thin wrappers over the raw syscalls (the *at forms,
// which exist on every arch), so they never need dlsym — they can be reached
// before the lazy resolver is usable. open/openat keep glibc's function (a
// pthread cancellation point), resolved via dlsym.
#define SMASH_WARM(path) smash::vm::warmString((path), smash::g_smash_vm_region)

SMASH_VISIBLE int open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    SMASH_WARM(path);
    using fn_t = int(*)(const char*, int, ...);
    SMASH_LAZY_RESOLVE(fn_t, open);
    if (!real_open) return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
    return real_open(path, flags, mode);
}
SMASH_VISIBLE int open64(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    SMASH_WARM(path);
    using fn_t = int(*)(const char*, int, ...);
    SMASH_LAZY_RESOLVE(fn_t, open64);
    if (!real_open64) return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags | O_LARGEFILE, mode));
    return real_open64(path, flags, mode);
}
SMASH_VISIBLE int openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    SMASH_WARM(path);
    using fn_t = int(*)(int, const char*, int, ...);
    SMASH_LAZY_RESOLVE(fn_t, openat);
    if (!real_openat) return static_cast<int>(syscall(SYS_openat, dirfd, path, flags, mode));
    return real_openat(dirfd, path, flags, mode);
}
SMASH_VISIBLE int openat64(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    SMASH_WARM(path);
    using fn_t = int(*)(int, const char*, int, ...);
    SMASH_LAZY_RESOLVE(fn_t, openat64);
    if (!real_openat64) return static_cast<int>(syscall(SYS_openat, dirfd, path, flags | O_LARGEFILE, mode));
    return real_openat64(dirfd, path, flags, mode);
}
SMASH_VISIBLE int creat(const char* path, mode_t mode) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode));
}
// arm64 has no renameat syscall, only renameat2.
#ifdef SYS_renameat
#define SMASH_SYS_RENAMEAT(f1, a, f2, b) syscall(SYS_renameat, f1, a, f2, b)
#else
#define SMASH_SYS_RENAMEAT(f1, a, f2, b) syscall(SYS_renameat2, f1, a, f2, b, 0)
#endif
SMASH_VISIBLE int rename(const char* from, const char* to) {
    SMASH_WARM(from); SMASH_WARM(to);
    return static_cast<int>(SMASH_SYS_RENAMEAT(AT_FDCWD, from, AT_FDCWD, to));
}
SMASH_VISIBLE int renameat(int fd1, const char* from, int fd2, const char* to) {
    SMASH_WARM(from); SMASH_WARM(to);
    return static_cast<int>(SMASH_SYS_RENAMEAT(fd1, from, fd2, to));
}
#ifdef SYS_renameat2
SMASH_VISIBLE int renameat2(int fd1, const char* from, int fd2, const char* to, unsigned int flags) {
    SMASH_WARM(from); SMASH_WARM(to);
    return static_cast<int>(syscall(SYS_renameat2, fd1, from, fd2, to, flags));
}
#endif
SMASH_VISIBLE int unlink(const char* path) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_unlinkat, AT_FDCWD, path, 0));
}
SMASH_VISIBLE int unlinkat(int dirfd, const char* path, int flags) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_unlinkat, dirfd, path, flags));
}
SMASH_VISIBLE int rmdir(const char* path) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_unlinkat, AT_FDCWD, path, AT_REMOVEDIR));
}
SMASH_VISIBLE int mkdir(const char* path, mode_t mode) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_mkdirat, AT_FDCWD, path, mode));
}
SMASH_VISIBLE int mkdirat(int dirfd, const char* path, mode_t mode) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_mkdirat, dirfd, path, mode));
}
SMASH_VISIBLE int access(const char* path, int mode) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_faccessat, AT_FDCWD, path, mode));
}
SMASH_VISIBLE int chdir(const char* path) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_chdir, path));
}
SMASH_VISIBLE int truncate(const char* path, off_t len) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_truncate, path, len));
}
SMASH_VISIBLE int link(const char* from, const char* to) {
    SMASH_WARM(from); SMASH_WARM(to);
    return static_cast<int>(syscall(SYS_linkat, AT_FDCWD, from, AT_FDCWD, to, 0));
}
SMASH_VISIBLE int symlink(const char* target, const char* path) {
    SMASH_WARM(target); SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_symlinkat, target, AT_FDCWD, path));
}
SMASH_VISIBLE int chmod(const char* path, mode_t mode) {
    SMASH_WARM(path);
    return static_cast<int>(syscall(SYS_fchmodat, AT_FDCWD, path, mode));
}
// fopen opens through glibc's internal __open, which no wrapper sees: warm
// the path here (Valkey's DEBUG RELOAD fopen()s its heap-held RDB path).
SMASH_VISIBLE FILE* fopen(const char* path, const char* mode) {
    SMASH_WARM(path);
    using fn_t = FILE*(*)(const char*, const char*);
    SMASH_LAZY_RESOLVE(fn_t, fopen);
    if (!real_fopen) { errno = ENOSYS; return nullptr; }
    return real_fopen(path, mode);
}
SMASH_VISIBLE FILE* fopen64(const char* path, const char* mode) {
    SMASH_WARM(path);
    using fn_t = FILE*(*)(const char*, const char*);
    SMASH_LAZY_RESOLVE(fn_t, fopen64);
    if (!real_fopen64) { errno = ENOSYS; return nullptr; }
    return real_fopen64(path, mode);
}
SMASH_VISIBLE FILE* freopen(const char* path, const char* mode, FILE* f) {
    SMASH_WARM(path);
    using fn_t = FILE*(*)(const char*, const char*, FILE*);
    SMASH_LAZY_RESOLVE(fn_t, freopen);
    if (!real_freopen) { errno = ENOSYS; return nullptr; }
    return real_freopen(path, mode, f);
}
#undef SMASH_WARM

// ── External-mapping interposers (mmap / munmap / madvise / mprotect / mremap)
//
// Application-direct mmap calls (bundled jemalloc / mimalloc arenas, CPython
// obmalloc arenas, FFmpeg frame pools) bypass smash's malloc and so escape
// compression. With SMASH_TRACK_EXTERNAL=1, private anonymous RW mappings are
// registered with VmRegion's external-page hash so the compressor's tick sees
// them (policy: ext::eligibleMapping in external_track.h), and every later
// call that changes such memory first settles smash's state for it
// (ext::detachRange → CompressorThread::detachExternalRange).
//
// smash's own mmap/madvise/mprotect go through raw syscalls (platform_mem.h),
// so none of these wrappers ever sees smash-internal memory.

SMASH_VISIBLE void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    using fn_t = void*(*)(void*, size_t, int, int, int, off_t);
    SMASH_LAZY_RESOLVE(fn_t, mmap);
    if (!real_mmap) return MAP_FAILED;
    auto* vm = smash::g_smash_vm_region;
    // MAP_FIXED replaces whatever was mapped there: settle tracked pages
    // first. Restore (not discard): if the mmap fails, the old mapping may
    // survive and must still hold its data.
    if (vm && (flags & MAP_FIXED) && addr && len)
        smash::ext::detachRange(vm, addr, len, smash::ext::kRestore);
    void* ret = real_mmap(addr, len, prot, flags, fd, offset);
    if (ret == MAP_FAILED || len == 0) return ret;
    if (vm && smash::ext::eligibleMapping(prot, flags))
        smash::ext::registerRange(vm, ret, len);
    return ret;
}

// mmap64 is the LFS (Large File Support) version used internally by glibc
SMASH_VISIBLE void* mmap64(void* addr, size_t len, int prot, int flags, int fd, off64_t offset) {
    using fn_t = void*(*)(void*, size_t, int, int, int, off64_t);
    SMASH_LAZY_RESOLVE(fn_t, mmap64);
    if (!real_mmap64) return MAP_FAILED;
    auto* vm = smash::g_smash_vm_region;
    if (vm && (flags & MAP_FIXED) && addr && len)
        smash::ext::detachRange(vm, addr, len, smash::ext::kRestore);
    void* ret = real_mmap64(addr, len, prot, flags, fd, offset);
    if (ret == MAP_FAILED || len == 0) return ret;
    if (vm && smash::ext::eligibleMapping(prot, flags))
        smash::ext::registerRange(vm, ret, len);
    return ret;
}

SMASH_VISIBLE int munmap(void* addr, size_t len) {
    using fn_t = int(*)(void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, munmap);
    if (!real_munmap) return -1;
    auto* vm = smash::g_smash_vm_region;
    // Untrack + free blobs under the per-page lock BEFORE the real munmap, so
    // a compressor worker mid-snapshot can't read an unmapped page
    // (model/SmashExternalRace.{lean,tla}); no syscall runs under the lock.
    if (vm && addr && len) smash::ext::detachRange(vm, addr, len, smash::ext::kUnmap);
    return real_munmap(addr, len);
}

// sbrk / __sbrk: bundled allocators that grow the program break themselves
// (gperftools tcmalloc's SbrkSysAllocator calls __sbrk) get private anonymous
// RW heap. glibc malloc's own break growth is an internal call and never
// reaches these wrappers.
namespace {
using sbrk_fn = void* (*)(intptr_t);
void* trackedSbrk(sbrk_fn real, intptr_t inc) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && inc < 0) {
        // Shrink: [brk+inc, brk) goes away. Drop smash's state first (same
        // ordering as munmap, so no worker is mid-snapshot on it).
        char* cur = static_cast<char*>(real(0));
        if (cur != reinterpret_cast<char*>(-1))
            smash::ext::detachRange(vm, cur + inc, static_cast<size_t>(-inc), smash::ext::kUnmap);
    }
    void* old = real(inc);
    if (vm && inc > 0 && old != reinterpret_cast<void*>(-1))
        smash::ext::registerRange(vm, old, static_cast<size_t>(inc));
    return old;
}
}  // namespace

SMASH_VISIBLE void* sbrk(intptr_t inc) {
    using fn_t = void*(*)(intptr_t);
    SMASH_LAZY_RESOLVE(fn_t, sbrk);
    if (!real_sbrk) { errno = ENOMEM; return reinterpret_cast<void*>(-1); }
    return trackedSbrk(real_sbrk, inc);
}

SMASH_VISIBLE void* __sbrk(intptr_t inc) {
    using fn_t = void*(*)(intptr_t);
    SMASH_LAZY_RESOLVE(fn_t, __sbrk);
    if (!real___sbrk) { errno = ENOMEM; return reinterpret_cast<void*>(-1); }
    return trackedSbrk(real___sbrk, inc);
}

// pthread_sigmask / sigprocmask: never let a thread block SIGSEGV / SIGBUS.
// smash decompresses on the fault; a synchronous fault on a thread that has
// them blocked is not delivered — the kernel resets the action to SIG_DFL and
// kills the process. jemalloc's background purge thread is created with
// every signal blocked (background_thread_create_signals_masked) and then
// walks extent metadata smash may have compressed: under
// SMASH_TRACK_EXTERNAL=1 bundled-jemalloc Valkey died this way within
// seconds. Blocking a synchronous fault signal has no useful effect anyway
// (it can't be postponed), so the only observable change is that an
// asynchronous kill(SIGSEGV/SIGBUS) is no longer held pending.
namespace {
inline const sigset_t* keepFaultSignals(int how, const sigset_t* set, sigset_t* copy) {
    if (!set || how == SIG_UNBLOCK) return set;
    if (sigismember(set, SIGSEGV) != 1 && sigismember(set, SIGBUS) != 1) return set;
    *copy = *set;
    sigdelset(copy, SIGSEGV);
    sigdelset(copy, SIGBUS);
    return copy;
}

// Fallback before dlsym resolves: raw rt_sigprocmask, also stripping glibc's
// internal SIGCANCEL / SIGSETXID (32, 33) as glibc itself does — blocking
// SIGSETXID would hang setuid() in multithreaded processes.
inline int rawSigmask(int how, const sigset_t* set, sigset_t* old) {
    sigset_t copy;
    if (set && how != SIG_UNBLOCK) {
        copy = *set;
        sigdelset(&copy, 32);
        sigdelset(&copy, 33);
        set = &copy;
    }
    return static_cast<int>(syscall(SYS_rt_sigprocmask, how, set, old, _NSIG / 8));
}
}  // namespace

SMASH_VISIBLE int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) {
    using fn_t = int(*)(int, const sigset_t*, sigset_t*);
    SMASH_LAZY_RESOLVE(fn_t, pthread_sigmask);
    sigset_t copy;
    set = keepFaultSignals(how, set, &copy);
    if (!real_pthread_sigmask) return rawSigmask(how, set, old) == 0 ? 0 : errno;
    return real_pthread_sigmask(how, set, old);
}
SMASH_VISIBLE int pthread_sigmask_232(int how, const sigset_t* set, sigset_t* old) {
    return pthread_sigmask(how, set, old);
}

SMASH_VISIBLE int sigprocmask(int how, const sigset_t* set, sigset_t* old) {
    using fn_t = int(*)(int, const sigset_t*, sigset_t*);
    SMASH_LAZY_RESOLVE(fn_t, sigprocmask);
    sigset_t copy;
    set = keepFaultSignals(how, set, &copy);
    if (!real_sigprocmask) return rawSigmask(how, set, old);
    return real_sigprocmask(how, set, old);
}

// sigaltstack: signal handlers (smash's included, SA_ONSTACK) run on the
// alternate stack, so it must never be compressed: untrack it (restoring
// any compressed page) before the kernel starts using it.
SMASH_VISIBLE int sigaltstack(const stack_t* ss, stack_t* old) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && ss && !(ss->ss_flags & SS_DISABLE) && ss->ss_sp && ss->ss_size)
        smash::ext::detachRange(vm, ss->ss_sp, ss->ss_size, smash::ext::kRestore);
    return smash::vm::rawSigaltstack(ss, old);
}

// mlock / mlock2 / mlockall: locked memory must stay resident (and may hold
// secrets kept out of swap), so it must not be compressed. Untrack the range,
// restoring any compressed page, before locking it. mlockall untracks every
// external page; with MCL_FUTURE no later mapping is tracked either.
SMASH_VISIBLE int mlock(const void* addr, size_t len) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && addr && len)
        smash::ext::detachRange(vm, const_cast<void*>(addr), len, smash::ext::kRestore);
    return static_cast<int>(syscall(SYS_mlock, addr, len));
}
SMASH_VISIBLE int mlock2(const void* addr, size_t len, unsigned int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && addr && len)
        smash::ext::detachRange(vm, const_cast<void*>(addr), len, smash::ext::kRestore);
#ifdef SYS_mlock2
    return static_cast<int>(syscall(SYS_mlock2, addr, len, flags));
#else
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}
SMASH_VISIBLE int mlockall(int flags) {
    if (flags & MCL_FUTURE) smash::ext::g_no_new_tracking.store(true, std::memory_order_relaxed);
    smash::ext::detachAll(smash::g_smash_vm_region);
    return static_cast<int>(syscall(SYS_mlockall, flags));
}

// madvise: allocators purge freed extents with DONTNEED / FREE. Drop smash's
// blob for those pages first, or a later access would decompress stale data
// over memory the allocator believes is zero (jemalloc's calloc fast path).
//
// Pages smash reclaims through its userfaultfd stay registered for MISSING
// faults, where an empty PTE turns a kernel-mode access into EFAULT: after a
// discard the purged pages are mapped to the zero page (refill), and
// MADV_FREE on them becomes MADV_DONTNEED (a lazily freed page the kernel
// reclaims later would become such an empty PTE at an unknowable time;
// DONTNEED is a valid implementation of FREE's contract).
SMASH_VISIBLE int madvise(void* addr, size_t len, int advice) {
    // Direct syscall (glibc's madvise is a bare syscall wrapper): no dlsym
    // resolution window in which the call could fail.
    auto* vm = smash::g_smash_vm_region;
    if (!vm || !addr || !len) return static_cast<int>(syscall(SYS_madvise, addr, len, advice));
    if (smash::ext::discardAdvice(advice)) {
        const size_t tracked = smash::ext::detachRange(vm, addr, len, smash::ext::kPurge);
        int adv = advice;
#ifdef MADV_FREE
        if (tracked && advice == MADV_FREE && smash::vm::g_uffd_fd >= 0) adv = MADV_DONTNEED;
#endif
        const int rc = static_cast<int>(syscall(SYS_madvise, addr, len, adv));
        if (tracked) smash::ext::refillRange(vm, addr, len);
        return rc;
    }
    if (smash::ext::untrackAdvice(advice))
        smash::ext::detachRange(vm, addr, len, smash::ext::kRestore);
    if (smash::ext::populateAdvice(advice) && vm->externalLivePages()) {
        // Prefaulting a compressed page fails (EFAULT for a uffd-MISSING one,
        // ENOMEM for PROT_NONE): the app asked for the range to be resident,
        // so decompress its tracked pages first (a read faults them in).
        const uintptr_t end = smash::ext::pageUp(addr, len);
        for (uintptr_t p = smash::ext::pageDown(addr); p < end; p += smash::kPageSize)
            if (vm->contains(p)) (void)*reinterpret_cast<volatile const char*>(p);
    }
    return static_cast<int>(syscall(SYS_madvise, addr, len, advice));
}

// mprotect: the app takes over the protection of these pages (guard pages,
// read-only freezing, W^X). Put compressed contents back and untrack them,
// then re-register if the range ends up plain RW again.
SMASH_VISIBLE int mprotect(void* addr, size_t len, int prot) {
    auto* vm = smash::g_smash_vm_region;
    size_t tracked = 0;
    if (vm && addr && len)
        tracked = smash::ext::detachRange(vm, addr, len, smash::ext::kRestore);
    int rc = static_cast<int>(syscall(SYS_mprotect, addr, len, prot));
    // Only re-track a range that was wholly tracked before (so it is known
    // private anonymous memory).
    if (rc == 0 && tracked && prot == (PROT_READ | PROT_WRITE) &&
        tracked == (smash::ext::pageUp(addr, len) - smash::ext::pageDown(addr)) / smash::kPageSize)
        smash::ext::registerRange(vm, addr, len);
    return rc;
}

// mremap can grow / shrink / move a mapping, carrying its (possibly PROT_NONE
// compressed) pages along. Restore + untrack the old range first, re-register
// the result.
SMASH_VISIBLE void* mremap(void* old_addr, size_t old_size, size_t new_size, int flags, ...) {
    using fn_t = void*(*)(void*, size_t, size_t, int, ...);
    SMASH_LAZY_RESOLVE(fn_t, mremap);
    if (!real_mremap) return MAP_FAILED;

    void* new_addr = nullptr;
    if (flags & MREMAP_FIXED) {
        va_list ap;
        va_start(ap, flags);
        new_addr = va_arg(ap, void*);
        va_end(ap);
    }

    auto* vm = smash::g_smash_vm_region;
    size_t tracked = 0;
    if (vm && old_addr && old_size)
        tracked = smash::ext::detachRange(vm, old_addr, old_size, smash::ext::kRestore);
    if (vm && (flags & MREMAP_FIXED) && new_addr && new_size)
        smash::ext::detachRange(vm, new_addr, new_size, smash::ext::kRestore);

    void* ret = (flags & MREMAP_FIXED)
        ? real_mremap(old_addr, old_size, new_size, flags, new_addr)
        : real_mremap(old_addr, old_size, new_size, flags);
    if (vm && tracked) {
        if (ret != MAP_FAILED) smash::ext::registerRange(vm, ret, new_size);
        else smash::ext::registerRange(vm, old_addr, old_size);  // unchanged
    }
    return ret;
}

// ioctl: an application that uses userfaultfd itself (live migration,
// GC barriers, checkpointing) registers its ranges with UFFDIO_REGISTER.
// A range smash's own uffd already covers would fail that with EBUSY, and a
// range smash compresses the classic way would have its pages dropped under
// the app's MISSING handler. Untrack (restore, unregister) the range first;
// the app owns it from then on. (A REGISTER that comes first makes smash's
// own registration fail with EBUSY: never compressed, see
// registerExternalRun.) Raw syscall: no dlsym, and smash's own ioctls use
// SYS_ioctl directly, so they never come through here.
SMASH_VISIBLE int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (req == UFFDIO_REGISTER && arg && fd != smash::vm::g_uffd_fd) {
        auto* vm = smash::g_smash_vm_region;
        const auto* r = static_cast<const uffdio_register*>(arg);
        if (vm && r->range.len)
            smash::ext::detachRange(vm, reinterpret_cast<void*>(r->range.start),
                                    static_cast<size_t>(r->range.len), smash::ext::kRestore);
    }
    return static_cast<int>(syscall(SYS_ioctl, fd, req, arg));
}

// liburing fixed buffers: IORING_REGISTER_BUFFERS pins the pages
// (FOLL_LONGTERM) and the kernel then reads and writes the pinned frames
// directly, with no access through the app's page tables. A pinned page
// smash later reclaims the classic way (PROT_NONE + MADV_DONTNEED) drops
// the app's mapping while the kernel keeps writing the old frame: I/O
// completions vanish. (The uffd path refuses pinned pages: UFFDIO_MOVE
// fails with EBUSY on a maybe-DMA-pinned folio, and the shadow copy is
// discarded.) Registering a range that holds a compressed page fails with
// EFAULT. So untrack (restore, unregister) every registered buffer first.
// liburing makes the io_uring_register syscall itself (no libc wrapper), so
// only these exported entry points can be seen, and only when liburing is a
// shared library. Versioned like liburing's own symbols, so references
// bound to LIBURING_2.x resolve here.
namespace {
void untrackIovecs(const struct iovec* iov, unsigned nr) {
    auto* vm = smash::g_smash_vm_region;
    if (!vm || !iov) return;
    for (unsigned k = 0; k < nr; ++k)
        if (iov[k].iov_base && iov[k].iov_len)
            smash::ext::detachRange(vm, iov[k].iov_base, iov[k].iov_len, smash::ext::kRestore);
}
void* liburingNext(const char* name, const char* ver) { return dlvsym(RTLD_NEXT, name, ver); }
}  // namespace

struct io_uring;
SMASH_VISIBLE int io_uring_register_buffers(struct io_uring* ring, const struct iovec* iov, unsigned nr) {
    using fn_t = int (*)(struct io_uring*, const struct iovec*, unsigned);
    static fn_t real = reinterpret_cast<fn_t>(liburingNext("io_uring_register_buffers", "LIBURING_2.0"));
    if (!real) return -ENOSYS;
    untrackIovecs(iov, nr);
    return real(ring, iov, nr);
}
SMASH_VISIBLE int io_uring_register_buffers_tags(struct io_uring* ring, const struct iovec* iov,
                                                 const __u64* tags, unsigned nr) {
    using fn_t = int (*)(struct io_uring*, const struct iovec*, const __u64*, unsigned);
    static fn_t real = reinterpret_cast<fn_t>(liburingNext("io_uring_register_buffers_tags", "LIBURING_2.1"));
    if (!real) return -ENOSYS;
    untrackIovecs(iov, nr);
    return real(ring, iov, tags, nr);
}
SMASH_VISIBLE int io_uring_register_buffers_update_tag(struct io_uring* ring, unsigned off,
                                                       const struct iovec* iov, const __u64* tags,
                                                       unsigned nr) {
    using fn_t = int (*)(struct io_uring*, unsigned, const struct iovec*, const __u64*, unsigned);
    static fn_t real = reinterpret_cast<fn_t>(liburingNext("io_uring_register_buffers_update_tag", "LIBURING_2.1"));
    if (!real) return -ENOSYS;
    untrackIovecs(iov, nr);
    return real(ring, off, iov, tags, nr);
}

} // extern "C"

// Create version aliases: epoll_wait_232 -> epoll_wait@GLIBC_2.3.2
__asm__(".symver epoll_wait_232,epoll_wait@GLIBC_2.3.2");
__asm__(".symver epoll_pwait_232,epoll_pwait@GLIBC_2.3.2");
__asm__(".symver fstat_233,fstat@GLIBC_2.33");
__asm__(".symver pthread_sigmask_232,pthread_sigmask@GLIBC_2.32");
__asm__(".symver fstat64_233,fstat64@GLIBC_2.33");
__asm__(".symver stat_233,stat@GLIBC_2.33");
__asm__(".symver stat64_233,stat64@GLIBC_2.33");
__asm__(".symver lstat_233,lstat@GLIBC_2.33");
__asm__(".symver lstat64_233,lstat64@GLIBC_2.33");
__asm__(".symver fstatat_233,fstatat@GLIBC_2.33");
__asm__(".symver fstatat64_233,fstatat64@GLIBC_2.33");
__asm__(".symver statx_228,statx@GLIBC_2.28");
__asm__(".symver getdents64_230,getdents64@GLIBC_2.30");
__asm__(".symver preadv_210,preadv@GLIBC_2.10");
__asm__(".symver pwritev_210,pwritev@GLIBC_2.10");
__asm__(".symver preadv2_226,preadv2@GLIBC_2.26");
__asm__(".symver pwritev2_226,pwritev2@GLIBC_2.26");

// ══════════════════════════════════════════════════════════════════════════════
// TBB scalable_allocator interposition
// ══════════════════════════════════════════════════════════════════════════════
//
// Intel TBB (Thread Building Blocks) uses its own memory allocator that bypasses
// malloc and allocates 64MB arenas directly via mmap. By interposing TBB's
// scalable_malloc family, we redirect those allocations through smash's malloc,
// making them eligible for compression.
//
// This is critical for walrus (neuronx-cc backend) which uses TBB and can have
// 15-20GB of TBB-managed memory that would otherwise escape compression.
//
// Note: These symbols are weak to allow linking even when TBB is not present.
// When TBB IS present and linked dynamically, LD_PRELOAD ensures our wrappers
// take precedence.

// TBB interposition call counters (for debugging)
static std::atomic<uint64_t> g_scalable_malloc_count{0};
static std::atomic<uint64_t> g_scalable_malloc_bytes{0};

extern "C" {

// Core allocation functions
SMASH_VISIBLE void* scalable_malloc(size_t size) {
    g_scalable_malloc_count.fetch_add(1, std::memory_order_relaxed);
    g_scalable_malloc_bytes.fetch_add(size, std::memory_order_relaxed);
    return malloc(size);
}

SMASH_VISIBLE void scalable_free(void* ptr) {
    free(ptr);
}

SMASH_VISIBLE void* scalable_realloc(void* ptr, size_t size) {
    return realloc(ptr, size);
}

SMASH_VISIBLE void* scalable_calloc(size_t nobj, size_t size) {
    return calloc(nobj, size);
}

// Aligned allocation functions
SMASH_VISIBLE int scalable_posix_memalign(void** memptr, size_t alignment, size_t size) {
    return posix_memalign(memptr, alignment, size);
}

SMASH_VISIBLE void* scalable_aligned_malloc(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
}

SMASH_VISIBLE void scalable_aligned_free(void* ptr) {
    free(ptr);
}

SMASH_VISIBLE void* scalable_aligned_realloc(void* ptr, size_t size, size_t alignment) {
    // TBB's aligned_realloc: allocate new, copy, free old
    if (!ptr) {
        return scalable_aligned_malloc(size, alignment);
    }
    if (size == 0) {
        free(ptr);
        return nullptr;
    }
    void* new_ptr = nullptr;
    if (posix_memalign(&new_ptr, alignment, size) != 0) {
        return nullptr;
    }
    // We don't know the old size, so we copy `size` bytes (safe if growing)
    // For shrinking, this may read past end but won't write past new allocation
    size_t old_size = malloc_usable_size(ptr);
    size_t copy_size = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);
    free(ptr);
    return new_ptr;
}

// Size query
SMASH_VISIBLE size_t scalable_msize(void* ptr) {
    if (!ptr) return 0;
    return malloc_usable_size(ptr);
}

// TBB memory pool allocation (route to regular malloc)
// These are used by TBB's memory_pool<scalable_allocator>
SMASH_VISIBLE void* pool_malloc(void* /*pool*/, size_t size) {
    return malloc(size);
}

SMASH_VISIBLE void pool_free(void* /*pool*/, void* ptr) {
    free(ptr);
}

SMASH_VISIBLE void* pool_realloc(void* /*pool*/, void* ptr, size_t size) {
    return realloc(ptr, size);
}

SMASH_VISIBLE void* pool_aligned_malloc(void* /*pool*/, size_t size, size_t alignment) {
    return scalable_aligned_malloc(size, alignment);
}

SMASH_VISIBLE void pool_aligned_free(void* /*pool*/, void* ptr) {
    free(ptr);
}

}  // extern "C"

// Report TBB interposition stats at process exit
namespace {
void reportTbbStatsAtExit() {
    uint64_t sc_count = g_scalable_malloc_count.load(std::memory_order_relaxed);
    uint64_t sc_bytes = g_scalable_malloc_bytes.load(std::memory_order_relaxed);
    if (sc_count > 0 && std::getenv("SMASH_STATS")) {
        // Use safe_snprintf + write(2): glibc's fprintf can call malloc
        // for stream lock acquisition, and atexit can fire after the
        // allocator is in a quiescing state where that recurses.
        char buf[160];
        int n = smash::safe_snprintf(buf, sizeof(buf),
            "[smash tbb] scalable_malloc: %lu calls, %.1f MB\n",
            (unsigned long)sc_count, sc_bytes / (1024.0 * 1024.0));
        if (n > 0) (void)!::write(2, buf, (size_t)n);
    }
}

struct TbbStatsRegistrar {
    TbbStatsRegistrar() { std::atexit(reportTbbStatsAtExit); }
} g_tbb_stats_registrar;
}  // namespace

// TBB cache_aligned_{allocate,deallocate} interposition: REMOVED.
// libtbb dispatches its allocate/deallocate through internal function-pointer
// handlers (`cache_aligned_allocate_handler`, `cache_aligned_deallocate_handler`).
// LD_PRELOAD interposes the public symbols only, which catches cross-DSO calls
// (libwalrus -> libtbb) but NOT libtbb-internal calls (which jump directly
// through the handler pointers in libtbb's data segment). The resulting
// allocate/deallocate-via-different-allocator mismatch corrupts the heap with
// "double free or corruption (out)" / "munmap_chunk(): invalid pointer" inside
// neuron-cc's Tensorizer subprocesses. Letting TBB manage its own cache-aligned
// allocations is the only safe option — we lose visibility into a few hundred
// MB of TBB-internal memory but gain correctness.

extern "C" {

// ── execve / execvp ─────────────────────────────────────────────────────────
//
// execve() doesn't return on success, so we can't use the EFAULT-retry
// pattern. Instead we proactively warm every smash-managed page that
// could appear in argv/envp strings, then call the real syscall.
//
// Production trigger: subprocess.run(['/path/to/binary', ...]) from
// Python under full smash, where the path string ended up in a
// smash-managed page that got compressed before subprocess fork+exec.
// Without this wrapper, execve returns EFAULT and Python surfaces it
// as `[Errno 14] Bad address: '<path>'`.

static inline void warm_str(const char* s) {
    auto* vm = smash::g_smash_vm_region;
    if (!vm || !s) return;
    if (!vm->contains(reinterpret_cast<uintptr_t>(s))) return;
    // Walk the string until NUL, touching each page.
    smash::vm::warmPages(s, strlen(s) + 1, vm);
}

static inline void warm_argv(char* const* arr) {
    if (!arr) return;
    auto* vm = smash::g_smash_vm_region;
    if (!vm) return;
    // Touch the array itself (pointers).
    size_t n = 0;
    while (arr[n]) ++n;
    if (n > 0) {
        smash::vm::warmPages(arr, (n + 1) * sizeof(char*), vm);
    }
    for (size_t i = 0; i < n; ++i) warm_str(arr[i]);
}

SMASH_VISIBLE int execve(const char* pathname, char* const argv[], char* const envp[]) {
    using fn_t = int(*)(const char*, char* const[], char* const[]);
    SMASH_LAZY_RESOLVE(fn_t, execve);
    warm_str(pathname);
    warm_argv(argv);
    warm_argv(envp);
    if (std::getenv("SMASH_TRACE_EXEC")) {
        const char* msg = "[smash exec] execve called\n";
        (void)!write(2, msg, strlen(msg));
    }
    if (!real_execve) return syscall(SYS_execve, pathname, argv, envp);
    return real_execve(pathname, argv, envp);
}

SMASH_VISIBLE int execv(const char* pathname, char* const argv[]) {
    using fn_t = int(*)(const char*, char* const[]);
    SMASH_LAZY_RESOLVE(fn_t, execv);
    if (std::getenv("SMASH_TRACE_EXEC")) {
        const char* msg = "[smash exec] execv called\n";
        (void)!write(2, msg, strlen(msg));
    }
    warm_str(pathname);
    warm_argv(argv);
    if (!real_execv) {
        extern char **environ;
        return syscall(SYS_execve, pathname, argv, environ);
    }
    return real_execv(pathname, argv);
}

SMASH_VISIBLE int execvp(const char* file, char* const argv[]) {
    using fn_t = int(*)(const char*, char* const[]);
    SMASH_LAZY_RESOLVE(fn_t, execvp);
    warm_str(file);
    warm_argv(argv);
    if (!real_execvp) return -1;  // PATH search needs libc
    return real_execvp(file, argv);
}

SMASH_VISIBLE int execvpe(const char* file, char* const argv[], char* const envp[]) {
    using fn_t = int(*)(const char*, char* const[], char* const[]);
    SMASH_LAZY_RESOLVE(fn_t, execvpe);
    warm_str(file);
    warm_argv(argv);
    warm_argv(envp);
    if (!real_execvpe) return -1;
    return real_execvpe(file, argv, envp);
}

// posix_spawn / posix_spawnp — Python's subprocess uses these by default
// since 3.8 when close_fds is True. Same warming pattern as execve.
struct posix_spawn_file_actions_t_opaque;
struct posix_spawnattr_t_opaque;

SMASH_VISIBLE int posix_spawn(pid_t* pid, const char* path,
                               const void* file_actions,
                               const void* attrp,
                               char* const argv[], char* const envp[]) {
    using fn_t = int(*)(pid_t*, const char*, const void*, const void*,
                        char* const[], char* const[]);
    SMASH_LAZY_RESOLVE(fn_t, posix_spawn);
    warm_str(path);
    warm_argv(argv);
    warm_argv(envp);
    if (!real_posix_spawn) return ENOSYS;
    return real_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}

SMASH_VISIBLE int posix_spawnp(pid_t* pid, const char* file,
                                const void* file_actions,
                                const void* attrp,
                                char* const argv[], char* const envp[]) {
    using fn_t = int(*)(pid_t*, const char*, const void*, const void*,
                        char* const[], char* const[]);
    SMASH_LAZY_RESOLVE(fn_t, posix_spawnp);
    warm_str(file);
    warm_argv(argv);
    warm_argv(envp);
    if (!real_posix_spawnp) return ENOSYS;
    return real_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
}

} // extern "C"

// ─── C++ OPERATOR NEW/DELETE VERSIONED ALIASES ─────────────────────────────────
// libBIR.so and other walrus libraries request operator new/delete with
// GLIBCXX_3.4 and CXXABI_1.3.x symbol versions, not GLIBC_2.2.5.
// Without these versioned aliases, LD_PRELOAD fails to capture C++ allocations,
// and walrus's statically-linked tcmalloc wins the symbol resolution.
// Result: allocations through smash, deallocations through tcmalloc → crash.
//
// We create aliases for the GLIBCXX/CXXABI-versioned symbols that call
// alloc8's xxmalloc/xxfree directly (NOT through the operator symbols, which
// would create infinite recursion via PLT).

// External declarations for alloc8's core allocation functions
extern "C" {
extern void* xxmalloc(size_t);
extern void  xxfree(void*);
extern void  xxfree_sized(void*, size_t);
extern void  xxfree_aligned_sized(void*, size_t, size_t);
extern void* xxmemalign(size_t, size_t);
}

// Aliased wrapper functions for GLIBCXX_3.4 versions
// These directly call xxmalloc/xxfree, matching alloc8's operator new/delete
extern "C" {
SMASH_VISIBLE void* _Znwm_GLIBCXX_3_4(size_t sz) {
    void* ptr = xxmalloc(sz);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
SMASH_VISIBLE void* _Znam_GLIBCXX_3_4(size_t sz) {
    void* ptr = xxmalloc(sz);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
SMASH_VISIBLE void  _ZdlPv_GLIBCXX_3_4(void* p)  { if (p) xxfree(p); }
SMASH_VISIBLE void  _ZdaPv_GLIBCXX_3_4(void* p)  { if (p) xxfree(p); }
SMASH_VISIBLE void* _ZnwmRKSt9nothrow_t_GLIBCXX_3_4(size_t sz, const void*)
    { return xxmalloc(sz); }
SMASH_VISIBLE void* _ZnamRKSt9nothrow_t_GLIBCXX_3_4(size_t sz, const void*)
    { return xxmalloc(sz); }
SMASH_VISIBLE void  _ZdlPvRKSt9nothrow_t_GLIBCXX_3_4(void* p, const void*)
    { if (p) xxfree(p); }
SMASH_VISIBLE void  _ZdaPvRKSt9nothrow_t_GLIBCXX_3_4(void* p, const void*)
    { if (p) xxfree(p); }
}

// CXXABI_1.3.9 versions (sized delete)
extern "C" {
SMASH_VISIBLE void _ZdlPvm_CXXABI_1_3_9(void* p, size_t sz)  { if (p) xxfree_sized(p, sz); }
SMASH_VISIBLE void _ZdaPvm_CXXABI_1_3_9(void* p, size_t sz)  { if (p) xxfree_sized(p, sz); }
}

// CXXABI_1.3.11 versions (aligned)
extern "C" {
SMASH_VISIBLE void* _ZnwmSt11align_val_t_CXXABI_1_3_11(size_t sz, size_t al) {
    void* ptr = xxmemalign(al, sz);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
SMASH_VISIBLE void* _ZnamSt11align_val_t_CXXABI_1_3_11(size_t sz, size_t al) {
    void* ptr = xxmemalign(al, sz);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
SMASH_VISIBLE void  _ZdlPvSt11align_val_t_CXXABI_1_3_11(void* p, size_t)
    { if (p) xxfree(p); }
SMASH_VISIBLE void  _ZdaPvSt11align_val_t_CXXABI_1_3_11(void* p, size_t)
    { if (p) xxfree(p); }
SMASH_VISIBLE void  _ZdlPvmSt11align_val_t_CXXABI_1_3_11(void* p, size_t sz, size_t al)
    { if (p) xxfree_aligned_sized(p, al, sz); }
SMASH_VISIBLE void  _ZdaPvmSt11align_val_t_CXXABI_1_3_11(void* p, size_t sz, size_t al)
    { if (p) xxfree_aligned_sized(p, al, sz); }
SMASH_VISIBLE void* _ZnwmSt11align_val_tRKSt9nothrow_t_CXXABI_1_3_11(
        size_t sz, size_t al, const void*)
    { return xxmemalign(al, sz); }
SMASH_VISIBLE void* _ZnamSt11align_val_tRKSt9nothrow_t_CXXABI_1_3_11(
        size_t sz, size_t al, const void*)
    { return xxmemalign(al, sz); }
}

// .symver directives create versioned symbol aliases.
// Use single @ for non-default versions (so we don't conflict with alloc8's
// GLIBC_2.2.5 versions). The dynamic linker will still match these when
// a caller requests the specific version (e.g., _Znwm@GLIBCXX_3.4).
__asm__(".symver _Znwm_GLIBCXX_3_4,_Znwm@GLIBCXX_3.4");
__asm__(".symver _Znam_GLIBCXX_3_4,_Znam@GLIBCXX_3.4");
__asm__(".symver _ZdlPv_GLIBCXX_3_4,_ZdlPv@GLIBCXX_3.4");
__asm__(".symver _ZdaPv_GLIBCXX_3_4,_ZdaPv@GLIBCXX_3.4");
__asm__(".symver _ZnwmRKSt9nothrow_t_GLIBCXX_3_4,_ZnwmRKSt9nothrow_t@GLIBCXX_3.4");
__asm__(".symver _ZnamRKSt9nothrow_t_GLIBCXX_3_4,_ZnamRKSt9nothrow_t@GLIBCXX_3.4");
__asm__(".symver _ZdlPvRKSt9nothrow_t_GLIBCXX_3_4,_ZdlPvRKSt9nothrow_t@GLIBCXX_3.4");
__asm__(".symver _ZdaPvRKSt9nothrow_t_GLIBCXX_3_4,_ZdaPvRKSt9nothrow_t@GLIBCXX_3.4");

__asm__(".symver _ZdlPvm_CXXABI_1_3_9,_ZdlPvm@CXXABI_1.3.9");
__asm__(".symver _ZdaPvm_CXXABI_1_3_9,_ZdaPvm@CXXABI_1.3.9");

__asm__(".symver _ZnwmSt11align_val_t_CXXABI_1_3_11,_ZnwmSt11align_val_t@CXXABI_1.3.11");
__asm__(".symver _ZnamSt11align_val_t_CXXABI_1_3_11,_ZnamSt11align_val_t@CXXABI_1.3.11");
__asm__(".symver _ZdlPvSt11align_val_t_CXXABI_1_3_11,_ZdlPvSt11align_val_t@CXXABI_1.3.11");
__asm__(".symver _ZdaPvSt11align_val_t_CXXABI_1_3_11,_ZdaPvSt11align_val_t@CXXABI_1.3.11");
__asm__(".symver _ZdlPvmSt11align_val_t_CXXABI_1_3_11,_ZdlPvmSt11align_val_t@CXXABI_1.3.11");
__asm__(".symver _ZdaPvmSt11align_val_t_CXXABI_1_3_11,_ZdaPvmSt11align_val_t@CXXABI_1.3.11");
__asm__(".symver _ZnwmSt11align_val_tRKSt9nothrow_t_CXXABI_1_3_11,_ZnwmSt11align_val_tRKSt9nothrow_t@CXXABI_1.3.11");
__asm__(".symver _ZnamSt11align_val_tRKSt9nothrow_t_CXXABI_1_3_11,_ZnamSt11align_val_tRKSt9nothrow_t@CXXABI_1.3.11");

#endif // __linux__
