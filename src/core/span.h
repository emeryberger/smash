// smash/src/core/span.h - Span descriptor with bitmap-based free tracking
//
// Key invariant: span metadata (this struct + bitmap) lives in bootstrap memory,
// NEVER mixed with user data pages. This keeps data pages pure for compression.
#pragma once

#include "smash/config.h"
#include "bootstrap_alloc.h"
#include "size_classes.h"
#include "../util/bitops.h"
#include "../vm/page_state.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace smash {

struct Span {
    void* base;                 // start of data pages
    uint64_t* bitmap;           // 1 = free, 0 = allocated; from BootstrapAlloc
    uint32_t page_count;
    uint32_t object_size;
    // Slots that *could* exist in the span's pages (pages*kPageSize/object_size).
    // Bitmap is always sized for this.  Under a per-page cap, most of these bits
    // are held at 0 and never handed out.  allocate() must scan the full bitmap
    // — not just (object_count+63)/64 words — because the live bits are spread
    // across every page, not packed at the front.
    uint32_t full_capacity;
    uint16_t object_count;      // currently usable slots (grows with widenCap)
    uint16_t allocated_count;
    uint8_t size_class;
    bool is_large;              // true for mmap-backed large allocations
    uint8_t arena_id;           // which arena this span belongs to
    uint16_t free_hint_word;    // word index to start bitmap scan from
    // Per-page cap currently encoded in the bitmap.  0 = no cap (dense).
    // Slab::widenCapIfNeeded() compares this to the current adaptive cap and
    // unlocks additional slots when the cap has relaxed since span init.
    uint16_t current_cap_per_page;

    // Intrusive list pointers (used by Slab's partial/full/empty lists)
    Span* list_prev;
    Span* list_next;

    // For large allocations: actual requested size
    size_t large_size;

    // For thread cache pool recycling
    Span* next_free;

    // Optional: page-state lookup so allocate() can skip slots whose page
    // is currently COMPRESSED. Set by Slab::allocateNewSpan when a
    // VmRegion is in use; null otherwise (large allocs, no-vm fallback).
    // The slab pre-computes first_page_vm_idx (= vm_region_->pageIndex(base))
    // so the inner allocate loop doesn't have to call back into VmRegion.
    PageStateTable* page_states;
    uint32_t        first_page_vm_idx;
    // Optional: per-page cold-tick counter array owned by CompressorThread.
    // allocate() resets the counter for the page that just received a chunk,
    // so phase2 doesn't compress pages that are still being filled.
    uint8_t*        cold_counts;

    // Initialize a slab span for the given size class.
    //
    // When max_slots_per_page > 0 and object_size < kPageSize, only the first
    // `cap` slots that start on each page are made available, where
    // cap = min(max_slots_per_page, slots-starting-on-that-page).  This
    // bounds per-page live count uniformly across size classes, which is
    // what the (1-q)^N page-cold probability argument actually requires.
    // Disallowed slots stay zero-filled and compress to near-zero.
    //
    // The bitmap remains sized for the full capacity (1 bit per potential
    // slot); disallowed slots are simply marked allocated (0) so the
    // allocator skips them.
    void init(void* base_, uint32_t pages, uint8_t sc, uint8_t arena = 0,
              uint32_t max_slots_per_page = 0) {
        base = base_;
        page_count = pages;
        size_class = sc;
        is_large = false;
        arena_id = arena;
        object_size = kSizeClasses[sc].size;
        full_capacity = (pages * kPageSize) / object_size;

        allocated_count = 0;
        free_hint_word = 0;
        list_prev = nullptr;
        list_next = nullptr;
        large_size = 0;
        next_free = nullptr;
        page_states = nullptr;
        first_page_vm_idx = 0;
        cold_counts = nullptr;

        size_t num_words = (full_capacity + 63) / 64;
        bitmap = static_cast<uint64_t*>(
            BootstrapAlloc::instance().allocate(num_words * sizeof(uint64_t), alignof(uint64_t)));

        const bool cap_active = max_slots_per_page > 0 && object_size < kPageSize;

        if (!cap_active) {
            object_count = static_cast<uint16_t>(full_capacity);
            current_cap_per_page = 0;
            if (num_words > 0) {
                __builtin_memset(bitmap, 0xFF, num_words * sizeof(uint64_t));
                size_t valid_bits = full_capacity % 64;
                if (valid_bits != 0) {
                    bitmap[num_words - 1] = (1ULL << valid_bits) - 1;
                }
            }
            return;
        }

        // Per-page cap path: build sparse bitmap.
        if (num_words > 0) __builtin_memset(bitmap, 0, num_words * sizeof(uint64_t));
        current_cap_per_page = static_cast<uint16_t>(max_slots_per_page);
        uint32_t live = 0;
        for (uint32_t p = 0; p < pages; ++p) {
            uint64_t first = (static_cast<uint64_t>(p) * kPageSize + object_size - 1) / object_size;
            uint64_t next  = (static_cast<uint64_t>(p + 1) * kPageSize + object_size - 1) / object_size;
            if (next > full_capacity) next = full_capacity;
            uint64_t avail_on_page = (next > first) ? (next - first) : 0;
            uint64_t use = avail_on_page < max_slots_per_page ? avail_on_page : max_slots_per_page;
            for (uint64_t i = 0; i < use; ++i) {
                uint64_t slot = first + i;
                bitmap[slot / 64] |= (1ULL << (slot % 64));
                ++live;
            }
        }
        if (live == 0) {
            bitmap[0] |= 1ULL;
            live = 1;
        }
        object_count = static_cast<uint16_t>(live);
    }

    // Widen (or remove) the per-page cap on an already-initialized span.
    // Unlocks additional slots on each page for future allocations.  Existing
    // live/free bookkeeping for the currently-usable slots is preserved.
    // Caller must hold the slab lock.  new_cap_per_page == 0 removes the cap
    // entirely (dense bitmap).  Safe to call with new_cap <= current_cap
    // (no-op).
    void widenCap(uint32_t new_cap_per_page) {
        if (is_large || object_size == 0 || object_size >= kPageSize) return;
        const uint32_t cur = current_cap_per_page;
        // 0 means "no cap / dense" — cannot widen further.
        if (cur == 0) return;
        const uint32_t pages = page_count;
        const uint32_t osz = object_size;
        uint32_t added = 0;
        if (new_cap_per_page == 0) {
            // Dense: flip every remaining bit on.  Counts slots beyond cur.
            for (uint32_t p = 0; p < pages; ++p) {
                uint64_t first = (static_cast<uint64_t>(p) * kPageSize + osz - 1) / osz;
                uint64_t next  = (static_cast<uint64_t>(p + 1) * kPageSize + osz - 1) / osz;
                if (next > full_capacity) next = full_capacity;
                uint64_t avail = (next > first) ? (next - first) : 0;
                uint64_t start_offset = (avail < cur) ? avail : cur;
                for (uint64_t i = start_offset; i < avail; ++i) {
                    uint64_t slot = first + i;
                    size_t w = slot / 64, b = slot % 64;
                    if (!(bitmap[w] & (1ULL << b))) {
                        bitmap[w] |= (1ULL << b);
                        ++added;
                    }
                }
            }
            current_cap_per_page = 0;
        } else {
            if (new_cap_per_page <= cur) return;
            for (uint32_t p = 0; p < pages; ++p) {
                uint64_t first = (static_cast<uint64_t>(p) * kPageSize + osz - 1) / osz;
                uint64_t next  = (static_cast<uint64_t>(p + 1) * kPageSize + osz - 1) / osz;
                if (next > full_capacity) next = full_capacity;
                uint64_t avail = (next > first) ? (next - first) : 0;
                uint64_t old_end = (avail < cur) ? avail : cur;
                uint64_t new_end = (avail < new_cap_per_page) ? avail : new_cap_per_page;
                for (uint64_t i = old_end; i < new_end; ++i) {
                    uint64_t slot = first + i;
                    size_t w = slot / 64, b = slot % 64;
                    if (!(bitmap[w] & (1ULL << b))) {
                        bitmap[w] |= (1ULL << b);
                        ++added;
                    }
                }
            }
            current_cap_per_page = static_cast<uint16_t>(new_cap_per_page);
        }
        if (added) {
            uint32_t nc = static_cast<uint32_t>(object_count) + added;
            if (nc > full_capacity) nc = full_capacity;
            if (nc > 0xFFFFu) nc = 0xFFFFu;
            object_count = static_cast<uint16_t>(nc);
            free_hint_word = 0;
        }
    }

    // Initialize a large allocation span
    void initLarge(void* base_, size_t size, uint32_t pages) {
        base = base_;
        page_count = pages;
        size_class = kNumClasses;
        is_large = true;
        arena_id = 0;
        object_size = 0;
        full_capacity = 0;
        current_cap_per_page = 0;
        object_count = 0;
        allocated_count = 1;
        free_hint_word = 0;
        bitmap = nullptr;
        list_prev = nullptr;
        list_next = nullptr;
        large_size = size;
        next_free = nullptr;
        page_states = nullptr;
        first_page_vm_idx = 0;
        cold_counts = nullptr;
    }

    // Allocate one object. Returns nullptr if span is full.
    //
    // Scans over `full_capacity` words, not `object_count`: under a per-page
    // cap the live bits are spread across every page (bit positions 0..cap-1
    // on each page), so a truncated scan would miss every live slot beyond
    // the first page and the span would appear "full" at ~1/page_count
    // utilization.
    // Check whether the page containing `slot_byte_offset` (offset from base)
    // is currently COMPRESSED. If so, allocating from it would force a
    // fault on the user's first access. The bitmap walk in allocate() uses
    // this to prefer slots on ACTIVE pages.  Inline-friendly; reads one
    // atomic byte from PageStateTable per check.
    inline bool slotPageCompressed(size_t slot_byte_offset) const {
        if (!page_states) return false;
        size_t page_off = slot_byte_offset / kPageSize;
        size_t page_idx = static_cast<size_t>(first_page_vm_idx) + page_off;
        return page_states->get(page_idx) == PageState::COMPRESSED;
    }

    // Reset the cold-tick counter for the page containing the slot we just
    // handed out, so phase2 won't see this page as "idle" while it's still
    // receiving fresh allocations.  Plain (non-atomic) write — cold_count_
    // is a uint8_t snapshot read by phase1/phase2 with no ordering needs.
    inline void resetColdCountForSlot(size_t slot_byte_offset) {
        if (!cold_counts) return;
        size_t page_off = slot_byte_offset / kPageSize;
        size_t page_idx = static_cast<size_t>(first_page_vm_idx) + page_off;
        cold_counts[page_idx] = 0;
    }

    // Walk one bitmap word, picking the first free slot whose page is NOT
    // COMPRESSED. Returns the chosen bit (0..63), or -1 if every free slot
    // in this word is on a compressed page.  As a side effect, records the
    // first compressed-page free slot in *fallback_bit (only if the caller
    // hasn't already recorded one elsewhere).
    inline int pickActiveBit(size_t word_idx, uint64_t word,
                             int* fallback_word, int* fallback_bit) const {
        while (word) {
            int bit = ctz(word);
            size_t slot = word_idx * 64 + bit;
            size_t off  = slot * object_size;
            if (!slotPageCompressed(off)) return bit;
            if (*fallback_word < 0) {
                *fallback_word = static_cast<int>(word_idx);
                *fallback_bit  = bit;
            }
            word &= word - 1;  // clear this bit, try next
        }
        return -1;
    }

    void* allocate() {
        size_t num_words = (full_capacity + 63) / 64;
        int fallback_word = -1, fallback_bit = 0;

        // Scan from hint position. Prefer slots on non-compressed pages so
        // the user's first access doesn't force a decompress fault.
        for (size_t i = free_hint_word; i < num_words; ++i) {
            uint64_t w = bitmap[i];
            if (!w) continue;
            int bit = pickActiveBit(i, w, &fallback_word, &fallback_bit);
            if (bit < 0) continue;
            size_t slot = i * 64 + bit;
            bitmap[i] &= ~(1ULL << bit);
            ++allocated_count;
            free_hint_word = static_cast<uint16_t>(i);
            size_t off = slot * object_size;
            resetColdCountForSlot(off);
            return static_cast<char*>(base) + off;
        }
        for (size_t i = 0; i < free_hint_word; ++i) {
            uint64_t w = bitmap[i];
            if (!w) continue;
            int bit = pickActiveBit(i, w, &fallback_word, &fallback_bit);
            if (bit < 0) continue;
            size_t slot = i * 64 + bit;
            bitmap[i] &= ~(1ULL << bit);
            ++allocated_count;
            free_hint_word = static_cast<uint16_t>(i);
            size_t off = slot * object_size;
            resetColdCountForSlot(off);
            return static_cast<char*>(base) + off;
        }

        // No active-page slot found. Fall back to the first compressed-page
        // slot we saw — the user's first access will fault and decompress.
        // Better than returning nullptr (which forces a new span).
        if (fallback_word >= 0) {
            size_t slot = static_cast<size_t>(fallback_word) * 64
                        + static_cast<size_t>(fallback_bit);
            bitmap[fallback_word] &= ~(1ULL << fallback_bit);
            ++allocated_count;
            free_hint_word = static_cast<uint16_t>(fallback_word);
            size_t off = slot * object_size;
            // Don't reset cold_counts here — the page is COMPRESSED.  The
            // user's first access will fault and decompress, and the fault
            // handler resets cold_count itself.
            return static_cast<char*>(base) + off;
        }
        return nullptr;  // full
    }

    // Free one object back to this span.
    void deallocate(void* ptr) {
        size_t offset = static_cast<size_t>(static_cast<char*>(ptr) - static_cast<char*>(base));
        size_t slot = offset / object_size;
        size_t word = slot / 64;
        size_t bit = slot % 64;
        bitmap[word] |= (1ULL << bit);   // mark free
        --allocated_count;
        if (word < free_hint_word) {
            free_hint_word = static_cast<uint16_t>(word);
        }
    }

    bool full() const { return allocated_count == object_count; }
    bool empty() const { return allocated_count == 0; }

    // Check if a pointer belongs to this span
    bool contains(const void* ptr) const {
        auto p = reinterpret_cast<uintptr_t>(ptr);
        auto b = reinterpret_cast<uintptr_t>(base);
        return p >= b && p < b + static_cast<size_t>(page_count) * kPageSize;
    }
};

// Allocate a Span descriptor from bootstrap memory
inline Span* newSpanDescriptor() {
    auto* s = static_cast<Span*>(
        BootstrapAlloc::instance().allocate(sizeof(Span), alignof(Span)));
    __builtin_memset(s, 0, sizeof(Span));
    return s;
}

} // namespace smash
