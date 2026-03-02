// smash/src/core/page_map.h - Two-level radix tree: virtual address → Span*
//
// Provides O(1) lookup from any address to the Span that manages it.
// L1 table allocated eagerly from bootstrap; L2 tables allocated lazily.
#pragma once

#include "smash/config.h"
#include "bootstrap_alloc.h"
#include "span.h"
#include "../util/spinlock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace smash {

class PageMap {
    using L2Entry = std::atomic<Span*>;
    using L2Table = L2Entry*;

    std::atomic<L2Table>* level1_;   // kPageMapL1Size entries, from bootstrap
    Spinlock l2_lock_;

    L2Table ensureL2(size_t l1_idx) {
        L2Table tbl = level1_[l1_idx].load(std::memory_order_acquire);
        if (tbl) [[likely]] return tbl;
        LockGuard guard(l2_lock_);
        tbl = level1_[l1_idx].load(std::memory_order_relaxed);
        if (tbl) return tbl;
        tbl = static_cast<L2Table>(
            BootstrapAlloc::instance().allocateZeroed(
                kPageMapL2Size * sizeof(L2Entry), alignof(L2Entry)));
        level1_[l1_idx].store(tbl, std::memory_order_release);
        return tbl;
    }

public:
    void init() {
        level1_ = static_cast<std::atomic<L2Table>*>(
            BootstrapAlloc::instance().allocateZeroed(
                kPageMapL1Size * sizeof(std::atomic<L2Table>),
                alignof(std::atomic<L2Table>)));
    }

    void set(uintptr_t addr, Span* span) {
        size_t page_idx = addr >> kPageShift;
        size_t l1 = page_idx >> kPageMapL2Bits;
        size_t l2 = page_idx & (kPageMapL2Size - 1);
        L2Table tbl = ensureL2(l1);
        tbl[l2].store(span, std::memory_order_release);
    }

    Span* get(uintptr_t addr) const {
        size_t page_idx = addr >> kPageShift;
        size_t l1 = page_idx >> kPageMapL2Bits;
        size_t l2 = page_idx & (kPageMapL2Size - 1);
        L2Table tbl = level1_[l1].load(std::memory_order_acquire);
        if (!tbl) [[unlikely]] return nullptr;
        return tbl[l2].load(std::memory_order_acquire);
    }

    // Set all pages in a range to the same span
    void setRange(uintptr_t addr, size_t num_pages, Span* span) {
        for (size_t i = 0; i < num_pages; ++i) {
            set(addr + i * kPageSize, span);
        }
    }

    // Clear all pages in a range
    void clearRange(uintptr_t addr, size_t num_pages) {
        setRange(addr, num_pages, nullptr);
    }
};

} // namespace smash
