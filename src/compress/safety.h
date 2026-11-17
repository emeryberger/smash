// smash/src/compress/safety.h - Fail-open kill switch, watchdog, SLO controller
//
// Deployment-safety plumbing shared by CompressorThread (which acts on it once
// per tick) and the exported C API / signal handlers (which only post requests).
// Everything here is either pure logic (HealthMonitor: unit-tested directly by
// tests/test_safety.cpp) or async-signal-safe (the request mailbox, the fault
// latency histogram). Nothing allocates.
//
//   Kill switch: a request (disable / enable) is posted into g_request by
//     smash_disable()/smash_enable(), by the SMASH_DISABLE_SIGNAL /
//     SMASH_ENABLE_SIGNAL handlers, or by the coordinator itself (control-file
//     poll, watchdog). The coordinator consumes it at its next 10 ms poll, so
//     the request is applied between ticks, never concurrently with a phase.
//   Watchdog: trips the kill switch when a health metric stays over its limit
//     for a window of ticks, or on a decompression failure.
//   SLO controller: instead of tripping, scales the effective cold threshold
//     multiplicatively while the refault rate is over budget.
//   See agents/safety.md.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>

namespace smash::safety {

// ── Request mailbox ─────────────────────────────────────────────────────────
enum Kind : int { kNone = 0, kDisable = 1, kEnable = 2 };
enum Reason : int {
    kReasonNone = 0,
    kReasonApi = 1,       // smash_disable() / smash_enable()
    kReasonSignal = 2,    // SMASH_DISABLE_SIGNAL / SMASH_ENABLE_SIGNAL
    kReasonFile = 3,      // SMASH_CONTROL_FILE
    kReasonWatchdog = 4,  // HealthMonitor trip
    kReasonStartup = 5,   // SMASH_START_DISABLED=1
};

// (reason << 4) | kind; 0 = no request pending. Last writer wins: a disable
// followed by an enable before the coordinator looks is an enable.
inline std::atomic<int> g_request{0};

inline void post(int kind, int reason) {
    g_request.store((reason << 4) | kind, std::memory_order_release);
}
inline int takeRequest() { return g_request.exchange(0, std::memory_order_acq_rel); }
inline bool disablePending() {
    return (g_request.load(std::memory_order_relaxed) & 0xf) == kDisable;
}

inline const char* reasonName(int r) {
    switch (r) {
    case kReasonApi: return "api";
    case kReasonSignal: return "signal";
    case kReasonFile: return "control-file";
    case kReasonWatchdog: return "watchdog";
    case kReasonStartup: return "startup";
    default: return "unknown";
    }
}

// Signal number from "12", "SIGUSR1", "USR1", "SIGRTMIN+3", "RTMAX-1", "HUP"...
// Returns 0 for anything unparsable or unsafe to take over (fault signals smash
// itself needs, uncatchable signals).
inline int parseSignal(const char* s) {
    if (!s || !*s) return 0;
    if (std::strncmp(s, "SIG", 3) == 0) s += 3;
    int sig = 0;
    if (s[0] >= '0' && s[0] <= '9') {
        sig = std::atoi(s);
    } else if (std::strcmp(s, "USR1") == 0) {
        sig = SIGUSR1;
    } else if (std::strcmp(s, "USR2") == 0) {
        sig = SIGUSR2;
    } else if (std::strcmp(s, "HUP") == 0) {
        sig = SIGHUP;
    } else if (std::strcmp(s, "WINCH") == 0) {
        sig = SIGWINCH;
#if defined(SIGRTMIN)
    } else if (std::strncmp(s, "RTMIN", 5) == 0) {
        sig = SIGRTMIN + (s[5] == '+' ? std::atoi(s + 6) : 0);
    } else if (std::strncmp(s, "RTMAX", 5) == 0) {
        sig = SIGRTMAX - (s[5] == '-' ? std::atoi(s + 6) : 0);
#endif
    }
    if (sig <= 0 || sig >= 65) return 0;
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGKILL || sig == SIGSTOP ||
        sig == SIGILL || sig == SIGFPE || sig == SIGABRT)
        return 0;
    return sig;
}

// ── Fault-service latency histogram (async-signal-safe recording) ───────────
// Log-linear: 4 sub-buckets per power of two of microseconds, so a bucket's
// upper bound over-states the value inside it by at most 19% (2^(1/4) - 1).
// p99 is reported as the upper bound of the bucket holding the 99th percentile
// sample: a conservative (early-tripping) estimate.
struct LatencyHist {
    static constexpr int kSub = 4;
    static constexpr int kOct = 26;  // up to 2^26 us (~67 s)
    static constexpr int kBuckets = kSub * kOct;
    std::atomic<uint32_t> b[kBuckets] = {};

    static int bucketOf(uint64_t us) {
        if (us < static_cast<uint64_t>(kSub)) return static_cast<int>(us);  // 0..3 exact
        const int msb = 63 - __builtin_clzll(us);  // >= 2
        const int sub = static_cast<int>((us >> (msb - 2)) & 3);
        int idx = (msb - 1) * kSub + sub;           // msb=2 -> 4..7
        return idx >= kBuckets ? kBuckets - 1 : idx;
    }
    // Smallest value strictly above every value mapped to bucket idx.
    static uint64_t upperBoundUs(int idx) {
        if (idx < kSub) return static_cast<uint64_t>(idx) + 1;
        const int msb = idx / kSub + 1;
        const uint64_t sub = static_cast<uint64_t>(idx % kSub);
        return (uint64_t{4} + sub + 1) << (msb - 2);
    }
    void record(uint64_t ns) {
        b[bucketOf(ns / 1000)].fetch_add(1, std::memory_order_relaxed);
    }
    // Drain all buckets (reset to 0). Returns the sample count; *p99_us gets
    // the conservative p99 (0 if no samples).
    uint64_t drain(uint64_t* p99_us) {
        uint32_t snap[kBuckets];
        uint64_t n = 0;
        for (int i = 0; i < kBuckets; ++i) {
            snap[i] = b[i].exchange(0, std::memory_order_relaxed);
            n += snap[i];
        }
        *p99_us = 0;
        if (n == 0) return 0;
        // Rank of the p99 sample (1-based): ceil(0.99 n).
        const uint64_t rank = (n * 99 + 99) / 100;
        uint64_t acc = 0;
        for (int i = 0; i < kBuckets; ++i) {
            acc += snap[i];
            if (acc >= rank) { *p99_us = upperBoundUs(i); break; }
        }
        return n;
    }
};

// ── Watchdog + SLO controller (pure logic) ──────────────────────────────────
struct HealthConfig {
    uint64_t wd_fault_rate = 0;   // SMASH_WATCHDOG_FAULT_RATE: decompress faults/s (0 = off)
    uint64_t wd_p99_us = 0;       // SMASH_WATCHDOG_P99_US: fault-service p99 (0 = off)
    uint64_t wd_decomp_fail = 0;  // SMASH_WATCHDOG_DECOMP_FAIL: failures to trip (0 = off, the default)
    uint32_t wd_window = 5;       // SMASH_WATCHDOG_WINDOW: ticks (1..kMaxWindow) a limit must hold
    uint64_t wd_min_samples = 20; // p99 needs at least this many faults in a tick
    uint64_t max_fault_rate = 0;  // SMASH_MAX_FAULT_RATE: controller budget (0 = off)
    bool watchdogOn() const { return wd_fault_rate || wd_p99_us || wd_decomp_fail; }
    bool timingOn() const { return wd_p99_us != 0; }
};

// One compressor tick's worth of health data.
struct HealthSample {
    uint64_t dt_ns = 0;             // wall time covered by this sample
    uint64_t decomp_faults = 0;     // app faults that decompressed a page, this tick
    uint64_t latency_samples = 0;   // faults timed this tick
    uint64_t p99_us = 0;            // conservative p99 of those
    uint64_t decomp_fail_total = 0; // cumulative decompression failures
};

// Refaults arrive in bursts (a page recompresses one tick, refaults the next),
// so the fault-rate limit is checked against the rate averaged over the last
// wd_window ticks, not tick by tick. The p99 limit must be exceeded on
// wd_window consecutive ticks that had at least wd_min_samples faults; ticks
// with fewer samples are neutral (they neither count nor reset).
class HealthMonitor {
public:
    static constexpr uint32_t kMaxWindow = 120;
    static constexpr uint32_t kMultOne = 256;            // x256 fixed point
    static constexpr uint32_t kMultMax = 255 * kMultOne; // floor 1 can reach 255 ticks

    explicit HealthMonitor(const HealthConfig& c = {}) : cfg_(c) {}
    void configure(const HealthConfig& c) { cfg_ = c; }
    const HealthConfig& config() const { return cfg_; }

    // Forget watchdog history (on re-enable). The decompress-failure count is
    // re-based so failures before the re-enable don't re-trip immediately.
    void resetWatchdog(uint64_t decomp_fail_total) {
        p99_streak_ = 0;
        ring_n_ = ring_pos_ = 0;
        fail_base_ = decomp_fail_total;
    }

    // Feed one tick. Returns true if the watchdog trips (only when `armed`);
    // `reason` then holds a one-line description. The controller multiplier is
    // updated regardless.
    bool update(const HealthSample& s, bool armed, char* reason, size_t len) {
        const uint64_t dt = s.dt_ns ? s.dt_ns : 1;
        last_rate_ = s.decomp_faults * 1000000000ull / dt;
        last_p99_us_ = s.latency_samples ? s.p99_us : 0;

        if (cfg_.max_fault_rate) {
            uint32_t m = mult_.load(std::memory_order_relaxed);
            if (last_rate_ > cfg_.max_fault_rate) {
                m = m >= kMultMax / 2 ? kMultMax : m * 2;
            } else if (last_rate_ * 2 < cfg_.max_fault_rate && m > kMultOne) {
                uint32_t d = m / 8;
                m = m - d < kMultOne ? kMultOne : m - d;
            }
            mult_.store(m, std::memory_order_relaxed);
        }

        if (!armed) return false;
        if (cfg_.wd_decomp_fail &&
            s.decomp_fail_total - fail_base_ >= cfg_.wd_decomp_fail) {
            fmt(reason, len, "decompress_failures=", s.decomp_fail_total - fail_base_,
                " >= limit ", cfg_.wd_decomp_fail, "");
            return true;
        }
        const uint32_t win = window();
        if (cfg_.wd_fault_rate) {
            ring_f_[ring_pos_] = s.decomp_faults;
            ring_dt_[ring_pos_] = dt;
            ring_pos_ = (ring_pos_ + 1) % win;
            if (ring_n_ < win) ++ring_n_;
            if (ring_n_ >= win) {
                uint64_t f = 0, t = 0;
                for (uint32_t k = 0; k < win; ++k) { f += ring_f_[k]; t += ring_dt_[k]; }
                const uint64_t avg = t ? static_cast<uint64_t>(
                                             static_cast<double>(f) * 1e9 / static_cast<double>(t))
                                       : 0;
                if (avg > cfg_.wd_fault_rate) {
                    fmt(reason, len, "fault_rate=", avg, "/s > limit ",
                        cfg_.wd_fault_rate, "/s averaged over ");
                    appendWindow(reason, len);
                    return true;
                }
            }
        }
        if (cfg_.wd_p99_us && s.latency_samples >= cfg_.wd_min_samples) {
            p99_streak_ = s.p99_us > cfg_.wd_p99_us ? p99_streak_ + 1 : 0;
            if (p99_streak_ >= win) {
                fmt(reason, len, "fault_p99_us<=", s.p99_us, " > limit ",
                    cfg_.wd_p99_us, "us for ");
                appendWindow(reason, len);
                return true;
            }
        }
        return false;
    }

    uint32_t multX256() const { return mult_.load(std::memory_order_relaxed); }
    uint64_t lastRate() const { return last_rate_; }
    uint64_t lastP99Us() const { return last_p99_us_; }

    // floor * multiplier, clamped to the 8-bit cold counter's range.
    static uint32_t scaleFloor(uint32_t floor, uint32_t mult_x256) {
        const uint64_t f = (static_cast<uint64_t>(floor) * mult_x256 + kMultOne - 1) / kMultOne;
        return f > 255 ? 255 : static_cast<uint32_t>(f);
    }

private:
    HealthConfig cfg_;
    std::atomic<uint32_t> mult_{kMultOne};
    uint32_t p99_streak_ = 0;
    uint64_t ring_f_[kMaxWindow] = {}, ring_dt_[kMaxWindow] = {};
    uint32_t ring_n_ = 0, ring_pos_ = 0;
    uint32_t window() const {
        return cfg_.wd_window < 1 ? 1 : cfg_.wd_window > kMaxWindow ? kMaxWindow : cfg_.wd_window;
    }
    uint64_t fail_base_ = 0;
    uint64_t last_rate_ = 0, last_p99_us_ = 0;

    // Tiny allocation-free formatter: a value-labelled line.
    static void put(char* out, size_t len, size_t& n, const char* s) {
        while (*s && n + 1 < len) out[n++] = *s++;
        out[n] = 0;
    }
    static void putU(char* out, size_t len, size_t& n, uint64_t v) {
        char tmp[24]; int k = 0;
        do { tmp[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k && n + 1 < len) out[n++] = tmp[--k];
        out[n] = 0;
    }
    static void fmt(char* out, size_t len, const char* a, uint64_t x, const char* b,
                    uint64_t y, const char* c) {
        if (!out || !len) return;
        size_t n = 0;
        out[0] = 0;
        put(out, len, n, a); putU(out, len, n, x); put(out, len, n, b);
        putU(out, len, n, y); put(out, len, n, c);
    }
    void appendWindow(char* out, size_t len) const {
        if (!out || !len) return;
        size_t n = std::strlen(out);
        putU(out, len, n, window());
        put(out, len, n, " ticks");
    }
};

}  // namespace smash::safety
