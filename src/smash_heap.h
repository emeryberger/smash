#pragma once
#include "smash/config.h"
#include "core/bootstrap_alloc.h"
#include "core/size_classes.h"
#include "core/span.h"
#include "core/page_map.h"
#include "core/slab.h"
#include "core/large_alloc.h"
#include "core/thread_cache.h"
#include "vm/vm_region.h"
#include "vm/page_state.h"
#include "vm/fault_handler.h"
#include "compress/compress_store.h"
#include "compress/compress_engine.h"
#include "compress/compressor_thread.h"
#include "util/bitops.h"
#include <cstddef>
#include <cstdint>
#include <atomic>

namespace smash {

// Counts threadInit() calls. The first call is the main thread during early
// init (before _objc_init completes). We defer compression start until the
// second threadInit() call, which only happens after full init is complete.
extern std::atomic<int> g_thread_init_count;

// Global pointer to the VmRegion, used by syscall wrappers to warm pages.
// Set after SmashHeap construction when compression is enabled.
extern VmRegion* g_smash_vm_region;

inline ThreadCache*& currentThreadCache() {
    static thread_local ThreadCache* cache = nullptr;
    return cache;
}

class SmashHeap {
    Slab slabs_[kNumArenas * kNumClasses];  // flat 2D: arena * kNumClasses + sc
    LargeAlloc large_alloc_;
    PageMap page_map_;

    Slab& slab(uint8_t arena, uint8_t sc) { return slabs_[arena * kNumClasses + sc]; }

    static uint8_t callsiteArena() {
#ifdef SMASH_ABLATION_NO_CALLSITE_ARENA
        return 0;
#else
        // XOR return addresses from two stack depths for arena routing.
        // xxmalloc tail-calls SmashHeap::malloc, so:
        //   depth 0 = app's malloc() callsite (or app wrapper)
        //   depth 1 = caller of that function
        // XORing both ensures different routing even through a common
        // application-level malloc wrapper.
        uintptr_t h = reinterpret_cast<uintptr_t>(__builtin_return_address(0))
                    ^ reinterpret_cast<uintptr_t>(__builtin_return_address(1));
        h ^= h >> 16;
        return static_cast<uint8_t>(h & (kNumArenas - 1));
#endif
    }

    // Phase 2-4: compression infrastructure
    VmRegion vm_region_;
    PageStateTable page_states_;
    PageLockTable page_locks_;
    CompressStore compress_store_;
    CompressEngine compress_engine_;
    CompressorThread compressor_;
    vm::FaultHandler fault_handler_;

    bool compression_inited_ = false;
    std::atomic<bool> compression_started_{false};

    // Fault callback: decompress on access to compressed/monitored pages
    static bool faultCallback(uintptr_t fault_addr, void* ctx) {
        auto* self = static_cast<SmashHeap*>(ctx);
        return self->compressor_.handleFault(fault_addr);
    }

    // Release hook: called by slab when freeing spans that may be compressed
    static void releaseHook(size_t page_idx, size_t page_count, void* ctx) {
        auto* self = static_cast<SmashHeap*>(ctx);
        self->compressor_.releaseCompressedPages(page_idx, page_count);
    }

    void startCompression() {
        bool expected = false;
        if (!compression_started_.compare_exchange_strong(expected, true))
            return;
        fault_handler_.start(faultCallback, this);
        compressor_.start();
    }

public:
    SmashHeap() {
        page_map_.init();

        // Try to init VmRegion for compression support
        bool vm_ok = vm_region_.init(kVmRegionSize);

        if (vm_ok) {
            page_states_.init(vm_region_.totalPages());
            page_locks_.init(vm_region_.totalPages());
            compress_store_.init();
            compress_engine_.init();
            compressor_.init(&vm_region_, &page_states_, &page_locks_,
                             &compress_store_, &compress_engine_,
                             &page_map_, &fault_handler_);

            for (int a = 0; a < kNumArenas; ++a)
                for (int i = 0; i < kNumClasses; ++i)
                    slabs_[a * kNumClasses + i].init(
                        static_cast<uint8_t>(i), &page_map_,
                        &vm_region_, &page_states_,
                        releaseHook, this,
                        static_cast<uint8_t>(a));
            compression_inited_ = true;
            g_smash_vm_region = &vm_region_;
            vm::g_page_pins = bootstrapArray<std::atomic<uint8_t>>(
                vm_region_.totalPages());
        } else {
            // Fallback: Phase 1 mode (no compression)
            for (int a = 0; a < kNumArenas; ++a)
                for (int i = 0; i < kNumClasses; ++i)
                    slabs_[a * kNumClasses + i].init(
                        static_cast<uint8_t>(i), &page_map_,
                        nullptr, nullptr, nullptr, nullptr,
                        static_cast<uint8_t>(a));
        }

        if (compression_inited_) {
            large_alloc_.init(&page_map_, &vm_region_, &page_states_,
                              releaseHook, this);
        } else {
            large_alloc_.init(&page_map_);
        }
    }

    ThreadCache* getOrCreateThreadCache() {
        ThreadCache*& tc = currentThreadCache();
        if (!tc) tc = newThreadCache();
        return tc;
    }

    void* malloc(size_t size) {
        if (size == 0) size = 1;
        uint8_t sc = sizeToClass(size);
        if (sc < kNumClasses) {
            ThreadCache* tc = getOrCreateThreadCache();
            void* ptr = tc->allocate(sc);
            if (ptr) return ptr;
            return tc->refill(sc, &slab(callsiteArena(), sc));
        }
        return large_alloc_.allocate(size, kMinAlignment);
    }

    void free(void* ptr) {
        if (!ptr) return;
        if (BootstrapAlloc::instance().owns(ptr)) return;
        Span* span = page_map_.get(reinterpret_cast<uintptr_t>(ptr));
        if (!span) return;
        if (span->is_large) { large_alloc_.deallocate(span); return; }
        uint8_t sc = span->size_class;
        ThreadCache* tc = getOrCreateThreadCache();
        if (!tc->deallocate(sc, ptr)) { tc->drain(sc, slabs_, &page_map_); tc->deallocate(sc, ptr); }
    }

    void* memalign(size_t alignment, size_t size) {
        if (size == 0) size = 1;
        if (alignment <= kMinAlignment) return this->malloc(size);
        return large_alloc_.allocate(size, alignment);
    }

    size_t getSize(void* ptr) {
        if (!ptr) return 0;
        if (BootstrapAlloc::instance().owns(ptr)) return 0;
        Span* span = page_map_.get(reinterpret_cast<uintptr_t>(ptr));
        if (!span) return 0;
        if (span->is_large) return span->large_size;
        return classSize(span->size_class);
    }

    void lock() {
        for (int i = 0; i < kNumArenas * kNumClasses; ++i) slabs_[i].lockSlab();
        large_alloc_.lockAlloc();
    }
    void unlock() {
        large_alloc_.unlockAlloc();
        for (int i = kNumArenas * kNumClasses - 1; i >= 0; --i) slabs_[i].unlockSlab();
    }
    void threadInit() {
        getOrCreateThreadCache();
        // Start compression after the second thread init call.
        // The first call is the main thread during early library init
        // (before _objc_init). Subsequent calls happen after init is safe.
        if (compression_inited_ &&
            !compression_started_.load(std::memory_order_acquire) &&
            g_thread_init_count.fetch_add(1, std::memory_order_acq_rel) >= 1) {
            startCompression();
        }
    }
    void threadCleanup() {
        ThreadCache*& tc = currentThreadCache();
        if (tc) { tc->drainAll(slabs_, &page_map_); returnThreadCache(tc); tc = nullptr; }
    }
};
} // namespace smash
