// SMASH_PROFILE resolution. ctest runs this binary several times with
// different environments (tests/CMakeLists.txt) and passes the expected
// effective values on the command line:
//   test_profile <cold_floor_ticks> <cold_overridden 0|1> <roi_threshold>
//                <prompt_trim_mb or -> <compressor_idle or ->
// Checks that a profile supplies its values, that an explicit expert variable
// beats the profile, and that balanced / unset changes nothing.
#include "smash/config.h"
#include "compress/compression_roi.h"
#include <cstdio>
#include <cstring>

using namespace smash;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool sameOpt(const char* got, const char* want) {
    if (std::strcmp(want, "-") == 0) return got == nullptr;
    return got && std::strcmp(got, want) == 0;
}

int main(int argc, char** argv) {
    if (argc != 6) { std::fprintf(stderr, "usage: see source\n"); return 2; }

    // Table invariants (independent of the environment).
    CHECK(profileDefault(SmashProfile::Balanced, "SMASH_COLD_TIMEOUT_SEC") == nullptr);
    CHECK(profileDefault(SmashProfile::Balanced, "SMASH_ROI_THRESHOLD") == nullptr);
    CHECK(profileDefault(SmashProfile::Latency, "SMASH_NOT_A_KNOB") == nullptr);
    CHECK(parseSmashProfile("latency") == SmashProfile::Latency);
    CHECK(parseSmashProfile("memory") == SmashProfile::Memory);
    CHECK(parseSmashProfile("balanced") == SmashProfile::Balanced);
    CHECK(parseSmashProfile("bogus") == SmashProfile::Balanced);
    CHECK(parseSmashProfile(nullptr) == SmashProfile::Balanced);

    // Effective values for this process's environment.
    auto& cfg = ROIConfig::instance();
    cfg.init(nullptr);
    const unsigned want_floor = static_cast<unsigned>(std::atoi(argv[1]));
    const bool want_over = std::atoi(argv[2]) != 0;
    const unsigned want_roi = static_cast<unsigned>(std::atoi(argv[3]));
    std::printf("profile=%s floor=%u overridden=%d roi=%u trim=%s idle=%s cold_ticks=%d\n",
                smashProfileName(getSmashProfile()), cfg.cold_ticks_floor,
                cfg.cold_ticks_overridden, cfg.roi_threshold,
                envOrProfile("SMASH_PROMPT_TRIM_MB") ? envOrProfile("SMASH_PROMPT_TRIM_MB") : "-",
                envOrProfile("SMASH_COMPRESSOR_IDLE") ? envOrProfile("SMASH_COMPRESSOR_IDLE") : "-",
                getColdTicks());
    CHECK(cfg.cold_ticks_floor == want_floor);
    CHECK(cfg.cold_ticks_overridden == want_over);
    CHECK(cfg.roi_threshold == want_roi);
    CHECK(sameOpt(envOrProfile("SMASH_PROMPT_TRIM_MB"), argv[4]));
    CHECK(sameOpt(envOrProfile("SMASH_COMPRESSOR_IDLE"), argv[5]));
    // getColdTicks (cooling-close, telemetry) agrees with the ROI floor
    // unless SMASH_COLD_TICKS is what set it (it only reads the timeout).
    if (!std::getenv("SMASH_COLD_TICKS")) CHECK(getColdTicks() == static_cast<int>(want_floor));

    if (failures) return 1;
    std::printf("PASS\n");
    return 0;
}
