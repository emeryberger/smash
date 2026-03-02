// smash/smash.h - Public API for Smash compression-aware allocator
//
// Smash works as a transparent malloc replacement via alloc8 interposition.
// This header provides optional application-level hints for advanced usage.
#pragma once

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Hint to smash that a region is about to go cold (eligible for compression).
// No-op in Phase 1.
void smash_hint_cold(void* ptr, size_t size);

// Hint to smash that a region is about to be heavily accessed (keep decompressed).
// No-op in Phase 1.
void smash_hint_hot(void* ptr, size_t size);

// Get compression statistics.
struct SmashStats {
    size_t total_allocated;
    size_t total_freed;
    size_t compressed_pages;
    size_t compressed_bytes_saved;
    size_t active_spans;
    size_t active_large_allocs;
};

void smash_get_stats(SmashStats* stats);

#ifdef __cplusplus
}
#endif
