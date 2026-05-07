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
#include <sys/select.h>
#include <sys/random.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <poll.h>
#include <cstdarg>
#include <cstdio>
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

// Use explicit visibility attribute - pragma doesn't work with -fvisibility=hidden
#define SMASH_VISIBLE __attribute__((visibility("default")))

extern "C" {

SMASH_VISIBLE ssize_t read(int fd, void* buf, size_t count) {
    using fn_t = ssize_t(*)(int, void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, read);
    if (!real_read) return syscall(SYS_read, fd, buf, count);
    return smash::vm::retryWith1Buf(
        [&] { return real_read(fd, buf, count); },
        buf, count);
}

SMASH_VISIBLE ssize_t write(int fd, const void* buf, size_t count) {
    using fn_t = ssize_t(*)(int, const void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, write);
    if (!real_write) return syscall(SYS_write, fd, buf, count);
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
    if (!real_poll) return syscall(SYS_poll, fds, nfds, timeout);
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
    if (!real_select) return syscall(SYS_select, nfds, readfds, writefds, exceptfds, timeout);
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

// ── getcwd buffer hooks ─────────────────────────────────────────────────────
// Function-pointer hook surface intended for alloc8's getcwd wrapper to
// call into smash so we can warm+pin the destination buffer before the
// kernel writes into it. alloc8's current Linux gnu_wrapper.cpp does not
// yet call through these — getcwd buffers therefore aren't pinned in
// today's build — but we still need the storage defined here so that
// libsmash.so's LD_PRELOAD load doesn't fail with
//   undefined symbol: xx_getcwd_finish_hook
// when no other module in the process provides it. install_getcwd_hooks
// (below) populates the pointers; an alloc8 update can later wire the
// call sites without touching libsmash.so.
using xx_getcwd_hook_fn = void(*)(void*, size_t);
xx_getcwd_hook_fn xx_getcwd_prepare_hook = nullptr;
xx_getcwd_hook_fn xx_getcwd_finish_hook  = nullptr;

static void smash_getcwd_prepare(void* buf, size_t size) {
    auto* vm = smash::g_smash_vm_region;
    if (bufferInHeap(buf, size, vm)) {
        smash::vm::warmPages(buf, size, vm);
    }
}
static void smash_getcwd_finish(void* /*buf*/, size_t /*size*/) {
    // No-op: pin counters were removed in the EFAULT-retry refactor;
    // the prepare-side warmPages above is sufficient since getcwd has
    // no retry surface and the buffer is short-lived.
}

// Install hooks via a constructor that runs after alloc8 is initialized.
__attribute__((constructor(200)))
static void install_getcwd_hooks() {
    xx_getcwd_prepare_hook = smash_getcwd_prepare;
    xx_getcwd_finish_hook  = smash_getcwd_finish;
}

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

SMASH_VISIBLE size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    using fn_t = size_t(*)(void*, size_t, size_t, FILE*);
    SMASH_LAZY_RESOLVE(fn_t, fread);
    if (!real_fread) return 0;
    auto* vm = smash::g_smash_vm_region;
    size_t total = size * nmemb;
    bool pin_buf = total && bufferInHeap(ptr, total, vm);
    if (pin_buf) smash::vm::warmPages(ptr, total, vm);
    warmGlibcFileBuffer(stream, vm);
    size_t ret = real_fread(ptr, size, nmemb, stream);
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
    char* ret = real_fgets(s, size, stream);
    return ret;
}

SMASH_VISIBLE int fgetc(FILE* stream) {
    using fn_t = int(*)(FILE*);
    SMASH_LAZY_RESOLVE(fn_t, fgetc);
    if (!real_fgetc) return EOF;
    auto* vm = smash::g_smash_vm_region;
    warmGlibcFileBuffer(stream, vm);
    int ret = real_fgetc(stream);
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
    if (!real_epoll_wait) return syscall(SYS_epoll_wait, epfd, events, maxevents, timeout);
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

// ── External-mapping interposers (mmap / munmap) ────────────────────────────
//
// Application-direct mmap calls (e.g., SpiderMonkey GC arenas, jemalloc
// internal slabs in projects that bundle it) bypass smash's malloc and so
// escape compression. Track MAP_ANONYMOUS + PROT_WRITE mappings via
// VmRegion's external-page hash so the compressor's tick can see them.
// File-backed and read-only mappings are explicitly skipped (the former
// would break msync semantics under compression; the latter never need it).
//
// External tracking is OFF by default; set SMASH_TRACK_EXTERNAL=1 to
// enable. Same opt-in polarity as the macOS path — see smash_heap.cpp
// for the rationale (Firefox-on-macOS regression with the registration
// path active).

namespace {

inline bool externalTrackingEnabledLinux() {
    static const bool enabled = []{
        const char* v = std::getenv("SMASH_TRACK_EXTERNAL");
        return v && v[0] == '1';
    }();
    return enabled;
}

inline void registerLinuxExternalRange(smash::VmRegion* vm, void* base, size_t len) {
    if (!vm || !base || !len) return;
    if (!externalTrackingEnabledLinux()) return;
    auto start = reinterpret_cast<uintptr_t>(base) & ~(uintptr_t{smash::kPageSize} - 1);
    auto end = (reinterpret_cast<uintptr_t>(base) + len + smash::kPageSize - 1)
               & ~(uintptr_t{smash::kPageSize} - 1);
    for (uintptr_t p = start; p < end; p += smash::kPageSize) {
        size_t idx = vm->trackExternalPage(p);
        if (idx == 0) continue;
        if (smash::g_smash_page_states_for_external)
            smash::g_smash_page_states_for_external->set(idx, smash::PageState::ACTIVE);
    }
}

inline void deregisterLinuxExternalRange(smash::VmRegion* vm, void* base, size_t len) {
    if (!vm || !base || !len) return;
    if (!externalTrackingEnabledLinux()) return;
    auto start = reinterpret_cast<uintptr_t>(base) & ~(uintptr_t{smash::kPageSize} - 1);
    auto end = (reinterpret_cast<uintptr_t>(base) + len + smash::kPageSize - 1)
               & ~(uintptr_t{smash::kPageSize} - 1);
    for (uintptr_t p = start; p < end; p += smash::kPageSize) {
        size_t idx = vm->pageIndex(p);
        if (idx == 0) continue;
        if (smash::g_smash_page_states_for_external)
            smash::g_smash_page_states_for_external->set(idx, smash::PageState::EMPTY);
        vm->untrackExternalPage(p);
    }
}

}  // namespace

SMASH_VISIBLE void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    using fn_t = void*(*)(void*, size_t, int, int, int, off_t);
    SMASH_LAZY_RESOLVE(fn_t, mmap);
    if (!real_mmap) return MAP_FAILED;
    void* ret = real_mmap(addr, len, prot, flags, fd, offset);
    if (ret == MAP_FAILED) return ret;
    bool anon = (flags & MAP_ANONYMOUS) != 0;
    bool writable = (prot & PROT_WRITE) != 0;
    if (!anon || !writable || len == 0) return ret;
    auto* vm = smash::g_smash_vm_region;
    if (vm) registerLinuxExternalRange(vm, ret, len);
    return ret;
}

SMASH_VISIBLE int munmap(void* addr, size_t len) {
    using fn_t = int(*)(void*, size_t);
    SMASH_LAZY_RESOLVE(fn_t, munmap);
    if (!real_munmap) return -1;
    auto* vm = smash::g_smash_vm_region;
    if (vm && addr && len) deregisterLinuxExternalRange(vm, addr, len);
    return real_munmap(addr, len);
}

} // extern "C"

// Create version aliases: epoll_wait_232 -> epoll_wait@GLIBC_2.3.2
__asm__(".symver epoll_wait_232,epoll_wait@GLIBC_2.3.2");
__asm__(".symver epoll_pwait_232,epoll_pwait@GLIBC_2.3.2");
__asm__(".symver fstat_233,fstat@GLIBC_2.33");
__asm__(".symver fstat64_233,fstat64@GLIBC_2.33");

#endif // __linux__
