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
#include <dlfcn.h>

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

// ── System malloc/free pointers for compress-only mode ──────────────────────
// Resolved early before any malloc interposition is active.
using MallocFn = void*(*)(size_t);
using FreeFn = void(*)(void*);
using CallocFn = void*(*)(size_t, size_t);
using ReallocFn = void*(*)(void*, size_t);
using MemalignFn = int(*)(void**, size_t, size_t);

struct SystemAllocFns {
    MallocFn malloc = nullptr;
    FreeFn free = nullptr;
    CallocFn calloc = nullptr;
    ReallocFn realloc = nullptr;
    MemalignFn posix_memalign = nullptr;

    void resolve() {
        malloc = reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
        free = reinterpret_cast<FreeFn>(dlsym(RTLD_NEXT, "free"));
        calloc = reinterpret_cast<CallocFn>(dlsym(RTLD_NEXT, "calloc"));
        realloc = reinterpret_cast<ReallocFn>(dlsym(RTLD_NEXT, "realloc"));
        posix_memalign = reinterpret_cast<MemalignFn>(dlsym(RTLD_NEXT, "posix_memalign"));
    }
};

extern SystemAllocFns g_system_alloc;

class SmashHeap {
    Slab slabs_[kNumArenas * kNumClasses];  // flat 2D: arena * kNumClasses + sc
    LargeAlloc large_alloc_;
    PageMap page_map_;

    Slab& slab(uint8_t arena, uint8_t sc) { return slabs_[arena * kNumClasses + sc]; }

    static uint8_t callsiteArena(uint8_t sc) {
#ifdef SMASH_ABLATION_NO_CALLSITE_ARENA
        return 0;
#else
        // LLAMA-style stack hash [Maas et al., ASPLOS 2020]:
        // hash(return_address, stack_height, object_size).
        //
        // Return address (depth 0) identifies the immediate call site.
        // Stack height (via __builtin_frame_address(0), safe at depth 0)
        // distinguishes calls through different wrapper chains that share
        // the same immediate call site.  Size class adds object-type
        // context.  This replaces the prior __builtin_return_address(1)
        // approach, which required frame-pointer walking and triggered
        // -Wframe-address warnings.
        uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        uintptr_t sh = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        uintptr_t h = ra ^ (sh >> 4) ^ static_cast<uintptr_t>(sc);
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
    std::atomic<int> warmup_count_{0};

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

    // Track allocation in compress-only mode
    void trackAllocation(void* ptr, size_t size) {
        if (!ptr || size == 0 || !compression_inited_) return;
        uintptr_t start_page = reinterpret_cast<uintptr_t>(ptr) & ~(kPageSize - 1);
        uintptr_t end_page = (reinterpret_cast<uintptr_t>(ptr) + size - 1) & ~(kPageSize - 1);

        for (uintptr_t p = start_page; p <= end_page; p += kPageSize) {
            size_t idx = vm_region_.trackPage(p);
            if (idx > 0) {
                PageState st = page_states_.get(idx);
                if (st == PageState::EMPTY)
                    page_states_.set(idx, PageState::ACTIVE);
            }
        }

        // Start compression after warmup
        if (!compression_started_.load(std::memory_order_relaxed)) {
            if (warmup_count_.fetch_add(1, std::memory_order_relaxed) >= 5000)
                startCompression();
        }
    }

public:
    SmashHeap() {
        bool compress_only = isCompressOnlyMode();

        if (!compress_only) {
            page_map_.init();
        }

        // Try to init VmRegion for compression support
        bool vm_ok = vm_region_.init(kVmRegionSize);

        if (vm_ok) {
            page_states_.init(vm_region_.totalPages());
            page_locks_.init(vm_region_.totalPages());
            compress_store_.init();
            compress_engine_.init();
            compressor_.init(&vm_region_, &page_states_, &page_locks_,
                             &compress_store_, &compress_engine_,
                             compress_only ? nullptr : &page_map_, &fault_handler_);

            if (!compress_only) {
                for (int a = 0; a < kNumArenas; ++a)
                    for (int i = 0; i < kNumClasses; ++i)
                        slabs_[a * kNumClasses + i].init(
                            static_cast<uint8_t>(i), &page_map_,
                            &vm_region_, &page_states_,
                            releaseHook, this,
                            static_cast<uint8_t>(a));
            }
            compression_inited_ = true;
            g_smash_vm_region = &vm_region_;
            vm::g_page_pins = bootstrapArray<std::atomic<uint8_t>>(
                vm_region_.totalPages());
        } else if (!compress_only) {
            // Fallback: Phase 1 mode (no compression)
            for (int a = 0; a < kNumArenas; ++a)
                for (int i = 0; i < kNumClasses; ++i)
                    slabs_[a * kNumClasses + i].init(
                        static_cast<uint8_t>(i), &page_map_,
                        nullptr, nullptr, nullptr, nullptr,
                        static_cast<uint8_t>(a));
        }

        if (!compress_only) {
            if (compression_inited_) {
                large_alloc_.init(&page_map_, &vm_region_, &page_states_,
                                  releaseHook, this);
            } else {
                large_alloc_.init(&page_map_);
            }
        }
    }

    ThreadCache* getOrCreateThreadCache() {
        ThreadCache*& tc = currentThreadCache();
        if (!tc) tc = newThreadCache();
        return tc;
    }

    void* malloc(size_t size) {
        if (isCompressOnlyMode()) {
            // During early init, g_system_alloc may not be resolved yet
            if (!g_system_alloc.malloc) return nullptr;
            void* ptr = g_system_alloc.malloc(size);
            trackAllocation(ptr, size);
            return ptr;
        }

        if (size == 0) size = 1;
        uint8_t sc = sizeToClass(size);
        if (sc < kNumClasses) {
            ThreadCache* tc = getOrCreateThreadCache();
            void* ptr = tc->allocate(sc);
            if (ptr) return ptr;
            return tc->refill(sc, &slab(callsiteArena(sc), sc));
        }
        return large_alloc_.allocate(size, kMinAlignment);
    }

    void free(void* ptr) {
        if (!ptr) return;
        if (BootstrapAlloc::instance().owns(ptr)) return;

        if (isCompressOnlyMode()) {
            if (g_system_alloc.free) g_system_alloc.free(ptr);
            return;
        }

        Span* span = page_map_.get(reinterpret_cast<uintptr_t>(ptr));
        if (!span) return;
        if (span->is_large) { large_alloc_.deallocate(span); return; }
        uint8_t sc = span->size_class;
        ThreadCache* tc = getOrCreateThreadCache();
        if (!tc->deallocate(sc, ptr)) { tc->drain(sc, slabs_, &page_map_); tc->deallocate(sc, ptr); }
    }

    void* memalign(size_t alignment, size_t size) {
        if (isCompressOnlyMode()) {
            if (!g_system_alloc.posix_memalign) return nullptr;
            void* ptr = nullptr;
            if (g_system_alloc.posix_memalign(&ptr, alignment, size) == 0) {
                trackAllocation(ptr, size);
                return ptr;
            }
            return nullptr;
        }

        if (size == 0) size = 1;
        if (alignment <= kMinAlignment) return this->malloc(size);
        return large_alloc_.allocate(size, alignment);
    }

    void* calloc(size_t count, size_t size) {
        if (isCompressOnlyMode()) {
            if (!g_system_alloc.calloc) return nullptr;
            void* ptr = g_system_alloc.calloc(count, size);
            trackAllocation(ptr, count * size);
            return ptr;
        }
        // In full mode, alloc8 handles calloc -> malloc + memset
        size_t total = count * size;
        void* ptr = this->malloc(total);
        if (ptr) __builtin_memset(ptr, 0, total);
        return ptr;
    }

    void* realloc(void* old_ptr, size_t size) {
        if (isCompressOnlyMode()) {
            if (!g_system_alloc.realloc) return nullptr;
            void* ptr = g_system_alloc.realloc(old_ptr, size);
            trackAllocation(ptr, size);
            return ptr;
        }
        // In full mode, alloc8 handles realloc
        if (!old_ptr) return this->malloc(size);
        if (size == 0) { this->free(old_ptr); return nullptr; }
        size_t old_size = getSize(old_ptr);
        void* new_ptr = this->malloc(size);
        if (new_ptr) {
            __builtin_memcpy(new_ptr, old_ptr, old_size < size ? old_size : size);
            this->free(old_ptr);
        }
        return new_ptr;
    }

    size_t getSize(void* ptr) {
        if (!ptr) return 0;
        if (BootstrapAlloc::instance().owns(ptr)) return 0;

        if (isCompressOnlyMode()) {
            return 0;  // Can't determine size for system allocations
        }

        Span* span = page_map_.get(reinterpret_cast<uintptr_t>(ptr));
        if (!span) return 0;
        if (span->is_large) return span->large_size;
        return classSize(span->size_class);
    }

    void lock() {
        if (isCompressOnlyMode()) return;
        for (int i = 0; i < kNumArenas * kNumClasses; ++i) slabs_[i].lockSlab();
        large_alloc_.lockAlloc();
    }
    void unlock() {
        if (isCompressOnlyMode()) return;
        large_alloc_.unlockAlloc();
        for (int i = kNumArenas * kNumClasses - 1; i >= 0; --i) slabs_[i].unlockSlab();
    }
    void threadInit() {
        if (!isCompressOnlyMode()) {
            getOrCreateThreadCache();
        }
        // Start compression after the second thread init call on macOS.
        // The first call is the main thread during early DYLD_INSERT init
        // (before _objc_init). Subsequent calls happen after init is safe.
        // On Linux, LD_PRELOAD init is complete before threadInit is called,
        // so we can start immediately on the first call.
        if (compression_inited_ &&
            !compression_started_.load(std::memory_order_acquire)) {
#ifdef __APPLE__
            if (g_thread_init_count.fetch_add(1, std::memory_order_acq_rel) >= 1)
#else
            g_thread_init_count.fetch_add(1, std::memory_order_acq_rel);
#endif
            startCompression();
        }
    }
    void threadCleanup() {
        if (isCompressOnlyMode()) return;
        ThreadCache*& tc = currentThreadCache();
        if (tc) { tc->drainAll(slabs_, &page_map_); returnThreadCache(tc); tc = nullptr; }
    }
};
} // namespace smash
