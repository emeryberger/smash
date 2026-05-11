#pragma once
#include <cmath>
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
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#endif

namespace smash {

// Counts threadInit() calls. The first call is the main thread during early
// init (before _objc_init completes). We defer compression start until the
// second threadInit() call, which only happens after full init is complete.
extern std::atomic<int> g_thread_init_count;

// Global pointer to the VmRegion, used by syscall wrappers to warm pages.
// Set after SmashHeap construction when compression is enabled.
extern VmRegion* g_smash_vm_region;

// Global pointer to the PageStateTable, used by the mmap / Mach VM
// interposers to mark newly-tracked external pages as ACTIVE (or EMPTY
// on unmap). Set when compression is initialized; nullptr otherwise.
// Same lifetime contract as g_smash_vm_region.
extern PageStateTable* g_smash_page_states_for_external;

// TLS variables on the malloc fast path are declared extern at namespace
// scope with tls_model("initial-exec"), and defined in smash_heap.cpp.
// libsmash is always loaded via LD_PRELOAD (so its TLS block is part of
// the startup-time TLS reservation), which makes initial-exec safe.
// Without this, the compiler emits __tls_get_addr indirections for
// every static-local thread_local in an inline function, costing one
// dependent indirect call per TLS access on the hot path.
extern __attribute__((tls_model("initial-exec")))
    thread_local ThreadCache* g_thread_cache;

extern __attribute__((tls_model("initial-exec")))
    thread_local int g_full_mode_cached;  // -1 = unknown, 0 = bypass, 1 = full

[[gnu::always_inline]]
inline ThreadCache*& currentThreadCache() { return g_thread_cache; }

// One-shot cache of "full mode" (= not compress-only, not large-only, no
// eager-zero).  If any of those env-var modes is on, the malloc fast path
// has to bypass to the slow path; if none, the fast path is safe.
[[gnu::always_inline]]
inline bool fullMallocPath() {
    int s = g_full_mode_cached;
    if (s < 0) [[unlikely]] {
        s = (!isCompressOnlyMode()
          && !isLargeOnlyMode()
          && !isEagerZeroMode()) ? 1 : 0;
        g_full_mode_cached = s;
    }
    return s == 1;
}

// ── System malloc/free pointers for compress-only mode ──────────────────────
// Resolved early before any malloc interposition is active.
using MallocFn = void*(*)(size_t);
using FreeFn = void(*)(void*);
using CallocFn = void*(*)(size_t, size_t);
using ReallocFn = void*(*)(void*, size_t);
using MemalignFn = int(*)(void**, size_t, size_t);
using MallocSizeFn = size_t(*)(const void*);

struct SystemAllocFns {
    MallocFn malloc = nullptr;
    FreeFn free = nullptr;
    CallocFn calloc = nullptr;
    ReallocFn realloc = nullptr;
    MemalignFn posix_memalign = nullptr;
    MallocSizeFn malloc_size = nullptr;

    // Find the pre-interposition original of a function by scanning the
    // __DATA,__interpose section.  Each entry is {replacement, original}.
    // Returns nullptr if not found.
#if defined(__APPLE__)
    static void* findOriginal(void* replacement) {
        // The interpose section may be in __DATA or __AUTH_CONST (ARM64e).
        static const char* segments[] = {"__AUTH_CONST", "__DATA"};
        uint32_t n = _dyld_image_count();
        for (uint32_t i = 0; i < n; i++) {
            auto* hdr = reinterpret_cast<const struct mach_header_64*>(
                _dyld_get_image_header(i));
            if (!hdr) continue;
            for (auto* seg : segments) {
                unsigned long sz = 0;
                auto* data = getsectiondata(hdr, seg, "__interpose", &sz);
                if (!data || sz == 0) continue;
                size_t count = sz / (2 * sizeof(void*));
                auto* entries = reinterpret_cast<void* const*>(data);
                for (size_t j = 0; j < count; j++) {
                    if (entries[j * 2] == replacement)
                        return entries[j * 2 + 1];
                }
            }
        }
        return nullptr;
    }
#endif

    void resolve() {
#if defined(__APPLE__)
        // On macOS, dlsym returns the interposed wrapper, not the real system
        // function.  Read the original from the __DATA,__interpose section.
        void* h = dlopen("/usr/lib/system/libsystem_malloc.dylib", RTLD_NOLOAD);
        if (h) {
            // Get the current (interposed) function addresses, then find
            // their pre-interposition originals.
            void* cur_malloc = dlsym(h, "malloc");
            void* cur_free = dlsym(h, "free");
            void* cur_calloc = dlsym(h, "calloc");
            void* cur_realloc = dlsym(h, "realloc");
            void* cur_memalign = dlsym(h, "posix_memalign");
            void* cur_msize = dlsym(h, "malloc_size");

            malloc = reinterpret_cast<MallocFn>(findOriginal(cur_malloc));
            free = reinterpret_cast<FreeFn>(findOriginal(cur_free));
            calloc = reinterpret_cast<CallocFn>(findOriginal(cur_calloc));
            realloc = reinterpret_cast<ReallocFn>(findOriginal(cur_realloc));
            posix_memalign = reinterpret_cast<MemalignFn>(findOriginal(cur_memalign));
            malloc_size = reinterpret_cast<MallocSizeFn>(findOriginal(cur_msize));
        }
#elif defined(__linux__)
        malloc = reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
        free = reinterpret_cast<FreeFn>(dlsym(RTLD_NEXT, "free"));
        calloc = reinterpret_cast<CallocFn>(dlsym(RTLD_NEXT, "calloc"));
        realloc = reinterpret_cast<ReallocFn>(dlsym(RTLD_NEXT, "realloc"));
        posix_memalign = reinterpret_cast<MemalignFn>(dlsym(RTLD_NEXT, "posix_memalign"));
        malloc_size = reinterpret_cast<MallocSizeFn>(dlsym(RTLD_NEXT, "malloc_usable_size"));
#endif
    }
};

extern SystemAllocFns g_system_alloc;

class SmashHeap {
    // Slab array sized to kTotalArenas (= kNumArenas when A3 off, 2*kNumArenas
    // when SMASH_COLD_ARENA_FEEDBACK is on).  Layout:
    //   arenas [0, kNumArenas)          — hot sub-arenas (default routing)
    //   arenas [kNumArenas, 2*kNumArenas) — cold sub-arenas (underfilled)
    Slab slabs_[kTotalArenas * kNumClasses];

    // A3 feedback state (unused/untouched when kColdArenaFeedback is false).
    // One entry per (base_arena, size_class).  Compressor bumps the counter
    // after every successful page compression that originated from the
    // corresponding slab; once it crosses kColdArenaThreshold, the sticky
    // bias flag flips to 1 and callsiteArena() routes subsequent
    // allocations to the cold sub-arena for that (arena, sc).
    std::atomic<uint32_t> compress_count_[kNumArenas * kNumClasses]{};
    std::atomic<uint8_t> cold_bias_[kNumArenas * kNumClasses]{};

    // C1b (adaptive cap) feedback.  Per (base_arena, size_class) counter
    // of re-warm events — a page that was COMPRESSED but got faulted back
    // to ACTIVE.  q̂ = decomp / (comp + decomp) is the online Pareto
    // estimator; cached cap values live in adaptive_cap_ so the slab
    // allocateNewSpan path can read them without recomputing every call.
    std::atomic<uint32_t> decompress_count_[kNumArenas * kNumClasses]{};
    std::atomic<uint32_t> adaptive_cap_[kNumArenas * kNumClasses]{};

    // Cohort measurement arrays (kMeasureCohorts only).  Per-page tracking
    // of first allocating thread ID and RA hash; a "mixed" flag per axis
    // flips when a second distinct value is seen.  Bootstrap-allocated once
    // compression is initialized.
    struct CohortPage {
        uint32_t first_tid;
        uint32_t first_ra;
        uint8_t  mixed_tid;
        uint8_t  mixed_ra;
    };
    CohortPage* cohort_pages_ = nullptr;
    size_t cohort_pages_len_ = 0;

    LargeAlloc large_alloc_;
    PageMap page_map_;

    Slab& slab(uint8_t arena, uint8_t sc) { return slabs_[arena * kNumClasses + sc]; }

    uint8_t callsiteArena(uint8_t sc) {
#ifdef SMASH_ABLATION_NO_CALLSITE_ARENA
        uint8_t base = 0;
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
        if constexpr (kThreadArenaHash) {
            // Thread-local monotonic id (A2-lite).  Multiplied by a
            // large odd constant so the low bits mix well into h.
            static std::atomic<uint32_t> next_tid{0};
            thread_local uint32_t tid = next_tid.fetch_add(1, std::memory_order_relaxed);
            h ^= static_cast<uintptr_t>(tid) * 0x9E3779B97F4A7C15ULL;
        }
        h ^= h >> 16;
        uint8_t base = static_cast<uint8_t>(h & (kNumArenas - 1));
#endif
        if constexpr (kColdArenaFeedback) {
            // If compressor has flagged this (arena, sc) as cold-biased,
            // route to the cold sub-arena.
            if (cold_bias_[base * kNumClasses + sc].load(std::memory_order_relaxed))
                return static_cast<uint8_t>(base + kNumArenas);
        }
        return base;
    }

public:
    // Called by the compressor after a page transitions to COMPRESSED.
    // Updates the (base_arena, size_class) compression counter; once the
    // threshold is crossed, flips the sticky cold-bias flag so subsequent
    // allocations route to the cold sub-arena.
    void onPageCompressed(uint8_t arena_id, uint8_t sc) {
        if constexpr (!kColdArenaFeedback && !kAdaptiveCap) return;
        if (sc >= kNumClasses) return;
        uint8_t base = arena_id & (kNumArenas - 1);   // strip cold half
        size_t idx = base * kNumClasses + sc;
        if constexpr (kColdArenaFeedback) {
            uint32_t prev = compress_count_[idx].fetch_add(1, std::memory_order_relaxed);
            if (prev + 1 == kColdArenaThreshold) {
                cold_bias_[idx].store(1, std::memory_order_relaxed);
            }
        } else {
            compress_count_[idx].fetch_add(1, std::memory_order_relaxed);
        }
        if constexpr (kAdaptiveCap) {
            recomputeAdaptiveCap(idx);
        }
    }

    static void compressedEventHook(size_t /*page_idx*/, uint8_t arena_id,
                                    uint8_t sc, void* ctx) {
        static_cast<SmashHeap*>(ctx)->onPageCompressed(arena_id, sc);
    }

    // Called by the compressor when a COMPRESSED page faults back to
    // ACTIVE.  Updates the decompression counter and recomputes the
    // cached adaptive cap for this (base_arena, sc).  The cap is the
    // smallest N with (1 - q̂)^N >= P_target, floored at kAdaptiveCapMin.
    // Hot buckets (q̂ >= q_max) disable the cap outright — under-packing
    // a hot bucket costs RSS without winning compressibility.
    void onPageDecompressed(uint8_t arena_id, uint8_t sc) {
        if constexpr (!kAdaptiveCap) return;
        if (sc >= kNumClasses) return;
        uint8_t base = arena_id & (kNumArenas - 1);
        size_t idx = base * kNumClasses + sc;
        decompress_count_[idx].fetch_add(1, std::memory_order_relaxed);
        recomputeAdaptiveCap(idx);
    }

    static void decompressedEventHook(size_t /*page_idx*/, uint8_t arena_id,
                                      uint8_t sc, void* ctx) {
        static_cast<SmashHeap*>(ctx)->onPageDecompressed(arena_id, sc);
    }

    // Recompute and cache the adaptive cap for bucket idx.  Called whenever
    // compress or decompress counters change.  Cheap (one log() call) and
    // races are benign — worst case we publish a slightly stale cap.
    void recomputeAdaptiveCap(size_t idx) {
        uint32_t comp = compress_count_[idx].load(std::memory_order_relaxed);
        uint32_t dec  = decompress_count_[idx].load(std::memory_order_relaxed);
        uint32_t total = comp + dec;
        uint32_t cap = 0;

        // Gate 1 — total-evidence floor.  Early in a bucket's life we know
        // nothing; cap=0 keeps the allocator in its default high-density path.
        if (total < kAdaptiveCapMinSamples) {
            adaptive_cap_[idx].store(0, std::memory_order_relaxed);
            return;
        }

        // Gate 2 — bilateral-evidence gate.  A stream of compressions with
        // zero re-warms looks like a "truly cold" bucket under the Pareto
        // model, but observationally is indistinguishable from "bucket that
        // hasn't had time to re-warm yet".  Workloads like duckdb and redis
        // fill-then-idle fall in the second camp; capping them before any
        // decomp event arrives bakes tiny caps into every span and
        // catastrophically regresses RSS once queries actually touch the
        // data.  Require at least one observed re-warm before applying a
        // cap.  If a bucket is genuinely cold forever, the default path
        // already wins (pages compress, stay compressed) — no under-packing
        // is needed.
        if (dec == 0) {
            adaptive_cap_[idx].store(0, std::memory_order_relaxed);
            return;
        }

        // Gate 3 — hot-bucket rejection.  Under-packing a hot bucket just
        // balloons RSS.  Laplace smoothing α=1 keeps q̂ bounded away from 0
        // for thin samples.
        constexpr double q_max = 0.30;
        double q = double(dec + 1) / double(total + 2);
        if (q >= q_max) {
            adaptive_cap_[idx].store(0, std::memory_order_relaxed);
            return;
        }

        double p = double(kAdaptiveCapTargetPct) / 100.0;
        double n = std::log(p) / std::log(1.0 - q);
        uint32_t cap_u = static_cast<uint32_t>(n);
        cap = cap_u < kAdaptiveCapMin ? kAdaptiveCapMin : cap_u;
        adaptive_cap_[idx].store(cap, std::memory_order_relaxed);
    }

    // Slab-facing cap lookup.  Returns the current cached cap for the
    // (arena, sc) bucket; 0 = no cap.  Called from Slab::allocateNewSpan
    // and Slab::maybeWiden.
    //
    // Clamps the cap against an "underfill floor" equal to
    // natural_slots_per_page / kAdaptiveCapUnderfillFactor — a cap that
    // would leave fewer than (natural / factor) slots per page is
    // rejected.  Without this clamp, a cap of 8 applied to a 64B size
    // class (natural density = 256 slots/page on a 16K page) forces 32×
    // VM underfill.  Compression reclaims zero-filled waste, but for
    // throughput-heavy workloads (duckdb, redis fill) allocations
    // outpace the compressor and RSS balloons.  The floor keeps the
    // underfill factor bounded so the Pareto-motivated cap only fires
    // where the cost is tolerable.
    static uint32_t adaptiveCapQuery(void* ctx, uint8_t arena, uint8_t sc) {
        auto* self = static_cast<SmashHeap*>(ctx);
        if (sc >= kNumClasses) return 0;
        uint8_t base = arena & (kNumArenas - 1);
        uint32_t raw = self->adaptive_cap_[base * kNumClasses + sc]
                           .load(std::memory_order_relaxed);
        if (raw == 0) return 0;
        uint32_t osz = kSizeClasses[sc].size;
        if (osz == 0 || osz >= kPageSize) return raw;
        uint32_t natural = static_cast<uint32_t>(kPageSize / osz);
        constexpr uint32_t kUnderfillFactor = 4;
        uint32_t floor_cap = natural / kUnderfillFactor;
        if (floor_cap < kAdaptiveCapMin) floor_cap = kAdaptiveCapMin;
        return raw < floor_cap ? floor_cap : raw;
    }

private:

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

public:
    // Called from a pthread_atfork child handler. After fork, the child has
    // inherited compression_started_=true and the parent's now-defunct
    // pthread_t handles, so the next allocation's `if (!started) startCompression()`
    // check short-circuits and the child runs with no compressor — hence the
    // committed=N / compressed=0 we see in postgres backends, Redis daemonized
    // children, etc.
    //
    // Atfork handler trio. The compressor runs on background threads that
    // disappear at fork(); without this the child inherits dead pthread_t
    // handles plus `compression_started_=true`, so its first malloc's check
    // short-circuits and the child runs with no compressor at all (postgres
    // backends, daemonized redis children, etc. all hit this).
    //
    // prepare: ask the coordinator to skip its next tick(); wait briefly
    //   for any in-flight tick to drain. Without this, a fork that lands
    //   mid-tick leaves the child holding page-locks (etc.) acquired by a
    //   thread that no longer exists.
    // parent:  resume normal ticks.
    // child:   reset thread bookkeeping and immediately respawn the
    //   compressor. We're in a fresh single-thread context, so
    //   pthread_create is fine here. Inherited PageState is left alone:
    //   COMPRESSED pages stay decompressible-on-fault because the
    //   compressed bytes live in BootstrapAlloc memory mapped CoW.
    void preparePauseForFork() {
        if (!compression_inited_) return;
        compressor_.pauseForFork();
    }
    void resumeAfterFork() {
        if (!compression_inited_) return;
        compressor_.resumeAfterFork();
    }
    void resetForFork() {
        if (!compression_inited_) return;
        compressor_.resetForFork();
        compression_started_.store(false, std::memory_order_release);
        startCompression();
    }
private:

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
            // Wire the radix tree to also publish span pointers into the
            // VmRegion's flat page→Span table. Must happen before any slab
            // initialization so the first allocation's setRange() mirrors.
            if (!compress_only) {
                page_map_.attachVmRegion(&vm_region_);
            }
            page_states_.init(vm_region_.totalPages());
            page_locks_.init(vm_region_.totalPages());
            compress_store_.init();
            compress_engine_.init();
            compressor_.init(&vm_region_, &page_states_, &page_locks_,
                             &compress_store_, &compress_engine_,
                             compress_only ? nullptr : &page_map_, &fault_handler_);
            // A3: register the cold-arena feedback hook (no-op when
            // SMASH_COLD_ARENA_FEEDBACK is off — onPageCompressed early-returns).
            if (!compress_only && (kColdArenaFeedback || kAdaptiveCap)) {
                compressor_.setCompressedCallback(compressedEventHook, this);
            }
            if (!compress_only && kAdaptiveCap) {
                compressor_.setDecompressedCallback(decompressedEventHook, this);
            }

            if (!compress_only) {
                for (int a = 0; a < kTotalArenas; ++a) {
                    // Hot sub-arenas (a < kNumArenas): no per-page cap.
                    // Cold sub-arenas (a >= kNumArenas, only when A3 on):
                    // apply kMaxSlotsPerPage to produce sparse pages.
                    // If A3 is off but kMaxSlotsPerPage > 0, the cap
                    // applies globally (ablation mode — measure C1 in
                    // isolation without the feedback loop).
                    uint32_t cap = 0;
                    if constexpr (kColdArenaFeedback) {
                        if (a >= kNumArenas) cap = kMaxSlotsPerPage;
                    } else {
                        cap = kMaxSlotsPerPage;
                    }
                    for (int i = 0; i < kNumClasses; ++i) {
                        slabs_[a * kNumClasses + i].init(
                            static_cast<uint8_t>(i), &page_map_,
                            &vm_region_, &page_states_,
                            releaseHook, this,
                            static_cast<uint8_t>(a), cap,
                            compressor_.coldCounts());
                        if constexpr (kAdaptiveCap) {
                            slabs_[a * kNumClasses + i].setCapFn(
                                adaptiveCapQuery, this);
                        }
                    }
                }
            }
            compression_inited_ = true;
            g_smash_vm_region = &vm_region_;
            g_smash_page_states_for_external = &page_states_;

            if constexpr (kMeasureCohorts) {
                cohort_pages_len_ = vm_region_.totalPages();
                cohort_pages_ = bootstrapArray<CohortPage>(cohort_pages_len_);
                __builtin_memset(cohort_pages_, 0,
                                 cohort_pages_len_ * sizeof(CohortPage));
                compressor_.setCohortData(cohort_pages_, cohort_pages_len_);
            }
        } else if (!compress_only) {
            // Fallback: Phase 1 mode (no compression).  No feedback loop
            // possible here, so still respect kUnderfillDenom for ablation.
            for (int a = 0; a < kTotalArenas; ++a) {
                uint32_t cap = 0;
                if constexpr (kColdArenaFeedback) {
                    if (a >= kNumArenas) cap = kMaxSlotsPerPage;
                } else {
                    cap = kMaxSlotsPerPage;
                }
                for (int i = 0; i < kNumClasses; ++i)
                    slabs_[a * kNumClasses + i].init(
                        static_cast<uint8_t>(i), &page_map_,
                        nullptr, nullptr, nullptr, nullptr,
                        static_cast<uint8_t>(a), cap);
            }
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

    void stampCohort(void* ptr, uint32_t ra_hash) {
        if constexpr (!kMeasureCohorts) return;
        if (!cohort_pages_) return;
        uintptr_t pa = reinterpret_cast<uintptr_t>(ptr);
        if (!vm_region_.contains(pa)) return;
        size_t idx = vm_region_.pageIndex(pa);
        if (idx >= cohort_pages_len_) return;
        auto& cp = cohort_pages_[idx];
        static std::atomic<uint32_t> next_ctid{1};
        thread_local uint32_t ctid =
            next_ctid.fetch_add(1, std::memory_order_relaxed);
        if (cp.first_tid == 0) cp.first_tid = ctid;
        else if (cp.first_tid != ctid) cp.mixed_tid = 1;
        if (cp.first_ra == 0) cp.first_ra = ra_hash;
        else if (cp.first_ra != ra_hash) cp.mixed_ra = 1;
    }

    // Out-of-line cold path for everything malloc() can't handle on the
    // fast path: compress-only/large-only/eager-zero modes, size > 16 KiB
    // (large alloc), or thread cache miss (refill from slab).  Marked
    // noinline+cold so the fast-path body stays small and the prologue
    // can be a single register save pair.
    [[gnu::noinline, gnu::cold]]
    void* mallocSlow(size_t size) {
        if (isCompressOnlyMode()) {
            // During early init, g_system_alloc may not be resolved yet
            if (!g_system_alloc.malloc) return nullptr;
            void* ptr = g_system_alloc.malloc(size);
            trackAllocation(ptr, size);
            return ptr;
        }

        if (size == 0) size = 1;

        // Large-only mode: allocations <= largeOnlyThreshold() pass through
        // to system malloc. Default threshold is kMaxSmallSize (16 KB) —
        // overridable via SMASH_LARGE_ONLY_THRESHOLD env var.
        if (isLargeOnlyMode() && size <= largeOnlyThreshold()) {
            if (g_system_alloc.malloc) return g_system_alloc.malloc(size);
        }

        uint8_t sc = sizeToClass(size);
        if (sc < kNumClasses) {
            ThreadCache* tc = getOrCreateThreadCache();
            void* ptr = tc->allocate(sc);
            if (!ptr) ptr = tc->refill(sc, &slab(callsiteArena(sc), sc));
            if constexpr (kMeasureCohorts) {
                if (ptr) {
                    uintptr_t ra = reinterpret_cast<uintptr_t>(
                        __builtin_return_address(0));
                    uint32_t ra32 = static_cast<uint32_t>(ra ^ (ra >> 32));
                    stampCohort(ptr, ra32);
                }
            }
            if (ptr && isEagerZeroMode())
                __builtin_memset(ptr, 0, classSize(sc));
            return ptr;
        }
        void* ptr = large_alloc_.allocate(size, kMinAlignment);
        if (ptr && isEagerZeroMode()) __builtin_memset(ptr, 0, size);
        return ptr;
    }

    // Hot path.  size in [1, kMaxSmallSize] AND thread cache already
    // initialised AND non-empty for this size class → return the cached
    // pointer.  Anything else falls into mallocSlow.  Everything past the
    // first `[[likely]]` returns directly without a function call.
    [[gnu::always_inline]]
    void* malloc(size_t size) {
        if (fullMallocPath() && size > 0 && size <= kMaxSmallSize) [[likely]] {
            uint8_t sc = sizeToClass(size);
            if (ThreadCache* tc = currentThreadCache()) [[likely]] {
                if (void* ptr = tc->allocate(sc)) [[likely]] return ptr;
            }
        }
        return mallocSlow(size);
    }

    void free(void* ptr) {
        if (!ptr) return;
        if (BootstrapAlloc::instance().owns(ptr)) return;

        if (isCompressOnlyMode()) {
            if (g_system_alloc.free) g_system_alloc.free(ptr);
            return;
        }

        // Fast path: pointer inside the VmRegion's contiguous arena → single
        // load from the flat page→Span table. Avoids the two-level radix
        // walk (acquire-load chain) that page_map_.get() requires.
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        Span* span;
        if (vm_region_.inContigArena(addr) && vm_region_.hasSpanTable()) [[likely]] {
            span = vm_region_.getSpan(vm_region_.contigPageIndex(addr));
        } else {
            span = page_map_.get(addr);
        }
        if (!span) {
            // Not a Smash-managed pointer. In large-only mode, small
            // allocations were forwarded to system malloc.
            if (isLargeOnlyMode() && g_system_alloc.free)
                g_system_alloc.free(ptr);
            return;
        }
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
        // Large-only: small aligned allocs go to system allocator
        if (isLargeOnlyMode() && size <= largeOnlyThreshold() && g_system_alloc.posix_memalign) {
            void* ptr = nullptr;
            if (g_system_alloc.posix_memalign(&ptr, alignment, size) == 0)
                return ptr;
            return nullptr;
        }
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
        size_t total = count * size;
        // Large-only: small callocs go to system allocator
        if (isLargeOnlyMode() && total <= largeOnlyThreshold() && g_system_alloc.calloc) {
            return g_system_alloc.calloc(count, size);
        }
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

        // Same flat-table fast path as free(): single load when the pointer
        // is inside the contiguous VmRegion arena, falling back to the
        // radix tree for non-VmRegion pointers (e.g. large-only mode's
        // system-malloc passthroughs).
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        Span* span;
        if (vm_region_.inContigArena(addr) && vm_region_.hasSpanTable()) [[likely]] {
            span = vm_region_.getSpan(vm_region_.contigPageIndex(addr));
        } else {
            span = page_map_.get(addr);
        }
        if (!span) {
            // In large-only mode, query system allocator for size.
            // alloc8's realloc uses this to size the memcpy.
            if (isLargeOnlyMode() && g_system_alloc.malloc_size)
                return g_system_alloc.malloc_size(ptr);
            return 0;
        }
        if (span->is_large) return span->large_size;
        return classSize(span->size_class);
    }

    void lock() {
        if (isCompressOnlyMode()) return;
        for (int i = 0; i < kTotalArenas * kNumClasses; ++i) slabs_[i].lockSlab();
        large_alloc_.lockAlloc();
    }
    void unlock() {
        if (isCompressOnlyMode()) return;
        large_alloc_.unlockAlloc();
        for (int i = kTotalArenas * kNumClasses - 1; i >= 0; --i) slabs_[i].unlockSlab();
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

    CohortPage* cohortPages() const { return cohort_pages_; }
    size_t cohortPagesLen() const { return cohort_pages_len_; }
};
} // namespace smash
