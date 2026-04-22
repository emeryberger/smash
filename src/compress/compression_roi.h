// smash/src/compress/compression_roi.h - ROI-based compression decision model
//
// Replaces fixed cold-tick thresholds with a cost/benefit model that considers
// compression ratio, CPU cost (compress + decompress), and cold duration.
// Auto-calibrates throughput at startup; all parameters configurable via env vars.
//
// Adding a new algorithm: add an AlgoProfile entry in ROIConfig::init().
// The ROI selection loop handles any number of profiles generically.
#pragma once

#include "smash/config.h"
#include "compress_engine.h"
#include "../core/bootstrap_alloc.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <algorithm>

#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace smash {

// --- Helper: read environment variables with defaults ---

inline int roiEnvInt(const char* name, int default_val) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : default_val;
}

inline double roiEnvDouble(const char* name, double default_val) {
    const char* v = std::getenv(name);
    return v ? std::strtod(v, nullptr) : default_val;
}

// --- Algorithm throughput/cost profile ---

struct AlgoProfile {
    CompressAlgo algo = CompressAlgo::NONE;
    uint16_t comp_mbs_hi = 0;     // compress throughput (MB/s) at high compressibility
    uint16_t comp_mbs_lo = 0;     // compress throughput (MB/s) at low compressibility
    uint16_t decomp_mbs_hi = 0;   // decompress throughput at high compressibility
    uint16_t decomp_mbs_lo = 0;   // decompress throughput at low compressibility
    uint8_t  min_cold_ticks = 0;  // minimum cold ticks before considering this algo
    uint8_t  ratio_scale_num = 1; // expected ratio vs LZ4 baseline (num/den)
    uint8_t  ratio_scale_den = 1;

    // Linear interpolation: better compression ratio -> higher throughput.
    // ratio_255: savings ratio (255 = 100% saved, 0 = nothing saved).
    uint32_t compThroughput(uint16_t ratio_255) const {
        uint32_t tp = static_cast<uint32_t>(comp_mbs_lo) +
            (static_cast<uint32_t>(comp_mbs_hi) - comp_mbs_lo) * ratio_255 / 255;
        return tp > 0 ? tp : 1;
    }

    uint32_t decompThroughput(uint16_t ratio_255) const {
        uint32_t tp = static_cast<uint32_t>(decomp_mbs_lo) +
            (static_cast<uint32_t>(decomp_mbs_hi) - decomp_mbs_lo) * ratio_255 / 255;
        return tp > 0 ? tp : 1;
    }

    // ROI = benefit / cost (byte-seconds of RSS saved per microsecond of CPU).
    //
    // benefit = kPageSize * (effective_ratio/255) * cold_count   [byte-seconds]
    // cost    = kPageSize/comp_tp + kPageSize/decomp_tp          [microseconds]
    //
    // Since both cost terms increase with R (worse ratio -> slower throughput)
    // and benefit decreases with R, ROI is steeply ratio-sensitive.
    uint32_t computeROI(uint8_t cold_count, uint16_t ratio_255) const {
        // Scale ratio for algorithms that achieve better ratios than LZ4
        uint16_t effective_ratio = ratio_255;
        if (ratio_scale_num != ratio_scale_den && ratio_scale_den > 0) {
            uint32_t scaled = static_cast<uint32_t>(ratio_255) *
                              ratio_scale_num / ratio_scale_den;
            effective_ratio = static_cast<uint16_t>(scaled > 255 ? 255 : scaled);
        }

        uint64_t benefit = static_cast<uint64_t>(kPageSize) *
                           effective_ratio * cold_count / 255;
        if (benefit == 0) return 0;

        // Use x256 scaling for sub-microsecond precision
        uint32_t comp_tp = compThroughput(effective_ratio);
        uint32_t decomp_tp = decompThroughput(effective_ratio);
        uint32_t cost_x256 =
            static_cast<uint32_t>(kPageSize) * 256 / comp_tp +
            static_cast<uint32_t>(kPageSize) * 256 / decomp_tp;
        if (cost_x256 == 0) return UINT32_MAX;

        uint64_t roi = benefit * 256 / cost_x256;
        return roi > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(roi);
    }
};

// --- ROI configuration singleton ---

struct ROIConfig {
    uint32_t roi_threshold = kRoiThresholdDefault;
    double min_compress_ratio = kMinCompressRatio;
    uint32_t cold_ticks_floor = kColdTicksDefault;
    uint32_t very_cold_ticks = kVeryColdTicks;

    AlgoProfile profiles[4];
    int num_profiles = 0;

    bool cold_ticks_overridden = false;
    bool very_cold_ticks_overridden = false;
    bool initialized = false;

    static ROIConfig& instance() {
        static ROIConfig cfg;
        return cfg;
    }

    void init(CompressEngine* engine) {
        if (initialized) return;
        initialized = true;

        // 1. Read policy env vars
        roi_threshold = static_cast<uint32_t>(
            roiEnvInt("SMASH_ROI_THRESHOLD", kRoiThresholdDefault));
        min_compress_ratio = roiEnvDouble("SMASH_MIN_COMPRESS_RATIO",
                                          kMinCompressRatio);

        // SMASH_COLD_TIMEOUT_SEC is the primary time-space tradeoff dial.
        // It sets the idle time (in seconds) before a page is compressed.
        // SMASH_COLD_TICKS is the lower-level override (in tick counts).
        const char* timeout_env = std::getenv("SMASH_COLD_TIMEOUT_SEC");
        const char* ct_env = std::getenv("SMASH_COLD_TICKS");
        if (timeout_env) {
            double sec = std::atof(timeout_env);
            if (sec > 0) {
                cold_ticks_floor = static_cast<uint32_t>(
                    sec * 1000.0 / kCompressIntervalMs + 0.5);
                if (cold_ticks_floor < 1) cold_ticks_floor = 1;
                cold_ticks_overridden = true;
            }
        } else if (ct_env) {
            cold_ticks_floor = static_cast<uint32_t>(std::atoi(ct_env));
            cold_ticks_overridden = true;
        }

        const char* vct_env = std::getenv("SMASH_VERY_COLD_TICKS");
        if (vct_env) {
            very_cold_ticks = static_cast<uint32_t>(std::atoi(vct_env));
            very_cold_ticks_overridden = true;
        }

        // 2. Try loading calibration file
        const char* cal_mode = std::getenv("SMASH_CALIBRATE");
        const char* cal_file = std::getenv("SMASH_CALIBRATION_FILE");
        bool mode_always = cal_mode && std::strcmp(cal_mode, "always") == 0;
        bool mode_never  = cal_mode && std::strcmp(cal_mode, "never") == 0;

        bool loaded = false;
        if (cal_file && !mode_always)
            loaded = loadCalibrationFile(cal_file);

        // 3. Auto-calibrate if needed
        bool just_calibrated = false;
        if (!loaded && !mode_never && engine) {
            calibrate(engine);
            just_calibrated = true;
        }
        if (!loaded && !just_calibrated)
            useDefaults();

        // 4. Apply per-throughput env var overrides
        applyEnvOverrides();

        // 5. Remove ZSTD profile if very_cold_ticks disabled
        if (very_cold_ticks_overridden && very_cold_ticks >= 9999) {
            int new_count = 0;
            for (int i = 0; i < num_profiles; ++i) {
                if (profiles[i].algo != CompressAlgo::ZSTD)
                    profiles[new_count++] = profiles[i];
            }
            num_profiles = new_count;
        }

        // 6. Save calibration file if requested
        if (cal_file && just_calibrated)
            saveCalibrationFile(cal_file);
    }

private:
    // Calibration data (stored for file save)
    struct CalData {
        uint16_t lz4_comp_hi = 3900, lz4_comp_lo = 500;
        uint16_t lz4_decomp_hi = 25000, lz4_decomp_lo = 3000;
        uint16_t zstd_comp_hi = 700, zstd_comp_lo = 45;
        uint16_t zstd_decomp_hi = 3800, zstd_decomp_lo = 1100;
        double lz4_ratio_zeros = 0.016, lz4_ratio_random = 1.0;
        double zstd_ratio_zeros = 0.010, zstd_ratio_random = 1.0;
    } cal_;

    void useDefaults() {
        buildProfiles();
    }

    void buildProfiles() {
        num_profiles = 0;

        // LZ4 profile
        profiles[num_profiles++] = AlgoProfile{
            CompressAlgo::LZ4,
            cal_.lz4_comp_hi, cal_.lz4_comp_lo,
            cal_.lz4_decomp_hi, cal_.lz4_decomp_lo,
            static_cast<uint8_t>(
                cold_ticks_floor > 255 ? 255 : cold_ticks_floor),
            1, 1
        };

        // ZSTD profile — compute ratio scale from calibration data
        uint8_t scale_num = 1, scale_den = 1;
        if (cal_.lz4_ratio_zeros > 0.001 && cal_.zstd_ratio_zeros > 0.001) {
            double lz4_sav = 1.0 - cal_.lz4_ratio_zeros;
            double zstd_sav = 1.0 - cal_.zstd_ratio_zeros;
            if (lz4_sav > 0.01) {
                double scale = zstd_sav / lz4_sav;
                if      (scale >= 1.5)  { scale_num = 3;  scale_den = 2; }
                else if (scale >= 1.3)  { scale_num = 4;  scale_den = 3; }
                else if (scale >= 1.1)  { scale_num = 11; scale_den = 10; }
            }
        }

        profiles[num_profiles++] = AlgoProfile{
            CompressAlgo::ZSTD,
            cal_.zstd_comp_hi, cal_.zstd_comp_lo,
            cal_.zstd_decomp_hi, cal_.zstd_decomp_lo,
            static_cast<uint8_t>(
                very_cold_ticks > 255 ? 255 : very_cold_ticks),
            scale_num, scale_den
        };
    }

    void calibrate(CompressEngine* engine) {
        // Allocate test pages from BootstrapAlloc (48 KB permanent)
        void* zeros_page = BootstrapAlloc::instance().allocate(kPageSize, kPageSize);
        void* random_page = BootstrapAlloc::instance().allocate(kPageSize, kPageSize);
        size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
        void* comp_buf = BootstrapAlloc::instance().allocate(max_comp, 16);
        void* decomp_buf = BootstrapAlloc::instance().allocate(kPageSize, kPageSize);

        if (!zeros_page || !random_page || !comp_buf || !decomp_buf) {
            useDefaults();
            return;
        }

        std::memset(zeros_page, 0, kPageSize);

        // Fill random page with splitmix64
        uint64_t rng = 0xDEADBEEFCAFE1234ULL;
        auto* rp = static_cast<uint64_t*>(random_page);
        for (size_t i = 0; i < kPageSize / 8; ++i) {
            rng ^= rng >> 30;
            rng *= 0xBF58476D1CE4E5B9ULL;
            rng ^= rng >> 27;
            rng *= 0x94D049BB133111EBULL;
            rng ^= rng >> 31;
            rp[i] = rng;
        }

        // Reuse engine's pre-allocated contexts (no extra allocation)
        void* lz4_state = engine->getLz4State();
        ZSTD_CCtx* zstd_cctx = engine->getZstdCCtx();
        ZSTD_DCtx* zstd_dctx = engine->getZstdDCtx();

        if (!lz4_state || !zstd_cctx || !zstd_dctx) {
            useDefaults();
            return;
        }

        static constexpr int kCalIters = 10;
        using Clock = std::chrono::steady_clock;

        struct TimingResult {
            double comp_us;
            double decomp_us;
            size_t comp_size;
        };

        auto benchLZ4 = [&](void* page) -> TimingResult {
            double comp_times[kCalIters], decomp_times[kCalIters];
            size_t last_comp_size = 0;

            for (int iter = 0; iter < kCalIters; ++iter) {
                auto t0 = Clock::now();
                int csz = LZ4_compress_fast_extState(
                    lz4_state,
                    static_cast<const char*>(page),
                    static_cast<char*>(comp_buf),
                    static_cast<int>(kPageSize),
                    static_cast<int>(max_comp), 1);
                auto t1 = Clock::now();
                comp_times[iter] = std::chrono::duration<double, std::micro>(
                    t1 - t0).count();
                last_comp_size = csz > 0 ? static_cast<size_t>(csz) : 0;
            }

            for (int iter = 0; iter < kCalIters; ++iter) {
                auto t0 = Clock::now();
                LZ4_decompress_safe(
                    static_cast<const char*>(comp_buf),
                    static_cast<char*>(decomp_buf),
                    static_cast<int>(last_comp_size),
                    static_cast<int>(kPageSize));
                auto t1 = Clock::now();
                decomp_times[iter] = std::chrono::duration<double, std::micro>(
                    t1 - t0).count();
            }

            std::sort(comp_times, comp_times + kCalIters);
            std::sort(decomp_times, decomp_times + kCalIters);
            return {comp_times[kCalIters / 2],
                    decomp_times[kCalIters / 2],
                    last_comp_size};
        };

        auto benchZSTD = [&](void* page) -> TimingResult {
            double comp_times[kCalIters], decomp_times[kCalIters];
            size_t last_comp_size = 0;

            for (int iter = 0; iter < kCalIters; ++iter) {
                auto t0 = Clock::now();
                size_t csz = ZSTD_compressCCtx(
                    zstd_cctx, comp_buf, max_comp,
                    page, kPageSize, kZstdDeepLevel);
                auto t1 = Clock::now();
                comp_times[iter] = std::chrono::duration<double, std::micro>(
                    t1 - t0).count();
                last_comp_size = ZSTD_isError(csz) ? 0 : csz;
            }

            for (int iter = 0; iter < kCalIters; ++iter) {
                auto t0 = Clock::now();
                ZSTD_decompressDCtx(zstd_dctx, decomp_buf, kPageSize,
                                     comp_buf, last_comp_size);
                auto t1 = Clock::now();
                decomp_times[iter] = std::chrono::duration<double, std::micro>(
                    t1 - t0).count();
            }

            std::sort(comp_times, comp_times + kCalIters);
            std::sort(decomp_times, decomp_times + kCalIters);
            return {comp_times[kCalIters / 2],
                    decomp_times[kCalIters / 2],
                    last_comp_size};
        };

        auto toMBps = [](double us) -> uint16_t {
            if (us <= 0.001) return 60000;
            double mbs = static_cast<double>(kPageSize) / us;
            return static_cast<uint16_t>(
                std::min(mbs, 60000.0));
        };

        // Benchmark LZ4
        auto lz4_zeros  = benchLZ4(zeros_page);
        auto lz4_random = benchLZ4(random_page);
        cal_.lz4_comp_hi   = toMBps(lz4_zeros.comp_us);
        cal_.lz4_comp_lo   = toMBps(lz4_random.comp_us);
        cal_.lz4_decomp_hi = toMBps(lz4_zeros.decomp_us);
        cal_.lz4_decomp_lo = toMBps(lz4_random.decomp_us);
        cal_.lz4_ratio_zeros  = lz4_zeros.comp_size > 0
            ? static_cast<double>(lz4_zeros.comp_size) / kPageSize : 1.0;
        cal_.lz4_ratio_random = lz4_random.comp_size > 0
            ? static_cast<double>(lz4_random.comp_size) / kPageSize : 1.0;

        // Benchmark ZSTD
        auto zstd_zeros  = benchZSTD(zeros_page);
        auto zstd_random = benchZSTD(random_page);
        cal_.zstd_comp_hi   = toMBps(zstd_zeros.comp_us);
        cal_.zstd_comp_lo   = toMBps(zstd_random.comp_us);
        cal_.zstd_decomp_hi = toMBps(zstd_zeros.decomp_us);
        cal_.zstd_decomp_lo = toMBps(zstd_random.decomp_us);
        cal_.zstd_ratio_zeros  = zstd_zeros.comp_size > 0
            ? static_cast<double>(zstd_zeros.comp_size) / kPageSize : 1.0;
        cal_.zstd_ratio_random = zstd_random.comp_size > 0
            ? static_cast<double>(zstd_random.comp_size) / kPageSize : 1.0;

        buildProfiles();
    }

    void applyEnvOverrides() {
        for (int i = 0; i < num_profiles; ++i) {
            auto& p = profiles[i];
            if (p.algo == CompressAlgo::LZ4) {
                const char* v;
                if ((v = std::getenv("SMASH_LZ4_COMP_MBS_HI")))
                    p.comp_mbs_hi = static_cast<uint16_t>(std::atoi(v));
                if ((v = std::getenv("SMASH_LZ4_COMP_MBS_LO")))
                    p.comp_mbs_lo = static_cast<uint16_t>(std::atoi(v));
                if ((v = std::getenv("SMASH_LZ4_DECOMP_MBS_HI")))
                    p.decomp_mbs_hi = static_cast<uint16_t>(std::atoi(v));
                if ((v = std::getenv("SMASH_LZ4_DECOMP_MBS_LO")))
                    p.decomp_mbs_lo = static_cast<uint16_t>(std::atoi(v));
            } else if (p.algo == CompressAlgo::ZSTD) {
                const char* v;
                if ((v = std::getenv("SMASH_ZSTD_COMP_MBS_HI")))
                    p.comp_mbs_hi = static_cast<uint16_t>(std::atoi(v));
                if ((v = std::getenv("SMASH_ZSTD_COMP_MBS_LO")))
                    p.comp_mbs_lo = static_cast<uint16_t>(std::atoi(v));
                if ((v = std::getenv("SMASH_ZSTD_DECOMP_MBS_HI")))
                    p.decomp_mbs_hi = static_cast<uint16_t>(std::atoi(v));
                if ((v = std::getenv("SMASH_ZSTD_DECOMP_MBS_LO")))
                    p.decomp_mbs_lo = static_cast<uint16_t>(std::atoi(v));
            }
        }
    }

    bool loadCalibrationFile(const char* path) {
        FILE* f = std::fopen(path, "r");
        if (!f) return false;

        char buf[4096];
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        if (n == 0) return false;
        buf[n] = '\0';

        auto extractUint = [&](const char* key) -> int {
            const char* p = std::strstr(buf, key);
            if (!p) return -1;
            p += std::strlen(key);
            while (*p && *p != ':') ++p;
            if (*p == ':') ++p;
            while (*p == ' ' || *p == '\t') ++p;
            return std::atoi(p);
        };
        auto extractDouble = [&](const char* key) -> double {
            const char* p = std::strstr(buf, key);
            if (!p) return -1.0;
            p += std::strlen(key);
            while (*p && *p != ':') ++p;
            if (*p == ':') ++p;
            while (*p == ' ' || *p == '\t') ++p;
            return std::strtod(p, nullptr);
        };

        int v;
        double d;
        if ((v = extractUint("\"lz4_comp_mbs_hi\""))   >= 0)
            cal_.lz4_comp_hi   = static_cast<uint16_t>(v);
        if ((v = extractUint("\"lz4_comp_mbs_lo\""))   >= 0)
            cal_.lz4_comp_lo   = static_cast<uint16_t>(v);
        if ((v = extractUint("\"lz4_decomp_mbs_hi\"")) >= 0)
            cal_.lz4_decomp_hi = static_cast<uint16_t>(v);
        if ((v = extractUint("\"lz4_decomp_mbs_lo\"")) >= 0)
            cal_.lz4_decomp_lo = static_cast<uint16_t>(v);
        if ((v = extractUint("\"zstd_comp_mbs_hi\""))  >= 0)
            cal_.zstd_comp_hi  = static_cast<uint16_t>(v);
        if ((v = extractUint("\"zstd_comp_mbs_lo\""))  >= 0)
            cal_.zstd_comp_lo  = static_cast<uint16_t>(v);
        if ((v = extractUint("\"zstd_decomp_mbs_hi\""))>= 0)
            cal_.zstd_decomp_hi= static_cast<uint16_t>(v);
        if ((v = extractUint("\"zstd_decomp_mbs_lo\""))>= 0)
            cal_.zstd_decomp_lo= static_cast<uint16_t>(v);

        if ((d = extractDouble("\"lz4_ratio_zeros\""))  >= 0)
            cal_.lz4_ratio_zeros  = d;
        if ((d = extractDouble("\"lz4_ratio_random\"")) >= 0)
            cal_.lz4_ratio_random = d;
        if ((d = extractDouble("\"zstd_ratio_zeros\"")) >= 0)
            cal_.zstd_ratio_zeros = d;
        if ((d = extractDouble("\"zstd_ratio_random\""))>= 0)
            cal_.zstd_ratio_random= d;

        buildProfiles();
        return true;
    }

    void saveCalibrationFile(const char* path) {
        FILE* f = std::fopen(path, "w");
        if (!f) return;

        std::fprintf(f, "{\n");
        std::fprintf(f, "  \"page_size\": %zu,\n",
                     static_cast<size_t>(kPageSize));
        std::fprintf(f, "  \"lz4_comp_mbs_hi\": %u,\n", cal_.lz4_comp_hi);
        std::fprintf(f, "  \"lz4_comp_mbs_lo\": %u,\n", cal_.lz4_comp_lo);
        std::fprintf(f, "  \"lz4_decomp_mbs_hi\": %u,\n", cal_.lz4_decomp_hi);
        std::fprintf(f, "  \"lz4_decomp_mbs_lo\": %u,\n", cal_.lz4_decomp_lo);
        std::fprintf(f, "  \"lz4_ratio_zeros\": %.6f,\n", cal_.lz4_ratio_zeros);
        std::fprintf(f, "  \"lz4_ratio_random\": %.6f,\n",cal_.lz4_ratio_random);
        std::fprintf(f, "  \"zstd_comp_mbs_hi\": %u,\n", cal_.zstd_comp_hi);
        std::fprintf(f, "  \"zstd_comp_mbs_lo\": %u,\n", cal_.zstd_comp_lo);
        std::fprintf(f, "  \"zstd_decomp_mbs_hi\": %u,\n",cal_.zstd_decomp_hi);
        std::fprintf(f, "  \"zstd_decomp_mbs_lo\": %u,\n",cal_.zstd_decomp_lo);
        std::fprintf(f, "  \"zstd_ratio_zeros\": %.6f,\n",cal_.zstd_ratio_zeros);
        std::fprintf(f, "  \"zstd_ratio_random\": %.6f\n",cal_.zstd_ratio_random);
        std::fprintf(f, "}\n");
        std::fclose(f);
    }
};

// --- Compression decision functions ---

namespace CompressionROI {

// Should this page be compressed?
// cold_count: ticks without access
// stats_count: SizeClassStats::count (sliding window sample count)
// stats_sum: SizeClassStats::sum (sum of ratio_255 values)
inline bool shouldCompress(uint8_t cold_count,
                            uint8_t stats_count, uint16_t stats_sum) {
    auto& cfg = ROIConfig::instance();

    // Hard floor: never compress before minimum ticks
    if (cold_count < cfg.cold_ticks_floor) return false;

    // Ablation: if cold_ticks was explicitly overridden, use fixed threshold only
    if (cfg.cold_ticks_overridden) return true;

#ifdef SMASH_ABLATION_NO_SKIP_STATS
    return true;
#endif

    // Not enough samples to estimate ratio — optimistically compress
    if (stats_count < 8) return true;

    // Compute LZ4 ROI using average ratio from sliding window
    uint16_t avg_ratio = stats_sum / stats_count;
    uint32_t roi = cfg.profiles[0].computeROI(cold_count, avg_ratio);
    return roi >= cfg.roi_threshold;
}

// Select the best compression algorithm for this page.
inline CompressAlgo selectAlgorithm(uint8_t cold_count,
                                     uint8_t stats_count, uint16_t stats_sum,
                                     CompressEngine* engine, uint8_t sc) {
    auto& cfg = ROIConfig::instance();

    // Not enough data for comparison — default to LZ4 or dict
    if (cfg.num_profiles <= 1 || stats_count < 8) {
        if (engine && engine->hasDictionary(sc) &&
            cold_count >= cfg.very_cold_ticks)
            return CompressAlgo::ZSTD_DICT;
        return CompressAlgo::LZ4;
    }

    uint16_t base_ratio = stats_sum / stats_count;

    // Evaluate all profiles, pick highest ROI
    CompressAlgo best_algo = CompressAlgo::LZ4;
    uint32_t best_roi = 0;

    for (int i = 0; i < cfg.num_profiles; ++i) {
        const auto& p = cfg.profiles[i];
        if (cold_count < p.min_cold_ticks) continue;
        uint32_t roi = p.computeROI(cold_count, base_ratio);
        if (roi > best_roi) {
            best_roi = roi;
            best_algo = p.algo;
        }
    }

    // If zstd won and we have a dict, prefer ZSTD_DICT
    if (best_algo == CompressAlgo::ZSTD &&
        engine && engine->hasDictionary(sc))
        best_algo = CompressAlgo::ZSTD_DICT;

    return best_algo;
}

} // namespace CompressionROI
} // namespace smash
