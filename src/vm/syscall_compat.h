// smash/src/vm/syscall_compat.h - Page warming for kernel syscall compatibility
//
// Kernel syscalls (kevent, read, recv, etc.) access userspace buffers directly
// without raising SIGSEGV: when copy_to_user / copy_from_user (Linux) or
// copyin / copyout (macOS) hits a protected page during a syscall, the kernel
// uses the page-fault fixup path to convert the fault into -EFAULT and discard
// the address. So our SEGV/SIGBUS handler never fires for syscall-side faults
// — we have to detect EFAULT in userspace and trigger a touch ourselves
// (which DOES go through the fault handler) so the page can be decompressed.
//
// retryWithDecompress is the canonical pattern: call the syscall, on EFAULT
// walk the buffer pages (one byte per page; SEGV → handler → decompress → RW)
// then retry. Bounded to 8 attempts with µs-scale backoff so unmapped-pointer
// bugs surface as EFAULT instead of livelocking. retryMachOnInvalidData is
// the same shape but keyed on MACH_RCV_INVALID_DATA / MACH_SEND_INVALID_DATA
// since mach_msg uses Mach error codes rather than errno.
//
// warmPages is a proactive read-then-write-back over each page in a buffer.
// Used by buffered-I/O wrappers (fread/fgets/fwrite, …) where libc's internal
// __read happens intra-dylib and never enters our wrapper, so EFAULT can't be
// observed/retried; proactive warming is the only defense.
#pragma once

#include "smash/config.h"
#include "vm_region.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <sys/uio.h>

#ifdef __APPLE__
#include <mach/message.h>
#endif

namespace smash::vm {

// Proactive read-then-write of one byte per page in [buf, buf+len).
// Used by wrappers that have no retry surface (intra-libSystem buffered I/O,
// getcwd hooks). PROT_NONE pages fault on read → handler decompresses; the
// write-back transitions PROT_READ monitoring pages back to PROT_RW.
inline void warmPages(const void* buf, size_t len, VmRegion* vm) {
    if (!buf || !len || !vm) return;
    auto base = reinterpret_cast<uintptr_t>(buf);
    auto end = base + len;
    for (auto p = base & ~(static_cast<uintptr_t>(kPageSize) - 1); p < end; p += kPageSize) {
        if (!vm->contains(p)) continue;
        volatile char* vp = reinterpret_cast<volatile char*>(p);
        char c = *vp;
        *vp = c;
    }
}

// Same touch pattern as warmPages but used from the EFAULT-retry callback —
// kept as a separate name so call sites are self-documenting.
inline void walkPagesForFault(const void* buf, size_t len, VmRegion* vm) {
    warmPages(buf, len, vm);
}

inline void walkIovecForFault(const struct iovec* iov, int iovcnt, VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len)
            walkPagesForFault(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

// Up to 8 retries with µs-scale exponential backoff (1, 2, 4, ..., 128µs).
// The compressor tick runs at ~10ms; 8 attempts at 128µs is well inside
// one tick — outlasts any compressor race that could re-compress between
// walk and retry. Bounded so a genuine bug (caller passed an unmapped
// pointer) still surfaces as EFAULT instead of livelocking.
template <typename Syscall, typename Walk>
inline auto retryWithDecompress(Syscall syscall, Walk walk)
    -> decltype(syscall()) {
    auto ret = syscall();
    long backoff_ns = 1000;  // 1µs
    for (int attempt = 0;
         ret == -1 && errno == EFAULT && attempt < 8;
         ++attempt) {
        walk();
        if (attempt > 0) {
            struct timespec ts = {0, backoff_ns};
            nanosleep(&ts, nullptr);
            backoff_ns *= 2;
        }
        ret = syscall();
    }
    return ret;
}

#ifdef __APPLE__
// Mach equivalent: mach_msg signals buffer-touch failure via
// MACH_RCV_INVALID_DATA / MACH_SEND_INVALID_DATA rather than EFAULT.
template <typename Syscall, typename Walk>
inline mach_msg_return_t retryMachOnInvalidData(Syscall syscall, Walk walk) {
    auto ret = syscall();
    long backoff_ns = 1000;
    for (int attempt = 0;
         (ret == MACH_RCV_INVALID_DATA || ret == MACH_SEND_INVALID_DATA) && attempt < 8;
         ++attempt) {
        walk();
        if (attempt > 0) {
            struct timespec ts = {0, backoff_ns};
            nanosleep(&ts, nullptr);
            backoff_ns *= 2;
        }
        ret = syscall();
    }
    return ret;
}
#endif

} // namespace smash::vm
