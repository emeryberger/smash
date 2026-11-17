// smash/src/external_track.h — SMASH_TRACK_EXTERNAL registration policy and
// the lifecycle hooks shared by the macOS (smash_heap.cpp) and Linux
// (linux_syscall_wrappers.cpp) mmap / munmap / madvise / mprotect / mremap
// interposers. See agents/external-tracking.md.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include "smash_heap.h"

namespace smash {

namespace ext {

enum Mode : int { kUnmap = 0, kPurge = 1, kRestore = 2, kRestoreAll = 3 };

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("SMASH_TRACK_EXTERNAL");
        return v && v[0] == '1';
    }();
    return on;
}

inline uintptr_t pageDown(const void* p) {
    return reinterpret_cast<uintptr_t>(p) & ~(uintptr_t{kPageSize} - 1);
}
inline uintptr_t pageUp(const void* p, size_t len) {
    return (reinterpret_cast<uintptr_t>(p) + len + kPageSize - 1) & ~(uintptr_t{kPageSize} - 1);
}

// Which new mappings are registered. Deliberately strict:
//  - private anonymous only. File-backed pages would break msync and the OS
//    already evicts them. MAP_SHARED anon is shared with forked children:
//    a compressed blob in this process goes stale the moment the child
//    writes, and decompressing it would overwrite the child's data.
//  - exactly PROT_READ|PROT_WRITE. Read-only pages are never dirtied; smash
//    restores PROT_RW on decompress, which would strip PROT_EXEC from a JIT
//    region.
//  - not MAP_LOCKED (see mlock in the interposers).
//  - no stacks (MAP_STACK / MAP_GROWSDOWN): a thread whose stack page is
//    compressed has nowhere to push the SIGSEGV frame. No MAP_HUGETLB:
//    hugetlbfs pages can't be split into 4 KiB protections.
// Set by mlockall(MCL_FUTURE): every later mapping is locked, so none may
// be tracked.
inline std::atomic<bool> g_no_new_tracking{false};

inline bool eligibleMapping(int prot, int flags) {
    if (!(flags & MAP_ANON)) return false;
    if (g_no_new_tracking.load(std::memory_order_relaxed)) return false;
#ifdef MAP_LOCKED
    // Locked memory is meant to stay resident (and is often secret: keys
    // kept out of swap); never copy it into the compressed store.
    if (flags & MAP_LOCKED) return false;
#endif
#ifdef __linux__
    if ((flags & MAP_TYPE) != MAP_PRIVATE) return false;
    if (flags & (MAP_STACK | MAP_GROWSDOWN | MAP_HUGETLB)) return false;
#else
    if (flags & MAP_SHARED) return false;
#endif
    return prot == (PROT_READ | PROT_WRITE);
}

// madvise advice values that discard page contents.
inline bool discardAdvice(int advice) {
    switch (advice) {
    case MADV_DONTNEED:
#ifdef MADV_FREE
    case MADV_FREE:
#endif
#ifdef MADV_REMOVE
    case MADV_REMOVE:
#endif
#ifdef MADV_FREE_REUSABLE
    case MADV_FREE_REUSABLE:
#endif
#ifdef __linux__
    case 24:  // MADV_DONTNEED_LOCKED (Linux 5.18; not in older headers)
#endif
        return true;
    default:
        return false;
    }
}

// Register every page of [base, base+len) as an external ACTIVE page.
inline void registerRange(VmRegion* vm, void* base, size_t len) {
    if (!vm || !base || !len || !enabled()) return;
    // Profile-driven skip: a prior run found external pages hot.
    if (g_smash_skip_external_tracking.load(std::memory_order_acquire)) return;
    if (!g_smash_page_states_for_external) return;
    const size_t npages = (len + kPageSize - 1) / kPageSize;
    // Oversized single mapping guard: a mapping larger than the whole slot
    // budget can never be fully tracked, and such mappings are app-managed
    // arenas (a DB buffer pool, a GC heap) kept warm by the app — skip it
    // rather than track a useless prefix (registration of a 6 GiB InnoDB pool
    // wedged mariadbd startup). Logged once so the gap is never silent.
    if (npages > vm->externalSlotCapacity()) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            char buf[192];
            int n = safe_snprintf(buf, sizeof(buf),
                "[smash] SMASH_TRACK_EXTERNAL: skipping mmap of %zu pages "
                "(> %zu slot budget) at %p — likely an app-managed arena kept "
                "warm; not compressible. Further such skips are silent.\n",
                npages, vm->externalSlotCapacity(), base);
            if (n > 0) (void)!::write(2, buf, (size_t)n);
        }
        return;
    }
    const uintptr_t end = pageUp(base, len);
    for (uintptr_t p = pageDown(base); p < end; p += kPageSize) {
        size_t idx = vm->trackExternalPage(p);
        if (idx == 0) {
            if (vm->externalSlotsRemaining() == 0) {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true, std::memory_order_relaxed)) {
                    const char msg[] = "[smash] SMASH_TRACK_EXTERNAL: external slot budget "
                                       "full; further mappings are not tracked until "
                                       "tracked ones are unmapped.\n";
                    (void)!::write(2, msg, sizeof(msg) - 1);
                }
                return;
            }
            continue;  // page inside smash's own arena
        }
        g_smash_page_states_for_external->set(idx, PageState::ACTIVE);
    }
}

// Settle smash's state for [base, base+len) before the app changes it.
// Returns how many pages were tracked (0 when nothing is tracked — the cheap
// common case: one relaxed load).
inline size_t detachRange(VmRegion* vm, void* base, size_t len, Mode mode) {
    if (!vm || !base || !len || !enabled()) return 0;
    if (vm->externalLivePages() == 0) return 0;
    ExternalDetachFn fn = g_smash_external_detach.load(std::memory_order_acquire);
    if (!fn) return 0;
    return fn(pageDown(base), pageUp(base, len), mode);
}

// Untrack every external page (mlockall). Walks the external slots via the
// compressor (address order is irrelevant for kRestore).
inline void detachAll(VmRegion* vm) {
    if (!vm || !enabled() || vm->externalLivePages() == 0) return;
    ExternalDetachFn fn = g_smash_external_detach.load(std::memory_order_acquire);
    if (fn) fn(0, 0, kRestoreAll);
}

}  // namespace ext
}  // namespace smash
