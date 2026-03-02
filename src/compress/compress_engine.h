// smash/src/compress/compress_engine.h - Multi-algorithm compression engine
//
// Supports LZ4, zstd, and zstd+dictionary compression.
// Pre-allocates all state from bootstrap to avoid malloc during
// compress/decompress (critical for fault handler path).
#pragma once

#include "smash/config.h"
#include "../core/bootstrap_alloc.h"

#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zdict.h>

#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace smash {

enum class CompressAlgo : uint8_t {
    NONE      = 0,
    LZ4       = 1,
    ZSTD      = 2,
    ZSTD_DICT = 3,
};

// Per-page compressed data info.
// Algorithm is packed into top 2 bits of comp_size (30 bits for size).
struct CompressedPageInfo {
    void* data;           // Pointer to compressed data in CompressStore
    uint32_t comp_size;   // bits 0-29: compressed size, bits 30-31: algo
    uint32_t alloc_size;  // Allocated bucket size in CompressStore (for free)

    void set(void* d, size_t sz, size_t alloc_sz, CompressAlgo algo) {
        data = d;
        comp_size = (static_cast<uint32_t>(sz) & 0x3FFFFFFFu) |
                    (static_cast<uint32_t>(algo) << 30);
        alloc_size = static_cast<uint32_t>(alloc_sz);
    }

    size_t compressedSize() const { return comp_size & 0x3FFFFFFFu; }
    CompressAlgo algorithm() const { return static_cast<CompressAlgo>(comp_size >> 30); }
};

class CompressEngine {
    // LZ4 state
    void* lz4_state_ = nullptr;

    // Zstd contexts (pre-allocated from bootstrap via custom allocator)
    ZSTD_CCtx* zstd_cctx_ = nullptr;
    ZSTD_DCtx* zstd_dctx_ = nullptr;

    // Per-size-class dictionary support
    struct DictEntry {
        void* dict_buffer;    // Raw dictionary data (in bootstrap memory)
        size_t dict_size;
        ZSTD_CDict* cdict;   // Pre-built compression dictionary
        ZSTD_DDict* ddict;   // Pre-built decompression dictionary
        bool trained;
    };
    DictEntry dicts_[kNumClasses]{};

    // Custom allocator routing to BootstrapAlloc (for zstd internal allocations)
    static void* bootstrapMalloc(void* /*opaque*/, size_t size) {
        return BootstrapAlloc::instance().allocate(size, 16);
    }
    static void bootstrapFree(void* /*opaque*/, void* /*ptr*/) {
        // BootstrapAlloc never frees — this is intentional
    }

    ZSTD_customMem customMem() const {
        return { bootstrapMalloc, bootstrapFree, nullptr };
    }

public:
    void init() {
        // LZ4
        size_t state_size = static_cast<size_t>(LZ4_sizeofState());
        lz4_state_ = BootstrapAlloc::instance().allocate(state_size, 16);

        // Zstd (bootstrap-backed, no malloc)
        auto mem = customMem();
        zstd_cctx_ = ZSTD_createCCtx_advanced(mem);
        zstd_dctx_ = ZSTD_createDCtx_advanced(mem);
    }

    // Compress src into dst using the specified algorithm.
    // Returns compressed size, or 0 on failure.
    size_t compress(const void* src, void* dst, size_t src_size, size_t dst_capacity,
                    CompressAlgo algo, uint8_t size_class = 0) {
        switch (algo) {
        case CompressAlgo::LZ4: {
            int result = LZ4_compress_fast_extState(
                lz4_state_,
                static_cast<const char*>(src),
                static_cast<char*>(dst),
                static_cast<int>(src_size),
                static_cast<int>(dst_capacity),
                1);
            return (result > 0) ? static_cast<size_t>(result) : 0;
        }
        case CompressAlgo::ZSTD: {
            size_t result = ZSTD_compressCCtx(
                zstd_cctx_,
                dst, dst_capacity,
                src, src_size,
                kZstdNormalLevel);
            return ZSTD_isError(result) ? 0 : result;
        }
        case CompressAlgo::ZSTD_DICT: {
            if (size_class < kNumClasses && dicts_[size_class].cdict) {
                size_t result = ZSTD_compress_usingCDict(
                    zstd_cctx_,
                    dst, dst_capacity,
                    src, src_size,
                    dicts_[size_class].cdict);
                return ZSTD_isError(result) ? 0 : result;
            }
            // Fallback to zstd deep level without dict
            size_t result = ZSTD_compressCCtx(
                zstd_cctx_,
                dst, dst_capacity,
                src, src_size,
                kZstdDeepLevel);
            return ZSTD_isError(result) ? 0 : result;
        }
        default:
            return 0;
        }
    }

    // Decompress src into dst using the specified algorithm.
    // Returns decompressed size, or 0 on failure.
    // SAFE FOR SIGNAL HANDLER: uses pre-allocated contexts only.
    size_t decompress(const void* src, void* dst, size_t src_size, size_t dst_capacity,
                      CompressAlgo algo, uint8_t size_class = 0) {
        switch (algo) {
        case CompressAlgo::LZ4: {
            int result = LZ4_decompress_safe(
                static_cast<const char*>(src),
                static_cast<char*>(dst),
                static_cast<int>(src_size),
                static_cast<int>(dst_capacity));
            return (result > 0) ? static_cast<size_t>(result) : 0;
        }
        case CompressAlgo::ZSTD: {
            size_t result = ZSTD_decompressDCtx(
                zstd_dctx_,
                dst, dst_capacity,
                src, src_size);
            return ZSTD_isError(result) ? 0 : result;
        }
        case CompressAlgo::ZSTD_DICT: {
            if (size_class < kNumClasses && dicts_[size_class].ddict) {
                size_t result = ZSTD_decompress_usingDDict(
                    zstd_dctx_,
                    dst, dst_capacity,
                    src, src_size,
                    dicts_[size_class].ddict);
                return ZSTD_isError(result) ? 0 : result;
            }
            // Fallback: try standard zstd decompress
            size_t result = ZSTD_decompressDCtx(
                zstd_dctx_,
                dst, dst_capacity,
                src, src_size);
            return ZSTD_isError(result) ? 0 : result;
        }
        default:
            return 0;
        }
    }

    // Backward-compatible overloads for existing code using LZ4 only
    size_t compress(const void* src, void* dst, size_t src_size, size_t dst_capacity) {
        return compress(src, dst, src_size, dst_capacity, CompressAlgo::LZ4);
    }
    size_t decompress(const void* src, void* dst, size_t src_size, size_t dst_capacity) {
        return decompress(src, dst, src_size, dst_capacity, CompressAlgo::LZ4);
    }

    // Train a dictionary for a size class from collected page samples.
    // samples: concatenated sample data, sizes: per-sample sizes, num_samples: count.
    // Returns true if training succeeded.
    bool trainDictionary(uint8_t size_class, const void* samples,
                         const size_t* sizes, unsigned num_samples) {
        if (size_class >= kNumClasses) return false;
        if (dicts_[size_class].trained) return true;

        // Allocate dictionary buffer from bootstrap (~32KB)
        constexpr size_t kDictCapacity = 32 * 1024;
        void* dict_buf = BootstrapAlloc::instance().allocate(kDictCapacity, 16);
        if (!dict_buf) return false;

        size_t dict_size = ZDICT_trainFromBuffer(
            dict_buf, kDictCapacity,
            samples, sizes, num_samples);

        if (ZDICT_isError(dict_size) || dict_size == 0) {
            return false;
        }

        auto mem = customMem();

        // Create compression dictionary
        ZSTD_compressionParameters cparams = ZSTD_getCParams(
            kZstdDeepLevel, kPageSize, dict_size);
        ZSTD_CDict* cdict = ZSTD_createCDict_advanced(
            dict_buf, dict_size,
            ZSTD_dlm_byRef, ZSTD_dct_auto,
            cparams, mem);

        // Create decompression dictionary
        ZSTD_DDict* ddict = ZSTD_createDDict_advanced(
            dict_buf, dict_size,
            ZSTD_dlm_byRef, ZSTD_dct_auto,
            mem);

        if (!cdict || !ddict) return false;

        dicts_[size_class].dict_buffer = dict_buf;
        dicts_[size_class].dict_size = dict_size;
        dicts_[size_class].cdict = cdict;
        dicts_[size_class].ddict = ddict;
        dicts_[size_class].trained = true;
        return true;
    }

    bool hasDictionary(uint8_t size_class) const {
        return size_class < kNumClasses && dicts_[size_class].trained;
    }

    // Maximum compressed output size for a given input and algorithm.
    static size_t maxCompressedSize(size_t src_size, CompressAlgo algo = CompressAlgo::LZ4) {
        switch (algo) {
        case CompressAlgo::LZ4:
            return static_cast<size_t>(LZ4_compressBound(static_cast<int>(src_size)));
        case CompressAlgo::ZSTD:
        case CompressAlgo::ZSTD_DICT:
            return ZSTD_compressBound(src_size);
        default:
            return static_cast<size_t>(LZ4_compressBound(static_cast<int>(src_size)));
        }
    }

    // Maximum across all algorithms (for buffer pre-allocation)
    static size_t maxCompressedSizeAny(size_t src_size) {
        size_t lz4 = static_cast<size_t>(LZ4_compressBound(static_cast<int>(src_size)));
        size_t zstd = ZSTD_compressBound(src_size);
        return std::max(lz4, zstd);
    }
};

} // namespace smash
