// Synthetic refault storm against the watchdog and the SLO controller. Run
// under LD_PRELOAD / DYLD_INSERT_LIBRARIES, both modes:
//   storm_test watchdog-rate  SMASH_WATCHDOG_FAULT_RATE: the storm must trip the
//                             kill switch (reason "watchdog: fault_rate=..."),
//                             after which the process is inert and data exact.
//   storm_test watchdog-p99   SMASH_WATCHDOG_P99_US (set tiny): same, reason p99.
//   storm_test controller     SMASH_MAX_FAULT_RATE: the effective cold threshold
//                             must rise during the storm (no trip), and the
//                             multiplier must decay once the storm stops.
//
// The storm: compressible large chunks (compressible in both modes) are read
// in full every 150 ms. Reads do not mark a page hot to Phase 3 (it tracks
// writes), so each page recompresses after the cold threshold and the next
// read refaults it: a sustained refault rate of (pages / cold threshold) per
// second. SMASH_RECOMPRESS_BACKOFF=0 keeps the per-page backoff from damping
// the storm, so the test exercises the safety mechanisms, not the backoff.
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct Status {  // mirrors SmashSafetyStatus (include/smash/smash.h)
    int disabled, inert;
    unsigned long long compressed_pages, reclaimed_pages, faults, decompress_faults, decompress_failures,
        watchdog_trips;
    unsigned cold_floor_ticks, slo_multiplier_x256;
    unsigned long long fault_rate, fault_p99_us;
    char reason[128];
};
using status_fn = int (*)(Status*);
status_fn g_status;

constexpr size_t kChunk = 2 * 1024 * 1024;
constexpr size_t kNumChunks = 8;  // 16 MiB = 4096 pages
unsigned char* g_chunks[kNumChunks];

unsigned char pat(size_t c, size_t off) {
    return static_cast<unsigned char>(1 + ((c * 13 + off / 128) % 253));
}

Status status() {
    Status s{};
    if (g_status(&s) != 0) { fprintf(stderr, "FAIL: smash_safety_status\n"); exit(1); }
    return s;
}

bool readAll() {
    for (size_t c = 0; c < kNumChunks; ++c)
        for (size_t j = 0; j < kChunk; ++j)
            if (g_chunks[c][j] != pat(c, j)) {
                fprintf(stderr, "FAIL: chunk %zu byte %zu = 0x%02x want 0x%02x\n", c, j,
                        g_chunks[c][j], pat(c, j));
                return false;
            }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "watchdog-rate";
    const char* lo = getenv("SMASH_LARGE_ONLY");
    const char* heap = lo && lo[0] == '1' ? "large-only" : "full";
    fprintf(stderr, "storm_test: %s (%s)\n", mode.c_str(), heap);
    g_status = reinterpret_cast<status_fn>(dlsym(RTLD_DEFAULT, "smash_safety_status"));
    if (!g_status) { fprintf(stderr, "FAIL: smash_safety_status not exported\n"); return 1; }

    for (size_t c = 0; c < kNumChunks; ++c) {
        g_chunks[c] = static_cast<unsigned char*>(malloc(kChunk));
        for (size_t j = 0; j < kChunk; ++j) g_chunks[c][j] = pat(c, j);
    }
    for (int i = 0; i < 5000; ++i) free(malloc(64));

    const unsigned base_floor = status().cold_floor_ticks;
    bool compressed = false;
    for (int t = 0; t < 400 && !compressed; ++t) {
        usleep(100 * 1000);
        compressed = status().compressed_pages > 256;
    }
    if (!compressed) { fprintf(stderr, "FAIL: nothing compressed within 40 s\n"); return 1; }
    fprintf(stderr, "storm_test: compressed, base cold floor %u ticks\n", base_floor);

    // Storm for up to 40 s.
    unsigned peak_floor = base_floor, peak_mult = 256;
    bool tripped = false;
    for (int round = 0; round < 260; ++round) {
        if (!readAll()) return 1;
        usleep(150 * 1000);
        Status s = status();
        if (s.cold_floor_ticks > peak_floor) peak_floor = s.cold_floor_ticks;
        if (s.slo_multiplier_x256 > peak_mult) peak_mult = s.slo_multiplier_x256;
        if (round % 10 == 0)
            fprintf(stderr, "  t=%.1fs rate=%llu/s p99=%lluus floor=%u mult=%u compressed=%llu\n",
                    round * 0.15, s.fault_rate, s.fault_p99_us, s.cold_floor_ticks,
                    s.slo_multiplier_x256, s.compressed_pages);
        if (s.disabled) { tripped = true; break; }
        if (mode == "controller" && peak_floor >= 4 * base_floor && round > 20) break;
    }

    Status s = status();
    if (mode == "controller") {
        if (tripped) {
            fprintf(stderr, "FAIL: controller mode must adapt, not trip (reason %s)\n", s.reason);
            return 1;
        }
        if (peak_floor < 2 * base_floor) {
            fprintf(stderr, "FAIL: cold floor never rose (base %u, peak %u)\n", base_floor,
                    peak_floor);
            return 1;
        }
        fprintf(stderr, "storm_test: floor rose %u -> %u ticks (mult x256 %u)\n", base_floor,
                peak_floor, peak_mult);
        // Storm over: the multiplier decays.
        unsigned m0 = status().slo_multiplier_x256, m = m0;
        for (int t = 0; t < 200 && m >= m0; ++t) {
            usleep(100 * 1000);
            m = status().slo_multiplier_x256;
        }
        if (m >= m0) {
            fprintf(stderr, "FAIL: multiplier did not decay (%u)\n", m0);
            return 1;
        }
        fprintf(stderr, "storm_test: multiplier decaying %u -> %u\n", m0, m);
        if (!readAll()) return 1;
        fprintf(stderr, "storm_test: PASS (controller, %s)\n", heap);
        return 0;
    }

    if (!tripped) { fprintf(stderr, "FAIL: watchdog did not trip during the storm\n"); return 1; }
    const char* want = mode == "watchdog-p99" ? "watchdog: fault_p99" : "watchdog: fault_rate";
    if (std::strncmp(s.reason, want, std::strlen(want)) != 0 || s.watchdog_trips < 1) {
        fprintf(stderr, "FAIL: reason \"%s\" trips=%llu, want \"%s...\"\n", s.reason,
                s.watchdog_trips, want);
        return 1;
    }
    fprintf(stderr, "storm_test: tripped: %s\n", s.reason);
    for (int t = 0; t < 300 && !status().inert; ++t) usleep(100 * 1000);
    s = status();
    if (!s.inert || s.compressed_pages != 0) {
        fprintf(stderr, "FAIL: not inert after trip (inert=%d compressed=%llu)\n", s.inert,
                s.compressed_pages);
        return 1;
    }
    const unsigned long long f0 = s.faults;
    if (!readAll()) return 1;
    sleep(2);
    if (!readAll()) return 1;
    s = status();
    if (s.faults != f0 || s.compressed_pages != 0) {
        fprintf(stderr, "FAIL: %llu faults / %llu compressed after the trip\n", s.faults - f0,
                s.compressed_pages);
        return 1;
    }
    fprintf(stderr, "storm_test: PASS (%s, %s)\n", mode.c_str(), heap);
    return 0;
}
