// smash/src/core/span.h - Span descriptor with bitmap-based free tracking
//
// Key invariant: span metadata (this struct + bitmap) lives in bootstrap memory,
// NEVER mixed with user data pages. This keeps data pages pure for compression.
#pragma once

#include "smash/config.h"
#include "bootstrap_alloc.h"
#include "size_classes.h"
#include "../util/bitops.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace smash {

struct Span {
    void* base;                 // start of data pages
    uint64_t* bitmap;           // 1 = free, 0 = allocated; from BootstrapAlloc
    uint32_t page_count;
    uint32_t object_size;
    uint16_t object_count;
    uint16_t allocated_count;
    uint8_t size_class;
    bool is_large;              // true for mmap-backed large allocations
    uint16_t free_hint_word;    // word index to start bitmap scan from

    // Intrusive list pointers (used by Slab's partial/full/empty lists)
    Span* list_prev;
    Span* list_next;

    // For large allocations: actual requested size
    size_t large_size;

    // For thread cache pool recycling
    Span* next_free;

    // Initialize a slab span for the given size class
    void init(void* base_, uint32_t pages, uint8_t sc) {
        base = base_;
        page_count = pages;
        size_class = sc;
        is_large = false;
        object_size = kSizeClasses[sc].size;
        object_count = static_cast<uint16_t>((pages * kPageSize) / object_size);
        allocated_count = 0;
        free_hint_word = 0;
        list_prev = nullptr;
        list_next = nullptr;
        large_size = 0;
        next_free = nullptr;

        // Allocate bitmap from bootstrap (1 bit per object, in uint64_t words)
        size_t num_words = (object_count + 63) / 64;
        bitmap = static_cast<uint64_t*>(
            BootstrapAlloc::instance().allocate(num_words * sizeof(uint64_t), alignof(uint64_t)));

        // Set all valid object bits to 1 (free)
        if (num_words > 0) {
            // Set all full words to all-1s
            __builtin_memset(bitmap, 0xFF, num_words * sizeof(uint64_t));
            // Mask off trailing bits in the last word that don't correspond to objects
            size_t valid_bits = object_count % 64;
            if (valid_bits != 0) {
                bitmap[num_words - 1] = (1ULL << valid_bits) - 1;
            }
        }
    }

    // Initialize a large allocation span
    void initLarge(void* base_, size_t size, uint32_t pages) {
        base = base_;
        page_count = pages;
        size_class = kNumClasses;
        is_large = true;
        object_size = 0;
        object_count = 0;
        allocated_count = 1;
        free_hint_word = 0;
        bitmap = nullptr;
        list_prev = nullptr;
        list_next = nullptr;
        large_size = size;
        next_free = nullptr;
    }

    // Allocate one object. Returns nullptr if span is full.
    void* allocate() {
        size_t num_words = (object_count + 63) / 64;
        // Scan from hint position
        for (size_t i = free_hint_word; i < num_words; ++i) {
            if (bitmap[i] != 0) {
                int bit = ctz(bitmap[i]);
                size_t slot = i * 64 + bit;
                bitmap[i] &= ~(1ULL << bit);   // mark allocated
                ++allocated_count;
                free_hint_word = static_cast<uint16_t>(i);
                return static_cast<char*>(base) + slot * object_size;
            }
        }
        // Wraparound: scan from beginning to hint
        for (size_t i = 0; i < free_hint_word; ++i) {
            if (bitmap[i] != 0) {
                int bit = ctz(bitmap[i]);
                size_t slot = i * 64 + bit;
                bitmap[i] &= ~(1ULL << bit);
                ++allocated_count;
                free_hint_word = static_cast<uint16_t>(i);
                return static_cast<char*>(base) + slot * object_size;
            }
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
