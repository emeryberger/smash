// WKdm - Word-Key Dictionary Matching compression
//
// Userspace implementation of the algorithm used by macOS VM compressor.
// Based on the published algorithm by Wilson & Kaplan (1997) and the
// XNU kernel header documentation.
//
// Each 32-bit word in the input is classified as one of four types:
//   00 = zero word          (0 bits of data)
//   01 = exact dict match   (4-bit dictionary index)
//   10 = partial match      (4-bit index + 10-bit low bits)
//   11 = miss               (full 32-bit word, inserted into dict)
//
// Dictionary: 16-entry direct-mapped, keyed on high 22 bits of each word.
// Output layout: [header(4 words)] [tags(64 words)] [full words] [queue positions] [low bits]
//
// This implementation handles 16KB pages (4096 words on 32-bit word size).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace smash {

class WKdm {
    static constexpr int kDictSize = 16;
    static constexpr int kDictBits = 4;
    static constexpr int kLowBits = 10;
    static constexpr int kLowMask = (1 << kLowBits) - 1;
    static constexpr int kHighBits = 22;
    static constexpr uint32_t kHighMask = ~static_cast<uint32_t>(kLowMask);

    static int dictIndex(uint32_t word) {
        // Direct-mapped: hash high 22 bits to 4-bit index
        uint32_t high = word >> kLowBits;
        return static_cast<int>((high ^ (high >> 6)) & (kDictSize - 1));
    }

public:
    // Compress src (page_size bytes) into dst.
    // Returns compressed size in bytes, or 0 if incompressible.
    // dst must have room for at least page_size bytes.
    // scratch must be at least page_size bytes.
    static size_t compress(const void* src, void* dst, size_t page_size,
                           size_t dst_capacity, void* scratch) {
        const auto* words = static_cast<const uint32_t*>(src);
        auto* out = static_cast<uint32_t*>(dst);
        size_t num_words = page_size / sizeof(uint32_t);

        // Tags area: 2 bits per word, packed 16 per uint32_t
        size_t num_tag_words = (num_words + 15) / 16;

        // Reserve header (4 words) + tags
        size_t header_and_tags = 4 + num_tag_words;
        auto* tags = out + 4;

        // Clear tags
        __builtin_memset(tags, 0, num_tag_words * sizeof(uint32_t));

        // Output pointers for the three variable-size areas
        auto* full_words = out + header_and_tags;
        auto* full_ptr = full_words;

        // Queue positions: 4-bit indices packed 8 per word
        // We'll write to scratch first, then copy
        auto* q_scratch = static_cast<uint32_t*>(scratch);
        size_t q_max_words = num_words / 8 + 1;
        __builtin_memset(q_scratch, 0, q_max_words * sizeof(uint32_t));
        size_t q_count = 0;

        // Low bits: 10-bit values packed 3 per word
        auto* low_scratch = q_scratch + q_max_words;
        size_t low_max_words = num_words / 3 + 1;
        __builtin_memset(low_scratch, 0, low_max_words * sizeof(uint32_t));
        size_t low_count = 0;

        uint32_t dict[kDictSize] = {};

        for (size_t i = 0; i < num_words; ++i) {
            uint32_t w = words[i];
            size_t tag_word = i / 16;
            int tag_shift = static_cast<int>((i % 16) * 2);
            uint32_t tag;

            if (w == 0) {
                tag = 0; // zero
            } else {
                int idx = dictIndex(w);
                if (dict[idx] == w) {
                    // Exact match
                    tag = 1;
                    // Store 4-bit index
                    q_scratch[q_count / 8] |=
                        (static_cast<uint32_t>(idx) << ((q_count % 8) * 4));
                    q_count++;
                } else if ((dict[idx] & kHighMask) == (w & kHighMask)) {
                    // Partial match (high bits match)
                    tag = 2;
                    dict[idx] = w;
                    // Store 4-bit index
                    q_scratch[q_count / 8] |=
                        (static_cast<uint32_t>(idx) << ((q_count % 8) * 4));
                    q_count++;
                    // Store 10-bit low bits
                    low_scratch[low_count / 3] |=
                        (static_cast<uint32_t>(w & kLowMask) << ((low_count % 3) * 10));
                    low_count++;
                } else {
                    // Miss
                    tag = 3;
                    dict[idx] = w;
                    *full_ptr++ = w;
                }
            }
            tags[tag_word] |= (tag << tag_shift);
        }

        // Pack queue positions after full words
        size_t q_words_count = (q_count + 7) / 8;
        auto* queue_start = full_ptr;
        __builtin_memcpy(queue_start, q_scratch, q_words_count * sizeof(uint32_t));
        auto* queue_end = queue_start + q_words_count;

        // Pack low bits after queue positions
        size_t low_words_count = (low_count + 2) / 3;
        auto* low_start = queue_end;
        __builtin_memcpy(low_start, low_scratch, low_words_count * sizeof(uint32_t));
        auto* low_end = low_start + low_words_count;

        // Fill header
        out[0] = 0x574B4431; // "WKD1" magic
        out[1] = static_cast<uint32_t>(queue_start - out);  // queue offset (words)
        out[2] = static_cast<uint32_t>(low_start - out);    // low bits offset (words)
        out[3] = static_cast<uint32_t>(low_end - out);      // end offset (words)

        size_t compressed_bytes = static_cast<size_t>(low_end - out) * sizeof(uint32_t);
        if (compressed_bytes >= page_size || compressed_bytes > dst_capacity) {
            return 0; // incompressible
        }
        return compressed_bytes;
    }

    // Decompress src into dst (page_size bytes).
    // Returns decompressed size, or 0 on failure.
    static size_t decompress(const void* src, void* dst, size_t src_size,
                             size_t page_size) {
        const auto* in = static_cast<const uint32_t*>(src);
        auto* words = static_cast<uint32_t*>(dst);
        size_t num_words = page_size / sizeof(uint32_t);

        if (src_size < 4 * sizeof(uint32_t)) return 0;
        if (in[0] != 0x574B4431) return 0; // magic check

        uint32_t q_offset = in[1];
        uint32_t low_offset = in[2];
        uint32_t end_offset = in[3];

        if (end_offset * sizeof(uint32_t) > src_size) return 0;

        const auto* tags = in + 4;
        size_t num_tag_words = (num_words + 15) / 16;
        const auto* full_words = in + 4 + num_tag_words;
        const auto* queue_pos = in + q_offset;
        const auto* low_bits = in + low_offset;

        size_t full_idx = 0;
        size_t q_idx = 0;
        size_t low_idx = 0;

        uint32_t dict[kDictSize] = {};

        for (size_t i = 0; i < num_words; ++i) {
            size_t tag_word = i / 16;
            int tag_shift = static_cast<int>((i % 16) * 2);
            uint32_t tag = (tags[tag_word] >> tag_shift) & 3;

            switch (tag) {
            case 0: // zero
                words[i] = 0;
                break;
            case 1: { // exact match
                uint32_t idx = (queue_pos[q_idx / 8] >> ((q_idx % 8) * 4)) & 0xF;
                q_idx++;
                words[i] = dict[idx];
                break;
            }
            case 2: { // partial match
                uint32_t idx = (queue_pos[q_idx / 8] >> ((q_idx % 8) * 4)) & 0xF;
                q_idx++;
                uint32_t low = (low_bits[low_idx / 3] >> ((low_idx % 3) * 10)) & kLowMask;
                low_idx++;
                uint32_t w = (dict[idx] & kHighMask) | low;
                dict[idx] = w;
                words[i] = w;
                break;
            }
            case 3: { // miss
                uint32_t w = full_words[full_idx++];
                int idx = dictIndex(w);
                dict[idx] = w;
                words[i] = w;
                break;
            }
            }
        }

        return page_size;
    }
};

} // namespace smash
