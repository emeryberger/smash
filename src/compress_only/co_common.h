// smash/src/compress_only/co_common.h - Shared state for compress-only variant
//
// Contains global compression state, page tracking, VM region scanning,
// and initialization. Included by both macOS and Linux interposition files.
#pragma once

#include "smash/config.h"
#include "../core/bootstrap_alloc.h"
#include "../vm/vm_region.h"
#include "../vm/page_state.h"
#include "../vm/platform_mem.h"
#include "../vm/fault_handler.h"
#include "../vm/syscall_compat.h"
#include "../compress/compress_store.h"
#include "../compress/compress_engine.h"
#include "../compress/compressor_thread.h"
#include "../util/spinlock.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <mach/mach.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Global VmRegion pointer (declared in smash_heap.h for the main library)
namespace smash { inline VmRegion* g_smash_vm_region = nullptr; }

// ── Global compression state ────────────────────────────────────────────────

namespace co {

inline smash::VmRegion g_vm;
inline smash::PageStateTable g_states;
inline smash::PageLockTable g_locks;
inline smash::CompressStore g_store;
inline smash::CompressEngine g_engine;
inline smash::CompressorThread g_compressor;
inline smash::vm::FaultHandler g_fault_handler;

inline std::atomic<bool> g_inited{false};
inline std::atomic<bool> g_compression_started{false};
inline std::atomic<int> g_warmup_count{0};
inline std::atomic<size_t> g_track_count{0};
inline std::atomic<size_t> g_malloc_count{0};
inline std::atomic<size_t> g_malloc_bytes{0};

inline bool faultCallback(uintptr_t fault_addr, void* /*ctx*/) {
    return g_compressor.handleFault(fault_addr);
}

// Forward declaration
void scanVmRegions();

inline void startCompression() {
    bool expected = false;
    if (!g_compression_started.compare_exchange_strong(expected, true))
        return;
    g_fault_handler.start(faultCallback, nullptr);
#ifndef __linux__
    // On macOS, scan VM regions to find heap pages
    scanVmRegions();
    g_compressor.setPreTickCallback(scanVmRegions);
#endif
    // On Linux, don't scan VM regions - it finds glibc internal pages
    // that crash when mprotected. Only track pages from malloc/mmap interposition.
    g_compressor.start();
}

inline void trackAllocation(void* ptr, size_t size) {
    if (!ptr || size == 0) return;
    uintptr_t start_page = reinterpret_cast<uintptr_t>(ptr) & ~(smash::kPageSize - 1);
    uintptr_t end_page = (reinterpret_cast<uintptr_t>(ptr) + size - 1) & ~(smash::kPageSize - 1);

    for (uintptr_t p = start_page; p <= end_page; p += smash::kPageSize) {
        size_t idx = g_vm.trackPage(p);
        if (idx > 0) {
            smash::PageState st = g_states.get(idx);
            if (st == smash::PageState::EMPTY)
                g_states.set(idx, smash::PageState::ACTIVE);
        }
    }
}

inline void trackMalloc(void* ptr, size_t size) {
    if (!ptr || !g_inited.load(std::memory_order_relaxed)) return;
    g_malloc_count.fetch_add(1, std::memory_order_relaxed);
    g_malloc_bytes.fetch_add(size, std::memory_order_relaxed);
    trackAllocation(ptr, size);
    if (!g_compression_started.load(std::memory_order_relaxed)) {
        if (g_warmup_count.fetch_add(1, std::memory_order_relaxed) >= 5000)
            startCompression();
    }
}

// ── Shared syscall helpers ──────────────────────────────────────────────────

inline void warmIovec(const struct iovec* iov, int iovcnt, smash::VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len)
            smash::vm::warmPages(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

// ── Initialization ──────────────────────────────────────────────────────────

inline void init() {
    if (!g_vm.init(smash::kVmRegionSize))
        return;

    g_states.init(g_vm.totalPages());
    g_locks.init(g_vm.totalPages());
    g_store.init();
    g_engine.init();

    g_compressor.init(&g_vm, &g_states, &g_locks, &g_store, &g_engine,
                      nullptr, &g_fault_handler);

    smash::g_smash_vm_region = &g_vm;

    g_inited.store(true, std::memory_order_release);
}

} // namespace co
