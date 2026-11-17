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

// ── Fail-open kill switch ────────────────────────────────────────────────────
// smash_disable(): stop compressing and return the process to an inert state.
// Every compressed page is decompressed back in place, every protection smash
// applied (PROT_READ monitoring, PROT_NONE, uffd write-protect) is removed,
// and Phase 1/2/3 stop. The allocator itself keeps serving malloc/free (it
// owns the live objects); it just never induces a fault again. Blocks until
// the drain completes (up to 30 s). Returns 0 = inert, 1 = still draining at
// the timeout (the drain continues in the background), -1 = no compressor.
// Same effect as SMASH_DISABLE_SIGNAL / a SMASH_CONTROL_FILE "disable", and
// what the watchdog does when it trips.
int smash_disable(void);

// smash_enable(): resume compression (cold streaks restart from zero).
// Returns 0 = enabled, 1 = timeout, -1 = no compressor.
int smash_enable(void);

struct SmashSafetyStatus {
    int disabled;                       // kill switch engaged
    int inert;                          // ... and the drain has finished
    unsigned long long compressed_pages;    // COMPRESSED + shadow pages now
    unsigned long long reclaimed_pages;     // of which reclaimed (COMPRESSED: memory dropped)
    unsigned long long faults;              // smash faults served since start
    unsigned long long decompress_faults;   // of which decompressed a page
    unsigned long long decompress_failures; // short decompressions (data loss)
    unsigned long long watchdog_trips;
    unsigned cold_floor_ticks;          // effective cold threshold now
    unsigned slo_multiplier_x256;       // SMASH_MAX_FAULT_RATE scale (256 = 1x)
    unsigned long long fault_rate;      // decompress faults/s, last tick
    unsigned long long fault_p99_us;    // fault-service p99, last tick (needs SMASH_WATCHDOG_P99_US)
    char reason[128];                   // why it was last disabled
};

// Fill *st. Returns 0, or -1 if smash has no compressor in this process.
int smash_safety_status(struct SmashSafetyStatus* st);

#ifdef __cplusplus
}
#endif
