// smash_hint_cold / smash_hint_hot / smash_get_stats under interposition,
// both modes (test_public_api, test_public_api_large_only).
//
// With SMASH_COLD_TIMEOUT_SEC=30, nothing goes cold on its own during this
// test. 16 MiB of compressible data in 1 MiB chunks (managed in both modes):
//   1. control: 4 s without a hint compresses nothing;
//   2. smash_hint_cold on half the chunks: they compress within ~10 s, the
//      other half stays uncompressed;
//   3. smash_hint_hot on the hinted half: compressed pages drop back to ~0
//      at once (decompressed by the hint, not on first access);
//   4. every byte reads back intact.
// The functions are looked up with dlsym so the binary also runs (and
// skips) without libsmash preloaded.
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "smash/smash.h"

using HintFn = void (*)(void*, size_t);
using StatsFn = int (*)(SmashStats*);

static StatsFn get_stats;

static unsigned long long compressedPages() {
    SmashStats st{};
    if (get_stats(&st) != 0) return ~0ull;
    return st.compressed_pages;
}

int main() {
    auto hint_cold = reinterpret_cast<HintFn>(dlsym(RTLD_DEFAULT, "smash_hint_cold"));
    auto hint_hot = reinterpret_cast<HintFn>(dlsym(RTLD_DEFAULT, "smash_hint_hot"));
    get_stats = reinterpret_cast<StatsFn>(dlsym(RTLD_DEFAULT, "smash_get_stats"));
    if (!hint_cold || !hint_hot || !get_stats) {
        fprintf(stderr, "public_api_test: libsmash not preloaded; skipping\n");
        return 77;
    }
    constexpr size_t kChunk = 1u << 20, kN = 16;
    unsigned char* c[kN];
    for (size_t i = 0; i < kN; ++i) {
        c[i] = static_cast<unsigned char*>(malloc(kChunk));
        if (!c[i]) return 2;
        for (size_t j = 0; j < kChunk; ++j) c[i][j] = static_cast<unsigned char>((i * 7 + j / 512) & 0x3f);
    }
    SmashStats st{};
    if (get_stats(&st) != 0) { fprintf(stderr, "FAIL: smash_get_stats returned error\n"); return 1; }
    if (st.managed_bytes < kN * kChunk / 2) {
        fprintf(stderr, "FAIL: managed_bytes=%llu, expected >= %zu\n", st.managed_bytes, kN * kChunk / 2);
        return 1;
    }

    sleep(4);
    const unsigned long long control = compressedPages();
    fprintf(stderr, "public_api_test: control (no hint, 4 s) compressed=%llu\n", control);
    if (control != 0) { fprintf(stderr, "FAIL: pages compressed without a hint\n"); return 1; }

    for (size_t i = 0; i < kN / 2; ++i) hint_cold(c[i], kChunk);
    unsigned long long after_cold = 0;
    for (int t = 0; t < 100 && (after_cold = compressedPages()) == 0; ++t) usleep(100 * 1000);
    sleep(2);  // let the rest of the hinted pages land
    after_cold = compressedPages();
    fprintf(stderr, "public_api_test: after smash_hint_cold compressed=%llu\n", after_cold);
    if (after_cold == 0 || after_cold == ~0ull) { fprintf(stderr, "FAIL: hint_cold compressed nothing\n"); return 1; }
    const unsigned long long page = kChunk * kN / 2 / (after_cold ? after_cold : 1);
    (void)page;

    for (size_t i = 0; i < kN / 2; ++i) hint_hot(c[i], kChunk);
    const unsigned long long after_hot = compressedPages();
    fprintf(stderr, "public_api_test: after smash_hint_hot compressed=%llu\n", after_hot);
    if (after_hot * 10 > after_cold) { fprintf(stderr, "FAIL: hint_hot left most pages compressed\n"); return 1; }

    for (size_t i = 0; i < kN; ++i)
        for (size_t j = 0; j < kChunk; ++j)
            if (c[i][j] != static_cast<unsigned char>((i * 7 + j / 512) & 0x3f)) {
                fprintf(stderr, "FAIL: chunk %zu byte %zu\n", i, j);
                return 1;
            }
    get_stats(&st);
    fprintf(stderr, "public_api_test: PASS (compressions=%llu decompress_faults=%llu)\n",
            st.compressions, st.decompress_faults);
    return 0;
}
