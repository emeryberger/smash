// smash/src/core/bootstrap_alloc.h - Bump allocator for internal metadata
//
// All smash metadata (span descriptors, bitmaps, page map tables, thread caches)
// is allocated from here. This avoids any dependency on the managed heap.
// Memory is never freed — just bump-allocated from mmap'd regions.
#pragma once

#include "smash/config.h"
#include "../vm/platform_mem.h"
#include "../util/spinlock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace smash {

class BootstrapAlloc {
    struct Region {
        char* base;
        size_t capacity;
        std::atomic<size_t> offset;
    };

    Region regions_[kBootstrapMaxRegions];
    std::atomic<size_t> num_regions_{0};
    Spinlock expand_lock_;

    void* tryAllocFrom(Region& r, size_t size, size_t align) {
        size_t off = r.offset.load(std::memory_order_relaxed);
        for (;;) {
            size_t aligned = (off + align - 1) & ~(align - 1);
            size_t end = aligned + size;
            if (end > r.capacity) return nullptr;
            if (r.offset.compare_exchange_weak(off, end, std::memory_order_relaxed))
                return r.base + aligned;
        }
    }

    bool addRegion(size_t size) {
        void* mem = vm::mapPages(size);
        if (!mem) return false;
        size_t idx = num_regions_.load(std::memory_order_relaxed);
        if (idx >= kBootstrapMaxRegions) {
            vm::unmapPages(mem, size);
            return false;
        }
        regions_[idx].base = static_cast<char*>(mem);
        regions_[idx].capacity = size;
        regions_[idx].offset.store(0, std::memory_order_relaxed);
        num_regions_.store(idx + 1, std::memory_order_release);
        return true;
    }

    void* expandAndAllocate(size_t size, size_t align) {
        LockGuard guard(expand_lock_);
        // Double-check: another thread may have expanded while we waited
        size_t n = num_regions_.load(std::memory_order_acquire);
        if (n > 0) {
            void* p = tryAllocFrom(regions_[n - 1], size, align);
            if (p) return p;
        }
        size_t region_size = kBootstrapExpandSize;
        if (size + align > region_size) region_size = size + align;
        if (!addRegion(region_size)) return nullptr;
        n = num_regions_.load(std::memory_order_acquire);
        return tryAllocFrom(regions_[n - 1], size, align);
    }

    BootstrapAlloc() {
        addRegion(kBootstrapInitialSize);
    }

public:
    static BootstrapAlloc& instance() {
        // Manual singleton to avoid __cxa_guard (which might call malloc on some platforms)
        alignas(BootstrapAlloc) static char buf[sizeof(BootstrapAlloc)];
        static std::atomic<int> state{0};  // 0=uninit, 1=initializing, 2=ready
        int s = state.load(std::memory_order_acquire);
        if (s == 2) [[likely]]
            return *reinterpret_cast<BootstrapAlloc*>(buf);
        if (s == 0 && state.compare_exchange_strong(s, 1, std::memory_order_acq_rel)) {
            new (buf) BootstrapAlloc();
            state.store(2, std::memory_order_release);
            return *reinterpret_cast<BootstrapAlloc*>(buf);
        }
        // Another thread initializing — spin
        while (state.load(std::memory_order_acquire) != 2) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
        }
        return *reinterpret_cast<BootstrapAlloc*>(buf);
    }

    void* allocate(size_t size, size_t align = 16) {
        if (size == 0) size = 1;
        // Try most recent region first (most likely to have space)
        size_t n = num_regions_.load(std::memory_order_acquire);
        for (size_t i = n; i > 0; --i) {
            void* p = tryAllocFrom(regions_[i - 1], size, align);
            if (p) return p;
        }
        return expandAndAllocate(size, align);
    }

    // Zero-initialized allocation
    void* allocateZeroed(size_t size, size_t align = 16) {
        void* p = allocate(size, align);
        if (p) __builtin_memset(p, 0, size);
        return p;
    }

    bool owns(const void* ptr) const {
        auto p = reinterpret_cast<uintptr_t>(ptr);
        size_t n = num_regions_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            auto base = reinterpret_cast<uintptr_t>(regions_[i].base);
            if (p >= base && p < base + regions_[i].capacity) return true;
        }
        return false;
    }
};

// Convenience: allocate a zeroed array of T from bootstrap
template<typename T>
T* bootstrapArray(size_t count) {
    size_t bytes = count * sizeof(T);
    return static_cast<T*>(BootstrapAlloc::instance().allocateZeroed(bytes, alignof(T)));
}

} // namespace smash
