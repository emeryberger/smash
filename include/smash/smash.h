// smash/smash.h - Public API for Smash compression-aware allocator
//
// Smash works as a transparent malloc replacement via alloc8 interposition.
// This header provides optional application-level hints for advanced usage.
#pragma once

#ifdef __cplusplus
#include <cstddef>
#else
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Hint that a region has gone cold: the pages lying wholly inside
// [ptr, ptr+size) skip the rest of their cold wait, so the next compressor
// tick (about 1 s) may compress them. The compression-ratio and cost gates
// still decide; pages smash does not manage are ignored. Not a guarantee.
void smash_hint_cold(void* ptr, size_t size);

// Hint that a region is about to be used: every page overlapping it restarts
// its cold wait, and compressed ones are decompressed now (by touching them)
// instead of on first access.
void smash_hint_hot(void* ptr, size_t size);

// Process-wide compression statistics. Returns 0, or -1 if smash has no
// compressor in this process (e.g. not preloaded).
struct SmashStats {
    unsigned long long managed_bytes;      // memory in pages smash manages now
    unsigned long long compressed_pages;   // pages compressed with memory released, now
    unsigned long long compressed_bytes;   // their uncompressed size
    unsigned long long stored_bytes;       // blob bytes holding them (+ pending ones)
    unsigned long long compressions;       // pages compressed and kept, since start
    unsigned long long decompress_faults;  // accesses that decompressed a page, since start
};

int smash_get_stats(struct SmashStats* stats);

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
