// Unit tests for compress/safety.h: signal parsing, the fault-latency
// histogram, and the HealthMonitor watchdog / SLO-controller logic. Pure logic,
// deterministic, no compressor thread (the end-to-end behaviour is covered by
// kill_switch_test.cpp and storm_test.cpp under interposition).
#include "compress/safety.h"

#include <cstdio>
#include <cstring>

using namespace smash::safety;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

static void testParseSignal() {
    CHECK(parseSignal("SIGUSR1") == SIGUSR1, "SIGUSR1");
    CHECK(parseSignal("USR2") == SIGUSR2, "USR2");
    char num[16];
    snprintf(num, sizeof(num), "%d", SIGUSR1);
    CHECK(parseSignal(num) == SIGUSR1, "numeric");
    snprintf(num, sizeof(num), "%d", SIGSEGV);
    CHECK(parseSignal("SIGSEGV") == 0 && parseSignal(num) == 0, "SEGV must be refused");
    snprintf(num, sizeof(num), "%d", SIGBUS);
    CHECK(parseSignal("SIGBUS") == 0 && parseSignal(num) == 0, "BUS must be refused");
    CHECK(parseSignal("SIGKILL") == 0 && parseSignal("9") == 0, "KILL refused");
    CHECK(parseSignal(nullptr) == 0 && parseSignal("") == 0 && parseSignal("bogus") == 0,
          "garbage refused");
#if defined(SIGRTMIN)
    CHECK(parseSignal("SIGRTMIN+2") == SIGRTMIN + 2, "RTMIN+2");
    CHECK(parseSignal("RTMAX-1") == SIGRTMAX - 1, "RTMAX-1");
#endif
}

static void testLatencyBuckets() {
    // Every value lands in a bucket whose [lower, upper) contains it, and the
    // upper bound over-states by at most 25% (4 sub-buckets per octave).
    for (uint64_t us = 0; us < (1u << 22); us = us < 64 ? us + 1 : us + us / 7) {
        const int b = LatencyHist::bucketOf(us);
        const uint64_t ub = LatencyHist::upperBoundUs(b);
        const uint64_t lb = b == 0 ? 0 : LatencyHist::upperBoundUs(b - 1);
        CHECK(us < ub && us >= lb, "us=%llu bucket=%d [%llu,%llu)", (unsigned long long)us, b,
              (unsigned long long)lb, (unsigned long long)ub);
        if (us >= 4) CHECK(ub * 4 <= (us + 1) * 5, "bound too loose us=%llu ub=%llu",
                           (unsigned long long)us, (unsigned long long)ub);
    }
    LatencyHist h;
    uint64_t p99 = 1;
    CHECK(h.drain(&p99) == 0 && p99 == 0, "empty drain");
    for (int i = 0; i < 990; ++i) h.record(10 * 1000);       // 10 us
    for (int i = 0; i < 10; ++i) h.record(5000 * 1000);      // 5 ms tail (1%)
    CHECK(h.drain(&p99) == 1000, "count");
    CHECK(p99 > 10 && p99 <= 13, "p99 should be in the 10 us bucket, got %llu",
          (unsigned long long)p99);
    for (int i = 0; i < 980; ++i) h.record(10 * 1000);
    for (int i = 0; i < 20; ++i) h.record(5000 * 1000);      // 2% tail
    h.drain(&p99);
    CHECK(p99 > 5000 && p99 <= 6250, "p99 should be in the 5 ms bucket, got %llu",
          (unsigned long long)p99);
    CHECK(h.drain(&p99) == 0, "drain resets");
}

static HealthSample tickWith(uint64_t faults, uint64_t lat_n = 0, uint64_t p99 = 0,
                             uint64_t fails = 0) {
    HealthSample s;
    s.dt_ns = 1000000000ull;  // 1 s ticks
    s.decomp_faults = faults;
    s.latency_samples = lat_n;
    s.p99_us = p99;
    s.decomp_fail_total = fails;
    return s;
}

static void testController() {
    HealthConfig c;
    c.wd_decomp_fail = 0;
    c.max_fault_rate = 100;
    HealthMonitor m(c);
    char why[96];
    CHECK(m.multX256() == 256, "starts at 1x");
    // Over budget: doubles each tick.
    m.update(tickWith(500), true, why, sizeof(why));
    CHECK(m.multX256() == 512, "2x after one storm tick, got %u", m.multX256());
    m.update(tickWith(500), true, why, sizeof(why));
    CHECK(m.multX256() == 1024, "4x");
    // Between budget/2 and budget: hold.
    m.update(tickWith(80), true, why, sizeof(why));
    CHECK(m.multX256() == 1024, "hold in the hysteresis band");
    // Under budget/2: decays by 1/8 per tick, back to exactly 1x.
    uint32_t prev = m.multX256();
    for (int i = 0; i < 40; ++i) {
        m.update(tickWith(0), true, why, sizeof(why));
        CHECK(m.multX256() <= prev, "monotone decay");
        prev = m.multX256();
    }
    CHECK(m.multX256() == 256, "decays back to 1x, got %u", m.multX256());
    // Saturates at the cap.
    for (int i = 0; i < 20; ++i) m.update(tickWith(1000000), true, why, sizeof(why));
    CHECK(m.multX256() == HealthMonitor::kMultMax, "cap");
    CHECK(HealthMonitor::scaleFloor(10, 256) == 10, "1x floor");
    CHECK(HealthMonitor::scaleFloor(10, 1024) == 40, "4x floor");
    CHECK(HealthMonitor::scaleFloor(10, HealthMonitor::kMultMax) == 255, "clamped to 255");
    CHECK(HealthMonitor::scaleFloor(1, HealthMonitor::kMultMax) == 255, "floor 1 reaches 255");
    // The controller alone never trips.
    CHECK(!m.update(tickWith(1000000), true, why, sizeof(why)), "controller never trips");
}

static void testWatchdogRate() {
    HealthConfig c;
    c.wd_decomp_fail = 0;
    c.wd_fault_rate = 100;
    c.wd_window = 3;
    HealthMonitor m(c);
    char why[96] = "";
    // Needs a full window of history before it can trip.
    CHECK(!m.update(tickWith(1000), true, why, sizeof(why)), "1 tick of history");
    CHECK(!m.update(tickWith(1000), true, why, sizeof(why)), "2 ticks of history");
    CHECK(m.update(tickWith(1000), true, why, sizeof(why)), "3 ticks averaging 1000/s trips");
    CHECK(std::strstr(why, "fault_rate=1000/s") && std::strstr(why, "limit 100/s"),
          "reason: '%s'", why);
    // Bursty refaults (quiet tick, burst tick, ...) are judged on the average.
    HealthMonitor b(c);
    bool tripped = false;
    for (int i = 0; i < 6 && !tripped; ++i)
        tripped = b.update(tickWith(i % 2 ? 900 : 0), true, why, sizeof(why));
    CHECK(tripped, "alternating 0/900 per tick (avg 300/s) must trip");
    HealthMonitor q(c);
    for (int i = 0; i < 12; ++i)
        CHECK(!q.update(tickWith(i % 3 == 0 ? 250 : 0), true, why, sizeof(why)),
              "avg 83/s is under 100/s (tick %d)", i);
    // Not armed (already disabled): never trips.
    HealthMonitor d(c);
    for (int i = 0; i < 10; ++i) CHECK(!d.update(tickWith(500), false, why, sizeof(why)), "unarmed");
    // Rate is normalised by dt.
    HealthMonitor r(c);
    HealthSample s = tickWith(500);
    s.dt_ns = 10ull * 1000000000ull;  // 50/s
    for (int i = 0; i < 10; ++i) CHECK(!r.update(s, true, why, sizeof(why)), "50/s is under 100/s");
    CHECK(r.lastRate() == 50, "lastRate %llu", (unsigned long long)r.lastRate());
    // Re-enable forgets the window.
    HealthMonitor w(c);
    w.update(tickWith(1000), true, why, sizeof(why));
    w.update(tickWith(1000), true, why, sizeof(why));
    w.resetWatchdog(0);
    CHECK(!w.update(tickWith(1000), true, why, sizeof(why)), "history cleared by reset");
}

static void testWatchdogP99() {
    HealthConfig c;
    c.wd_decomp_fail = 0;
    c.wd_p99_us = 1000;
    c.wd_window = 2;
    c.wd_min_samples = 20;
    HealthMonitor m(c);
    char why[96] = "";
    CHECK(!m.update(tickWith(5, 5, 50000), true, why, sizeof(why)), "too few samples");
    CHECK(!m.update(tickWith(5, 5, 50000), true, why, sizeof(why)), "too few samples (2)");
    CHECK(!m.update(tickWith(100, 100, 5000), true, why, sizeof(why)), "1/2");
    CHECK(!m.update(tickWith(0, 0, 0), true, why, sizeof(why)), "a sample-less tick is neutral");
    CHECK(m.update(tickWith(100, 100, 5000), true, why, sizeof(why)), "2/2 trips");
    CHECK(std::strstr(why, "p99") != nullptr, "reason: '%s'", why);
    HealthMonitor u(c);
    CHECK(!u.update(tickWith(100, 100, 5000), true, why, sizeof(why)), "1/2");
    CHECK(!u.update(tickWith(100, 100, 500), true, why, sizeof(why)), "an under-limit tick resets");
    CHECK(!u.update(tickWith(100, 100, 5000), true, why, sizeof(why)), "1/2 again");
}

static void testWatchdogDecompFail() {
    HealthConfig d;  // default: off (opt-in)
    HealthMonitor dm(d);
    char why0[96] = "";
    CHECK(!dm.update(tickWith(0, 0, 0, 99), true, why0, sizeof(why0)), "off by default");
    HealthConfig c;
    c.wd_decomp_fail = 1;  // SMASH_WATCHDOG_DECOMP_FAIL=1: trip on the first failure
    HealthMonitor m(c);
    char why[96] = "";
    CHECK(!m.update(tickWith(0, 0, 0, 0), true, why, sizeof(why)), "no failures");
    CHECK(m.update(tickWith(0, 0, 0, 1), true, why, sizeof(why)), "first failure trips");
    CHECK(std::strstr(why, "decompress_failures=1") != nullptr, "reason '%s'", why);
    // Re-enable re-bases the count: old failures do not re-trip.
    m.resetWatchdog(1);
    CHECK(!m.update(tickWith(0, 0, 0, 1), true, why, sizeof(why)), "rebased");
    CHECK(m.update(tickWith(0, 0, 0, 2), true, why, sizeof(why)), "a new failure trips");
    HealthConfig off;
    off.wd_decomp_fail = 0;
    HealthMonitor o(off);
    CHECK(!o.update(tickWith(0, 0, 0, 99), true, why, sizeof(why)), "=0 disables");
}

static void testMailbox() {
    g_request.store(0);
    post(kDisable, kReasonSignal);
    CHECK(disablePending(), "disable pending");
    post(kEnable, kReasonApi);  // last writer wins
    CHECK(!disablePending(), "enable replaced it");
    const int r = takeRequest();
    CHECK((r & 0xf) == kEnable && (r >> 4) == kReasonApi, "take");
    CHECK(takeRequest() == 0, "empty after take");
}

int main() {
    testParseSignal();
    testLatencyBuckets();
    testController();
    testWatchdogRate();
    testWatchdogP99();
    testWatchdogDecompFail();
    testMailbox();
    if (failures) {
        fprintf(stderr, "test_safety: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_safety: all passed\n");
    return 0;
}
