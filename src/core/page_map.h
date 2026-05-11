// smash/src/core/page_map.h - Two-level radix tree: virtual address → Span*
//
// Provides O(1) lookup from any address to the Span that manages it.
// L1 table allocated eagerly from bootstrap; L2 tables allocated lazily.
//
// Optionally mirrors writes into a VmRegion's flat page-index → Span* table
// when the address is inside the VmRegion. The flat table gives the fast
// path a single-load lookup; the radix tree remains the authoritative source
// for addresses that escape the VmRegion (large allocs that bypass it).
#pragma once

#include "smash/config.h"
#include "bootstrap_alloc.h"
#include "span.h"
#include "../vm/vm_region.h"
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
    // Optional VmRegion whose flat page→Span table mirrors radix writes.
    // Set via attachVmRegion(); nullptr falls back to radix-only behavior.
    VmRegion* vm_region_ = nullptr;

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

    // If the address falls inside the attached VmRegion, mirror the write
    // into its flat table. Always also update the radix tree so external
    // (non-VmRegion) callers continue to resolve via PageMap::get().
    [[gnu::always_inline]]
    void mirrorFlat(uintptr_t addr, Span* span) {
        if (!vm_region_) return;
        if (!vm_region_->contains(addr)) return;
        size_t idx = vm_region_->pageIndex(addr);
        // pageIndex() returns 0 for "not present in tracking mode"; in full
        // mode index 0 is a legitimate contig-arena page, but contains()
        // gates entry — so a 0 here is fine because we only land here when
        // the address is inside the region.
        vm_region_->setSpan(idx, span);
    }

public:
    void init() {
        level1_ = static_cast<std::atomic<L2Table>*>(
            BootstrapAlloc::instance().allocateZeroed(
                kPageMapL1Size * sizeof(std::atomic<L2Table>),
                alignof(std::atomic<L2Table>)));
    }

    // Attach a VmRegion so subsequent set/setRange/clearRange operations
    // also update its flat page→Span table. Must be called before any
    // allocation activity routes through this PageMap if the flat table
    // is to be consistent with the radix tree.
    void attachVmRegion(VmRegion* vr) { vm_region_ = vr; }

    void set(uintptr_t addr, Span* span) {
        size_t page_idx = addr >> kPageShift;
        size_t l1 = page_idx >> kPageMapL2Bits;
        size_t l2 = page_idx & (kPageMapL2Size - 1);
        L2Table tbl = ensureL2(l1);
        tbl[l2].store(span, std::memory_order_release);
        mirrorFlat(addr, span);
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
