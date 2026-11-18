// Unit test for util/cgroup_monitor.h (SMASH_CGROUP):
//   1. pure parsers: /proc/self/cgroup, memory.max/high/current, keyed files
//      (memory.events), PSI (memory.pressure);
//   2. the pure policy decide(): thresholds, PSI trigger, sticky hold window,
//      cold-floor override;
//   3. Monitor against a fake cgroup tree (the SMASH_CGROUP_ROOT mechanism):
//      Kubernetes-style pod + container nesting where the binding limit is on
//      the PARENT, an uncapped tree, and a missing directory.
// No smash allocator involved; runs on Linux and macOS.
#include "util/cgroup_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace smash::cgroup;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

static void writeFile(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void testParsers() {
    char out[kPathMax];
    CHECK(parseSelfCgroup("0::/kubepods.slice/pod1/cri-x.scope\n", out, sizeof(out)) &&
          strcmp(out, "/kubepods.slice/pod1/cri-x.scope") == 0, "v2 line: %s", out);
    CHECK(parseSelfCgroup("12:memory:/docker/abc\n1:name=systemd:/x\n0::/system.slice/a.service\n",
                          out, sizeof(out)) && strcmp(out, "/system.slice/a.service") == 0,
          "hybrid: %s", out);
    CHECK(parseSelfCgroup("0::/\n", out, sizeof(out)) && strcmp(out, "/") == 0, "namespace root");
    CHECK(!parseSelfCgroup("12:memory:/docker/abc\n", out, sizeof(out)), "v1-only must fail");
    CHECK(!parseSelfCgroup("", out, sizeof(out)), "empty must fail");

    CHECK(parseLimit("max\n") == kUnlimited, "max");
    CHECK(parseLimit("536870912\n") == 536870912LL, "number");
    CHECK(parseLimit("0\n") == 0, "zero");
    CHECK(parseLimit("garbage") == kParseError, "garbage");

    const char* ev = "low 0\nhigh 1234\nmax 7\noom 1\noom_kill 1\noom_group_kill 0\n";
    uint64_t v = 0;
    CHECK(parseKeyed(ev, "high", &v) && v == 1234, "events high=%llu", (unsigned long long)v);
    CHECK(parseKeyed(ev, "max", &v) && v == 7, "events max");
    CHECK(parseKeyed(ev, "oom_kill", &v) && v == 1, "events oom_kill");
    // "oom" must not match "oom_kill"/"oom_group_kill" prefixes and vice versa.
    CHECK(parseKeyed(ev, "oom", &v) && v == 1, "events oom");
    CHECK(!parseKeyed(ev, "missing", &v), "missing key");

    PsiLine some, full;
    const char* psi = "some avg10=12.34 avg60=1.00 avg300=0.50 total=987654\n"
                      "full avg10=0.5 avg60=0.00 avg300=0.00 total=42\n";
    CHECK(parsePsi(psi, &some, &full), "psi parse");
    CHECK(some.valid && some.avg10_x100 == 1234 && some.total_us == 987654,
          "psi some %u %llu", some.avg10_x100, (unsigned long long)some.total_us);
    CHECK(full.valid && full.avg10_x100 == 50 && full.total_us == 42, "psi full %u", full.avg10_x100);
}

static Sample mk(int64_t limit, int64_t current, uint32_t psi_x100 = 0) {
    Sample s;
    s.valid = true;
    s.limit = limit;
    s.current = current;
    s.headroom = limit - current;
    s.psi_some.valid = true;
    s.psi_some.avg10_x100 = psi_x100;
    return s;
}

static void testPolicy() {
    PolicyParams p;            // elevated 10%, critical 3%, psi 10%, hold 10
    const uint32_t base = 10, minf = 2;
    uint32_t hold = 0;
    const int64_t L = 1000LL << 20;

    // Plenty of headroom: no action.
    Decision d = decide(mk(L, L / 2), p, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kNone && !d.skip_nonresident && d.cold_floor == 0 &&
          !d.keep_warm, "idle");

    // 20% headroom: no pressure yet, but the store is kept warm (warm_pct 25).
    d = decide(mk(L, L - L / 5), p, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kNone && d.keep_warm && !d.skip_nonresident, "pre-pressure warm");
    CHECK(hold == 0, "warm alone does not arm the hold");

    // Invalid / uncapped samples never trigger.
    Sample inv;
    d = decide(inv, p, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kNone, "invalid sample");

    // 5% headroom → elevated: skip swapped pages, halve the floor.
    hold = 0;
    d = decide(mk(L, L - L / 20), p, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kElevated && d.skip_nonresident && d.keep_warm && d.cold_floor == 5,
          "elevated level=%d floor=%u", (int)d.level, d.cold_floor);
    CHECK(hold == p.hold_samples, "hold armed");

    // Over memory.high (negative headroom) → critical: minimum floor.
    hold = 0;
    d = decide(mk(L, L + 4096), p, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kCritical && d.cold_floor == minf, "critical floor=%u", d.cold_floor);

    // PSI alone escalates to critical even with headroom.
    hold = 0;
    d = decide(mk(L, L / 2, 2500), p, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kCritical, "psi critical");

    // Sticky: pressure clears but the level is held for hold_samples samples.
    hold = 0;
    Pressure lvl = decide(mk(L, L), p, Pressure::kNone, &hold, base, minf).level;
    CHECK(lvl == Pressure::kCritical, "pinned at limit is critical");
    int held = 0;
    for (int i = 0; i < 100; ++i) {
        d = decide(mk(L, L / 2), p, lvl, &hold, base, minf);
        if (d.level == Pressure::kNone) break;
        CHECK(d.level == Pressure::kCritical, "held level");
        lvl = d.level;
        ++held;
    }
    CHECK(held == static_cast<int>(p.hold_samples), "held %d samples", held);
    CHECK(d.level == Pressure::kNone && d.cold_floor == 0, "released");

    // Floor never rises above the configured base (a base below min stays).
    hold = 0;
    d = decide(mk(L, L), p, Pressure::kNone, &hold, 1, minf);
    CHECK(d.cold_floor == 1, "floor <= base (%u)", d.cold_floor);

    // Between-tick warming only at a pressure level (default skip mode 1).
    hold = 0;
    d = decide(mk(L, L - L / 5), p, Pressure::kNone, &hold, base, minf);
    CHECK(d.keep_warm && !d.warm_between_ticks, "no between-tick warming without pressure");
    d = decide(mk(L, L - L / 20), p, Pressure::kNone, &hold, base, minf);
    CHECK(d.warm_between_ticks, "between-tick warming at elevated");

    // skip_swapped=2: swapped pages are skipped whenever capped, even with
    // plenty of headroom (mode 1 swaps them all back in once the hold ends).
    PolicyParams p2 = p;
    p2.skip_swapped = 2;
    hold = 0;
    d = decide(mk(L, L / 2), p2, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kNone && d.skip_nonresident && !d.keep_warm,
          "skip_swapped=2 skips with headroom");
    d = decide(inv, p2, Pressure::kNone, &hold, base, minf);
    CHECK(!d.skip_nonresident, "skip_swapped=2 needs a valid capped sample");
    // skip_swapped=0: never, even at critical.
    PolicyParams p0 = p;
    p0.skip_swapped = 0;
    hold = 0;
    d = decide(mk(L, L + 4096), p0, Pressure::kNone, &hold, base, minf);
    CHECK(d.level == Pressure::kCritical && !d.skip_nonresident && d.keep_warm,
          "skip_swapped=0 never skips");
}

static void testMonitorFakeTree() {
    char tmpl[] = "cgroup_fake.XXXXXX";
    char* root = mkdtemp(tmpl);
    CHECK(root != nullptr, "mkdtemp");
    if (!root) return;
    char cwd[1024];
    CHECK(getcwd(cwd, sizeof(cwd)) != nullptr, "getcwd");
    std::string base = std::string(cwd) + "/" + root;
    // base/            (no memory.max: models the cgroup root → walk stops)
    // base/pod/        memory.max = 256 MiB, current 250 MiB  (binding)
    // base/pod/ctr/    memory.max = max, memory.high = 512 MiB, current 100 MiB
    std::string pod = base + "/pod", ctr = pod + "/ctr";
    mkdir(pod.c_str(), 0755);
    mkdir(ctr.c_str(), 0755);
    writeFile(pod + "/memory.max", "268435456\n");
    writeFile(pod + "/memory.high", "max\n");
    writeFile(pod + "/memory.current", "262144000\n");
    writeFile(pod + "/memory.events", "low 0\nhigh 0\nmax 3\noom 0\noom_kill 0\n");
    writeFile(ctr + "/memory.max", "max\n");
    writeFile(ctr + "/memory.high", "536870912\n");
    writeFile(ctr + "/memory.current", "104857600\n");
    writeFile(ctr + "/memory.events", "low 0\nhigh 9\nmax 0\noom 0\noom_kill 0\n");
    writeFile(ctr + "/memory.pressure",
              "some avg10=3.50 avg60=0.00 avg300=0.00 total=5000\n"
              "full avg10=1.00 avg60=0.00 avg300=0.00 total=100\n");

    Monitor m;
    CHECK(m.init(ctr.c_str()), "nested tree must be capped");
    CHECK(m.depthCount() == 2, "depth count %d", m.depthCount());
    Sample s = m.sample();
    CHECK(s.valid, "sample valid");
    CHECK(s.binding_depth == 1, "binding depth %d (pod limit is tighter)", s.binding_depth);
    CHECK(s.limit == 268435456LL && s.current == 262144000LL, "limit %lld current %lld",
          (long long)s.limit, (long long)s.current);
    CHECK(s.headroom == 268435456LL - 262144000LL, "headroom");
    CHECK(s.events_max == 3, "events of binding cgroup (%llu)", (unsigned long long)s.events_max);
    CHECK(s.psi_some.valid && s.psi_some.avg10_x100 == 350, "leaf psi %u", s.psi_some.avg10_x100);

    // The policy sees ~2.3% headroom on the pod → critical.
    uint32_t hold = 0;
    Decision d = decide(s, PolicyParams{}, Pressure::kNone, &hold, 10, 2);
    CHECK(d.level == Pressure::kCritical && d.skip_nonresident && d.cold_floor == 2,
          "fake tree decision level=%d", (int)d.level);

    // Limits are re-read every sample (k8s in-place resize): raise the pod.
    writeFile(pod + "/memory.max", "1073741824\n");
    s = m.sample();
    CHECK(s.binding_depth == 0 && s.limit == 536870912LL, "after resize: depth %d limit %lld",
          s.binding_depth, (long long)s.limit);

    // Uncapped tree: memory.max/high = max everywhere → not capped.
    std::string un = base + "/uncapped";
    mkdir(un.c_str(), 0755);
    writeFile(un + "/memory.max", "max\n");
    writeFile(un + "/memory.high", "max\n");
    writeFile(un + "/memory.current", "1000\n");
    Monitor m2;
    CHECK(!m2.init(un.c_str()), "uncapped must not be capped");

    // Missing directory → not capped, no crash.
    Monitor m3;
    CHECK(!m3.init((base + "/nope").c_str()), "missing dir");
    CHECK(!m3.sample().valid, "missing dir sample invalid");

    std::string cmd = "rm -rf '" + base + "'";
    (void)!system(cmd.c_str());
}

int main() {
    testParsers();
    testPolicy();
    testMonitorFakeTree();
    if (failures) {
        fprintf(stderr, "cgroup_monitor_test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "cgroup_monitor_test: ALL PASSED\n");
    return 0;
}
