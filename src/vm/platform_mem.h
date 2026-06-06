// smash/src/vm/platform_mem.h - Raw OS memory primitives (never calls malloc)
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include "../util/safe_printf.h"  // allocation-free snprintf

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace smash::vm {

// Map anonymous private pages. Returns nullptr on failure.
inline void* mapPages(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

// Reserve virtual address space without committing physical pages.
inline void* reservePages(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#elif defined(__linux__)
    void* p = mmap(nullptr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#else
    // macOS: pages are lazily backed, plain mmap suffices
    void* p = mmap(nullptr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

// Commit pages within a reserved region (make them accessible).
inline bool commitPages(void* addr, size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

// Decommit pages (release physical backing but keep virtual reservation).
inline void decommitPages(void* addr, size_t size) {
    // SMASH_NO_DECOMMIT=1 — debugging knob to disable physical reclamation.
    // Useful for diagnosing whether application crashes are caused by
    // smash zeroing pages that the application still references via a
    // use-after-free bug. With decommit off, freed pages keep their
    // contents and the UAF reads stale-but-still-shaped data, like
    // glibc would. Costs all RSS savings; only for diagnosis.
    static const bool no_decommit = []{
        const char* v = std::getenv("SMASH_NO_DECOMMIT");
        return v && v[0] == '1';
    }();
    if (no_decommit) return;
#if defined(_WIN32)
    VirtualFree(addr, size, MEM_DECOMMIT);
#elif defined(__linux__)
    // MADV_DONTNEED immediately frees the physical page AND shoots down the
    // TLB on every core mapping it — the dominant smash-induced IPI cost
    // (microbench: MADV_DONTNEED added ~45K of 102K TLB IPIs over 100K ops).
    // MADV_FREE is lazy: it marks the page reclaimable but doesn't drop it or
    // shoot down TLBs until the kernel hits memory pressure, cutting both the
    // IPI count (~11%) and wall time (~17%) in the microbench. The tradeoff is
    // that MADV_FREE pages remain in RSS until reclaimed, so on a
    // low-memory-pressure host the RSS win shrinks. Correctness is unaffected:
    // smash only decommits pages that are already PROT_NONE, so any access
    // faults into the handler and is overwritten with decompressed data before
    // the (possibly stale, possibly zero) underlying page is observed.
    // SMASH_MADV_FREE=1 selects the lazy path; default DONTNEED preserves the
    // current eager-reclaim RSS behavior.
    static const int madv = []{
        const char* v = std::getenv("SMASH_MADV_FREE");
        return (v && v[0] == '1') ? MADV_FREE : MADV_DONTNEED;
    }();
    madvise(addr, size, madv);
#elif defined(__APPLE__)
    // MADV_FREE_REUSABLE tells the kernel to reclaim physical backing immediately.
    // Must be called while pages are still accessible (PROT_READ or PROT_RW);
    // fails with EPERM on PROT_NONE pages.
    madvise(addr, size, MADV_FREE_REUSABLE);
#endif
}

// Unmap pages entirely (release virtual + physical).
inline void unmapPages(void* addr, size_t size) {
#if defined(_WIN32)
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    munmap(addr, size);
#endif
}

// Protect pages. Returns true on success, false on failure.
//
// Linux mprotect can fail with ENOMEM when the kernel's VMA split would
// take the process over `/proc/sys/vm/max_map_count` (default 65530).
// Smash creates one VMA boundary per compressed page since each page is
// PROT_NONE'd individually within a larger PROT_RW reservation. On long
// compiles with many cold pages, this hits the cap and subsequent
// mprotect() calls silently fail. Callers should check the return.
inline bool protectPages(void* addr, size_t size, bool read, bool write) {
#if defined(_WIN32)
    DWORD prot = PAGE_NOACCESS;
    if (read && write) prot = PAGE_READWRITE;
    else if (read) prot = PAGE_READONLY;
    DWORD old;
    return VirtualProtect(addr, size, prot, &old) != 0;
#else
    int prot = PROT_NONE;
    if (read) prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (mprotect(addr, size, prot) == 0) return true;
    int err = errno;
    static const bool trace = []{
        const char* v = getenv("SMASH_TRACE_MPROTECT_FAIL");
        return v && v[0] == '1';
    }();
    if (trace) {
        char buf[160];
        int n = smash::safe_snprintf(buf, sizeof(buf),
            "[smash mprotect-fail] addr=%p size=%zu prot=%d errno=%d\n",
            addr, size, prot, err);
        if (n > 0) (void)!::write(2, buf, (size_t)n);
    }
    errno = err;
    return false;
#endif
}

// Replace pages with a fresh anonymous mapping at the same address.
// Returns true on success. Used to collapse the per-page VMA fragmentation
// that mprotect-PROT_NONE creates so we don't hit vm.max_map_count.
//
// MAP_FIXED replaces the existing mapping atomically; the kernel will
// re-merge with adjacent identically-protected VMAs after the call.
inline bool remapPages(void* addr, size_t size, bool read, bool write) {
#if defined(_WIN32)
    // No analogous primitive on Windows that's strictly safer than
    // VirtualProtect — fall back to a plain protect call.
    return protectPages(addr, size, read, write);
#else
    int prot = PROT_NONE;
    if (read) prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    void* p = mmap(addr, size, prot,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p == addr;
#endif
}

} // namespace smash::vm
