// smash/src/vm/syscall_compat.h - Page warming + pinning for kernel syscall compatibility
//
// Kernel syscalls (kevent, read, recv, etc.) access userspace buffers directly
// without triggering SIGSEGV. If Smash has marked those pages PROT_READ or
// PROT_NONE (for compression monitoring), the kernel gets EFAULT instead.
//
// warmPages() touches each Smash-managed page in a buffer range before the
// syscall, triggering the fault handler to restore full access.
//
// pinPages()/unpinPages() prevent the compressor from re-protecting pages
// during long-blocking syscalls (e.g., read() on a FIFO).
#pragma once

#include "smash/config.h"
#include "vm_region.h"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <sys/uio.h>

#ifdef __APPLE__
#include <mach/message.h>
#endif

namespace smash::vm {

// Global pin-count array, indexed by VmRegion page index.
// Pages with pin_count > 0 are skipped by the compressor's monitoring phase.
// Allocated from BootstrapAlloc in SmashHeap constructor.
inline std::atomic<uint8_t>* g_page_pins = nullptr;

inline void warmPages(const void* buf, size_t len, VmRegion* vm) {
    if (!buf || !len || !vm) return;
    auto base = reinterpret_cast<uintptr_t>(buf);
    auto end = base + len;
    for (auto p = base & ~(static_cast<uintptr_t>(kPageSize) - 1); p < end; p += kPageSize) {
        if (!vm->contains(p)) continue;
        volatile char* vp = reinterpret_cast<volatile char*>(p);
        char c = *vp;   // read triggers ACTIVE_MONITORING -> ACTIVE
        *vp = c;        // write ensures PROT_READ|PROT_WRITE
    }
}

// Pin pages in a buffer range to prevent the compressor from
// re-protecting them during a blocking syscall.
inline void pinPages(const void* buf, size_t len, VmRegion* vm) {
    if (!buf || !len || !vm || !g_page_pins) return;
    auto base = reinterpret_cast<uintptr_t>(buf);
    auto end = base + len;
    for (auto p = base & ~(static_cast<uintptr_t>(kPageSize) - 1); p < end; p += kPageSize) {
        if (!vm->contains(p)) continue;
        size_t idx = vm->pageIndex(p);
        g_page_pins[idx].fetch_add(1, std::memory_order_relaxed);
    }
}

// Unpin pages after syscall completes.
inline void unpinPages(const void* buf, size_t len, VmRegion* vm) {
    if (!buf || !len || !vm || !g_page_pins) return;
    auto base = reinterpret_cast<uintptr_t>(buf);
    auto end = base + len;
    for (auto p = base & ~(static_cast<uintptr_t>(kPageSize) - 1); p < end; p += kPageSize) {
        if (!vm->contains(p)) continue;
        size_t idx = vm->pageIndex(p);
        g_page_pins[idx].fetch_sub(1, std::memory_order_relaxed);
    }
}

// Pin iovec array buffers.
inline void pinIovec(const struct iovec* iov, int iovcnt, VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len)
            pinPages(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

// Unpin iovec array buffers.
inline void unpinIovec(const struct iovec* iov, int iovcnt, VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len)
            unpinPages(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

// Retry a syscall on EFAULT after re-warming buffer pages.
//
// Closes the nanosecond-scale TOCTOU window between pinPages() in the
// caller and the compressor's per-page mprotect (compressor_thread.h
// phase3Range / escalateToDeepMonitoring): the kernel can synchronously
// touch a page right after we pin but before our pin-bump is observed,
// returning EFAULT instead of delivering a fault we could decompress.
//
// Caller holds the pin across all attempts. `rewarm` re-touches buffer
// pages so the user-space fault handler can restore PROT_READ|PROT_WRITE
// before we re-issue the syscall.
template <typename Syscall, typename Rewarm>
inline auto retryOnEfault(Syscall syscall, Rewarm rewarm)
    -> decltype(syscall()) {
    auto ret = syscall();
    for (int attempt = 0;
         ret == -1 && errno == EFAULT && attempt < 3;
         ++attempt) {
        rewarm();
        ret = syscall();
    }
    return ret;
}

// Touch one byte per page in [buf, buf+len) to trigger the fault handler.
// Reads alone are insufficient: a PROT_NONE page faults on read (handler
// decompresses → PROT_RW), but a PROT_READ monitoring page does not fault
// on read (read access is allowed) — only the write below transitions it
// back to PROT_RW via the handler. The read-then-write-back pattern is
// content-preserving and safe on any Smash-managed page (vm->contains()
// filter) since smash-protected pages always have PROT_RW underneath.
inline void walkPagesForFault(const void* buf, size_t len, VmRegion* vm) {
    if (!buf || !len || !vm) return;
    auto base = reinterpret_cast<uintptr_t>(buf);
    auto end = base + len;
    for (auto p = base & ~(static_cast<uintptr_t>(kPageSize) - 1); p < end; p += kPageSize) {
        if (!vm->contains(p)) continue;
        volatile char* vp = reinterpret_cast<volatile char*>(p);
        char c = *vp;   // PROT_NONE → fault → decompress
        *vp = c;        // PROT_READ → fault → upgrade to PROT_RW
    }
}

inline void walkIovecForFault(const struct iovec* iov, int iovcnt, VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len)
            walkPagesForFault(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

// Retry a syscall on EFAULT, walking buffer pages between attempts to
// trigger the fault handler. Up to 8 attempts with µs-scale backoff
// (1, 2, 4, ..., 128µs). The compressor tick runs at ~10ms, so even
// 8 attempts at 128µs is well inside one tick — outlasts any compressor
// race that could re-compress between walk and retry.
//
// Bounded retry (not unbounded) so a genuine bug — caller passed an
// unmapped pointer — still surfaces as EFAULT instead of livelocking.
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
// Mach equivalent: kernel signals a buffer-touch failure during mach_msg
// via MACH_RCV_INVALID_DATA / MACH_SEND_INVALID_DATA rather than EFAULT.
// Same retry shape as retryWithDecompress.
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
