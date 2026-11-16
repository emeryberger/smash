// tools/roi_probe.cpp - dev tool: run smash's startup calibration and print, for
// several compression ratios, the cold tick at which AlgoProfile::computeROI
// first reaches 256 / 1024 / 4096 (the reasoning in agents/env-vars.md).
// Not built by CMake. Build against an existing build tree, e.g.:
//   c++ -std=c++20 -O1 -Iinclude -Isrc -Ibuild/_deps/alloc8-src/include \
//     -Ibuild/_deps/printf-src -Ibuild/_deps/zstd-src/lib -Ibuild/_deps/lz4-src/lib \
//     tools/roi_probe.cpp build/_deps/printf-src/printf.cpp \
//     build/liblz4_static.a build/_deps/zstd-build/lib/libzstd.a -o roi_probe
#include "smash/config.h"
#include "compress/compress_engine.h"
#include "compress/compression_roi.h"
#include <cstdio>
using namespace smash;
int main() {
    CompressEngine e; e.init();
    auto& c = ROIConfig::instance(); c.init(&e);
    auto& p = c.profiles[0];
    std::printf("fast: comp hi/lo %u/%u MB/s, decomp hi/lo %u/%u MB/s\n",
                p.comp_mbs_hi, p.comp_mbs_lo, p.decomp_mbs_hi, p.decomp_mbs_lo);
    const unsigned thr[] = {256, 1024, 4096};
    const unsigned ratios[] = {64, 128, 192, 230};  // savings x/255: 25%, 50%, 75%, 90%
    for (unsigned r : ratios) {
        std::printf("savings %3u%%:", r * 100 / 255);
        for (unsigned t : thr) {
            int at = -1;
            for (int cc = 1; cc <= 255; ++cc) if (p.computeROI(cc, r) >= t) { at = cc; break; }
            std::printf("  roi>=%u at %d ticks", t, at);
        }
        std::printf("   (roi@1 tick = %u)\n", p.computeROI(1, r));
    }
}
extern "C" void _putchar(char) {}
