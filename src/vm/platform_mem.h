// smash/src/vm/platform_mem.h - Raw OS memory primitives (never calls malloc)
#pragma once

#include <cstddef>
#include <cstdint>

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
#if defined(_WIN32)
    VirtualFree(addr, size, MEM_DECOMMIT);
#elif defined(__linux__)
    madvise(addr, size, MADV_DONTNEED);
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

// Protect pages.
inline void protectPages(void* addr, size_t size, bool read, bool write) {
#if defined(_WIN32)
    DWORD prot = PAGE_NOACCESS;
    if (read && write) prot = PAGE_READWRITE;
    else if (read) prot = PAGE_READONLY;
    DWORD old;
    VirtualProtect(addr, size, prot, &old);
#else
    int prot = PROT_NONE;
    if (read) prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    mprotect(addr, size, prot);
#endif
}

} // namespace smash::vm
