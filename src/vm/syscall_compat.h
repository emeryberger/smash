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
#include <cstddef>
#include <cstdint>
#include <sys/uio.h>

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

} // namespace smash::vm
