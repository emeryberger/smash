// Test end-to-end compression: compress cold pages, decompress on fault
#include "vm/vm_region.h"
#include "vm/page_state.h"
#include "compress/compress_store.h"
#include "compress/compress_engine.h"
#include "compress/compressor_thread.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace smash;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

static void testCompressEngine() {
    CompressEngine engine;
    engine.init();

    // Compress a buffer of repeated bytes (highly compressible)
    auto* src = static_cast<char*>(
        BootstrapAlloc::instance().allocate(kPageSize, kPageSize));
    memset(src, 0x42, kPageSize);

    size_t max_comp = CompressEngine::maxCompressedSize(kPageSize);
    auto* dst = static_cast<char*>(
        BootstrapAlloc::instance().allocate(max_comp, 16));

    size_t comp_size = engine.compress(src, dst, kPageSize, max_comp);
    CHECK(comp_size > 0, "compression failed");
    CHECK(comp_size < kPageSize, "compressed size %zu >= original %zu", comp_size, kPageSize);

    // Decompress and verify
    auto* restored = static_cast<char*>(
        BootstrapAlloc::instance().allocate(kPageSize, kPageSize));
    size_t decomp_size = engine.decompress(dst, restored, comp_size, kPageSize);
    CHECK(decomp_size == kPageSize, "decompressed size %zu != %zu", decomp_size, kPageSize);
    CHECK(memcmp(src, restored, kPageSize) == 0, "decompressed data doesn't match original");
}

static void testCompressStore() {
    CompressStore store;
    store.init();

    // Store some data
    char data[256];
    memset(data, 0xBB, sizeof(data));

    size_t alloc_size = 0;
    void* stored = store.store(data, sizeof(data), &alloc_size);
    CHECK(stored != nullptr, "store failed");
    CHECK(alloc_size >= sizeof(data), "alloc_size %zu < data size %zu", alloc_size, sizeof(data));
    CHECK(memcmp(stored, data, sizeof(data)) == 0, "stored data doesn't match");

    // Release
    store.release(stored, alloc_size);

    // Store again (should reuse from free list)
    void* stored2 = store.store(data, sizeof(data), &alloc_size);
    CHECK(stored2 != nullptr, "second store failed");
    store.release(stored2, alloc_size);
}

static void testCompressorRoundtrip() {
    // Set up all components
    VmRegion vm;
    bool ok = vm.init(64 * kPageSize);
    CHECK(ok, "VmRegion init failed");

    PageStateTable states;
    states.init(vm.totalPages());

    PageLockTable locks;
    locks.init(vm.totalPages());

    CompressStore store;
    store.init();

    CompressEngine engine;
    engine.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine);

    // Allocate a page and fill with compressible data
    void* page = vm.allocatePages(1);
    CHECK(page != nullptr, "page alloc failed");

    size_t page_idx = vm.pageIndex(reinterpret_cast<uintptr_t>(page));
    states.set(page_idx, PageState::ACTIVE);

    // Fill with a repeating pattern (highly compressible)
    memset(page, 0x42, kPageSize);

    // Tick 1: cold_count goes to 1, page becomes ACTIVE_MONITORING (PROT_READ)
    compressor.compressTick();
    CHECK(states.get(page_idx) == PageState::ACTIVE_MONITORING,
          "after tick 1: expected ACTIVE_MONITORING, got %d",
          static_cast<int>(states.get(page_idx)));

    // Tick 2: cold_count reaches kColdTicks, two-level monitoring escalates
    // to PROT_NONE (deep monitoring) to detect reads as well as writes
    compressor.compressTick();

    // Tick 3: survived deep monitoring, truly cold — compress
    compressor.compressTick();
    CHECK(states.get(page_idx) == PageState::COMPRESSED || states.get(page_idx) == PageState::COMPRESSED_SHADOW,
          "after tick 3: expected COMPRESSED, got %d",
          static_cast<int>(states.get(page_idx)));

    // Simulate a fault: decompress the page
    bool handled = compressor.handleFault(reinterpret_cast<uintptr_t>(page));
    CHECK(handled, "handleFault should return true for compressed page");
    CHECK(states.get(page_idx) == PageState::ACTIVE,
          "after fault: expected ACTIVE, got %d",
          static_cast<int>(states.get(page_idx)));

    // Verify data integrity after decompression
    auto* bytes = static_cast<uint8_t*>(page);
    bool data_ok = true;
    for (size_t i = 0; i < kPageSize; ++i) {
        if (bytes[i] != 0x42) {
            data_ok = false;
            fprintf(stderr, "FAIL: data mismatch at offset %zu: got 0x%02x expected 0x42\n",
                    i, bytes[i]);
            break;
        }
    }
    CHECK(data_ok, "decompressed data doesn't match original");
}

static void testIncompressibleData() {
    VmRegion vm;
    vm.init(64 * kPageSize);

    PageStateTable states;
    states.init(vm.totalPages());

    PageLockTable locks;
    locks.init(vm.totalPages());

    CompressStore store;
    store.init();

    CompressEngine engine;
    engine.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine);

    // Allocate a page and fill with pseudo-random data (incompressible)
    void* page = vm.allocatePages(1);
    CHECK(page != nullptr, "page alloc failed");

    size_t page_idx = vm.pageIndex(reinterpret_cast<uintptr_t>(page));
    states.set(page_idx, PageState::ACTIVE);

    // Fill with pseudo-random bytes
    auto* bytes = static_cast<uint8_t*>(page);
    uint32_t seed = 12345;
    for (size_t i = 0; i < kPageSize; ++i) {
        seed = seed * 1103515245 + 12345;
        bytes[i] = static_cast<uint8_t>(seed >> 16);
    }

    // Tick three times to attempt compression (two-level monitoring requires 3)
    compressor.compressTick();
    compressor.compressTick();
    compressor.compressTick();

    // With random data, compression ratio likely exceeds kMinCompressRatio,
    // so the page should NOT be compressed. After failed compression, tick's
    // Phase 3 sets remaining ACTIVE pages to ACTIVE_MONITORING.
    PageState st = states.get(page_idx);
    CHECK(st != PageState::COMPRESSED,
          "incompressible page should NOT be COMPRESSED, got %d", static_cast<int>(st));

    // Verify data is still intact
    seed = 12345;
    bool data_ok = true;
    for (size_t i = 0; i < kPageSize; ++i) {
        seed = seed * 1103515245 + 12345;
        uint8_t expected = static_cast<uint8_t>(seed >> 16);
        if (bytes[i] != expected) {
            data_ok = false;
            break;
        }
    }
    CHECK(data_ok, "data corrupted after failed compression attempt");
}

static void testAccessTracking() {
    VmRegion vm;
    vm.init(64 * kPageSize);

    PageStateTable states;
    states.init(vm.totalPages());

    PageLockTable locks;
    locks.init(vm.totalPages());

    CompressStore store;
    store.init();

    CompressEngine engine;
    engine.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine);

    // Allocate a page
    void* page = vm.allocatePages(1);
    CHECK(page != nullptr, "page alloc failed");

    size_t page_idx = vm.pageIndex(reinterpret_cast<uintptr_t>(page));
    states.set(page_idx, PageState::ACTIVE);
    memset(page, 0x42, kPageSize);

    // Tick once: page becomes ACTIVE_MONITORING
    compressor.compressTick();
    CHECK(states.get(page_idx) == PageState::ACTIVE_MONITORING,
          "expected ACTIVE_MONITORING");

    // Simulate a write fault (user writes to monitored page)
    bool handled = compressor.handleFault(reinterpret_cast<uintptr_t>(page));
    CHECK(handled, "handleFault for ACTIVE_MONITORING should succeed");
    CHECK(states.get(page_idx) == PageState::ACTIVE,
          "after write fault: expected ACTIVE, got %d",
          static_cast<int>(states.get(page_idx)));

    // Now tick four more times — the fault set accessed_=true, so:
    // Tick 1: accessed_=true → cold_count stays 0, cleared. Phase 3: → ACTIVE_MONITORING
    // Tick 2: accessed_=false → cold_count=1. Phase 2: skip (< kColdTicks)
    // Tick 3: accessed_=false → cold_count=2 (== kColdTicks). Phase 2: escalate to deep monitoring
    // Tick 4: accessed_=false → cold_count=3 (> kColdTicks). Phase 2: compress!
    compressor.compressTick();
    CHECK(states.get(page_idx) == PageState::ACTIVE_MONITORING,
          "after tick 1: expected ACTIVE_MONITORING");

    compressor.compressTick();
    // cold_count=1, not yet compressed
    CHECK(states.get(page_idx) != PageState::COMPRESSED,
          "after tick 2: should not be COMPRESSED yet");

    compressor.compressTick();
    // cold_count=2, deep monitoring escalation — not yet compressed
    CHECK(states.get(page_idx) != PageState::COMPRESSED,
          "after tick 3: should not be COMPRESSED yet (deep monitoring)");

    compressor.compressTick();
    // cold_count=3, survived deep monitoring — should now be compressed
    CHECK(states.get(page_idx) == PageState::COMPRESSED || states.get(page_idx) == PageState::COMPRESSED_SHADOW,
          "after 4 ticks without access: expected COMPRESSED, got %d",
          static_cast<int>(states.get(page_idx)));

    // Decompress and verify
    handled = compressor.handleFault(reinterpret_cast<uintptr_t>(page));
    CHECK(handled, "handleFault for COMPRESSED should succeed");

    auto* bytes = static_cast<uint8_t*>(page);
    bool ok = true;
    for (size_t i = 0; i < kPageSize; ++i) {
        if (bytes[i] != 0x42) { ok = false; break; }
    }
    CHECK(ok, "data mismatch after access tracking round-trip");
}

static void testReleaseCompressedPages() {
    VmRegion vm;
    vm.init(64 * kPageSize);

    PageStateTable states;
    states.init(vm.totalPages());

    PageLockTable locks;
    locks.init(vm.totalPages());

    CompressStore store;
    store.init();

    CompressEngine engine;
    engine.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine);

    // Allocate, compress, then release
    void* page = vm.allocatePages(1);
    CHECK(page != nullptr, "page alloc failed");

    size_t page_idx = vm.pageIndex(reinterpret_cast<uintptr_t>(page));
    states.set(page_idx, PageState::ACTIVE);
    memset(page, 0x42, kPageSize);

    // Compress it (3 ticks: monitoring + deep monitoring + compress)
    compressor.compressTick();
    compressor.compressTick();
    compressor.compressTick();
    CHECK(states.get(page_idx) == PageState::COMPRESSED || states.get(page_idx) == PageState::COMPRESSED_SHADOW,
          "expected COMPRESSED");

    // Release the compressed page (simulates span deallocation)
    compressor.releaseCompressedPages(page_idx, 1);
    CHECK(states.get(page_idx) == PageState::EMPTY,
          "after release: expected EMPTY, got %d",
          static_cast<int>(states.get(page_idx)));
}

// Partial reclamation: after most blobs are released, trim() returns the
// pages with no live blob, and every survivor and later reuse of the free
// lists (including slots that extend onto trimmed pages, and split
// remainders) stays byte-exact.
static void fillBlob(char* p, size_t size, uint32_t id) {
    for (size_t j = 0; j < size; ++j) p[j] = static_cast<char>((id * 131u + j * 7u) & 0xff);
}
static bool blobOk(const char* p, size_t size, uint32_t id) {
    for (size_t j = 0; j < size; ++j)
        if (p[j] != static_cast<char>((id * 131u + j * 7u) & 0xff)) return false;
    return true;
}

static void testCompressStoreTrim() {
    CompressStore store;
    store.init();
    constexpr uint32_t kN = 20000;
    struct B { void* p; size_t req, alloc; uint32_t id; bool live; };
    auto* b = static_cast<B*>(BootstrapAlloc::instance().allocate(sizeof(B) * kN * 2, 16));
    char tmp[1400];
    uint32_t next_id = 1;
    auto put = [&](B& x, size_t page_idx) {
        x.req = 48 + (x.id * 37u) % 1200;  // 48..1247 B: many slots straddle pages
        fillBlob(tmp, x.req, x.id);
        x.p = store.store(tmp, x.req, &x.alloc, page_idx);
        x.live = x.p != nullptr;
        CHECK(x.p, "store failed id=%u", x.id);
    };
    for (uint32_t i = 0; i < kN; ++i) { b[i].id = next_id++; put(b[i], i); }

    // Mass release: keep 1 in 10, in runs of 400 (blobs are sharded by
    // page_idx % 8, so a run of 400 is 50 consecutive blobs per shard). With
    // 16 KiB pages (Apple silicon) scattered survivors would pin every page.
    for (uint32_t i = 0; i < kN; ++i)
        if ((i / 400) % 10) { store.release(b[i].p, b[i].alloc, i); b[i].live = false; }
    size_t t1 = store.trim();
    CHECK(t1 > 0, "trim returned no pages after releasing 90%% of blobs");
    CHECK(store.trim() == 0, "second trim with nothing released should be a no-op");
    for (uint32_t i = 0; i < kN; ++i)
        if (b[i].live) CHECK(blobOk(static_cast<char*>(b[i].p), b[i].req, b[i].id),
                             "survivor id=%u corrupted after trim", b[i].id);

    // Refill through the (trimmed) free lists, then check everything.
    for (uint32_t i = kN; i < 2 * kN - 2000; ++i) { b[i].id = next_id++; put(b[i], i - kN); }
    for (uint32_t i = 0; i < 2 * kN - 2000; ++i)
        if (b[i].live) CHECK(blobOk(static_cast<char*>(b[i].p), b[i].req, b[i].id),
                             "blob id=%u corrupted after refill", b[i].id);

    // Second cycle: release a different subset, trim, verify.
    for (uint32_t i = 0; i < 2 * kN - 2000; ++i)
        if (b[i].live && ((i / 400) % 3) != 0) {
            store.release(b[i].p, b[i].alloc, i >= kN ? i - kN : i); b[i].live = false;
        }
    size_t t2 = store.trim();
    CHECK(t2 > 0, "second trim returned no pages");
    for (uint32_t i = 0; i < 2 * kN - 2000; ++i)
        if (b[i].live) CHECK(blobOk(static_cast<char*>(b[i].p), b[i].req, b[i].id),
                             "blob id=%u corrupted after second trim", b[i].id);
    CHECK(store.trimmedPages() == t1 + t2, "trimmedPages %zu != %zu", store.trimmedPages(), t1 + t2);

    // Release everything (regions reset) and make sure the store still works.
    for (uint32_t i = 0; i < 2 * kN - 2000; ++i)
        if (b[i].live) { store.release(b[i].p, b[i].alloc, i >= kN ? i - kN : i); b[i].live = false; }
    store.trim();
    B z{nullptr, 0, 0, next_id++, false};
    put(z, 7);
    CHECK(z.p && blobOk(static_cast<char*>(z.p), z.req, z.id), "store after full drain failed");
    fprintf(stderr, "compress store trim: %zu + %zu pages returned\n", t1, t2);
}

int main() {
    testCompressEngine();
    testCompressStore();
    testCompressStoreTrim();
    testCompressorRoundtrip();
    testIncompressibleData();
    testAccessTracking();
    testReleaseCompressedPages();

    if (failures == 0) {
        fprintf(stderr, "compression: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "compression: %d failures\n", failures);
        return 1;
    }
}
