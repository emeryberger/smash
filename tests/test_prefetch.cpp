// Test prefetch: fault on middle page decompresses adjacent, boundary clipping
#include "vm/vm_region.h"
#include "vm/page_state.h"
#include "compress/compress_store.h"
#include "compress/compress_engine.h"
#include "compress/compressor_thread.h"
#include "core/page_map.h"
#include "core/span.h"
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

static void testPrefetchAdjacentPages() {
    // Set up all components
    constexpr size_t kTestPages = 64;
    VmRegion vm;
    bool ok = vm.init(kTestPages * kPageSize);
    CHECK(ok, "VmRegion init failed");

    PageStateTable states;
    states.init(vm.totalPages());

    PageLockTable locks;
    locks.init(vm.totalPages());

    CompressStore store;
    store.init();

    CompressEngine engine;
    engine.init();

    PageMap page_map;
    page_map.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine, &page_map);

    // Allocate a contiguous run of 8 pages (simulating a span)
    constexpr int kSpanPages = 8;
    void* span_base = vm.allocatePages(kSpanPages);
    CHECK(span_base != nullptr, "span alloc failed");

    // Create a span descriptor and register in page map
    Span* span = newSpanDescriptor();
    span->init(span_base, kSpanPages, 0);
    span->is_large = true;  // not a real slab page; prevent zeroFreeSlots from zeroing test data
    page_map.setRange(reinterpret_cast<uintptr_t>(span_base), kSpanPages, span);

    size_t base_idx = vm.pageIndex(reinterpret_cast<uintptr_t>(span_base));

    // Set all pages ACTIVE and fill with distinct patterns
    for (int p = 0; p < kSpanPages; ++p) {
        states.set(base_idx + p, PageState::ACTIVE);
        void* addr = vm.pageAddress(base_idx + p);
        memset(addr, 0x30 + p, kPageSize);  // Each page has a distinct byte
    }

    // Tick twice to compress all pages
    compressor.compressTick();
    compressor.compressTick();

    // Verify all pages are compressed
    for (int p = 0; p < kSpanPages; ++p) {
        CHECK(states.get(base_idx + p) == PageState::COMPRESSED,
              "page %d should be COMPRESSED, got %d",
              p, static_cast<int>(states.get(base_idx + p)));
    }

    // Fault on page 4 (middle of span)
    // This should decompress page 4 + prefetch pages 2,3,5,6
    size_t fault_page = base_idx + 4;
    bool handled = compressor.handleFault(
        reinterpret_cast<uintptr_t>(vm.pageAddress(fault_page)));
    CHECK(handled, "handleFault should succeed");

    // Check page 4 is ACTIVE
    CHECK(states.get(fault_page) == PageState::ACTIVE,
          "faulted page should be ACTIVE");

    // Check adjacent pages within ±kPrefetchWindow are also ACTIVE
    for (int delta = -kPrefetchWindow; delta <= kPrefetchWindow; ++delta) {
        if (delta == 0) continue;
        size_t adj = fault_page + delta;
        if (adj < base_idx || adj >= base_idx + kSpanPages) continue;
        CHECK(states.get(adj) == PageState::ACTIVE,
              "adjacent page at offset %d should be ACTIVE (prefetched), got %d",
              delta, static_cast<int>(states.get(adj)));
    }

    // Verify data integrity for all decompressed pages
    for (int delta = -kPrefetchWindow; delta <= kPrefetchWindow; ++delta) {
        size_t idx = fault_page + delta;
        if (idx < base_idx || idx >= base_idx + kSpanPages) continue;
        void* addr = vm.pageAddress(idx);
        auto* bytes = static_cast<uint8_t*>(addr);
        int page_num = static_cast<int>(idx - base_idx);
        uint8_t expected = static_cast<uint8_t>(0x30 + page_num);
        bool data_ok = true;
        for (size_t i = 0; i < kPageSize; ++i) {
            if (bytes[i] != expected) {
                data_ok = false;
                break;
            }
        }
        CHECK(data_ok, "data mismatch on page %d (offset %d)", page_num, delta);
    }

    // Pages outside prefetch window should still be COMPRESSED
    // Pages 0 and 1 should still be compressed (fault_page=4, window=2 → min prefetched=2)
    CHECK(states.get(base_idx + 0) == PageState::COMPRESSED,
          "page 0 should still be COMPRESSED");
    CHECK(states.get(base_idx + 1) == PageState::COMPRESSED,
          "page 1 should still be COMPRESSED");
    // Page 7 should still be compressed (fault_page=4, window=2 → max prefetched=6)
    CHECK(states.get(base_idx + 7) == PageState::COMPRESSED,
          "page 7 should still be COMPRESSED");
}

static void testPrefetchBoundaryClipping() {
    // Test that prefetch respects span boundaries
    constexpr size_t kTestPages = 64;
    VmRegion vm;
    vm.init(kTestPages * kPageSize);

    PageStateTable states;
    states.init(vm.totalPages());

    PageLockTable locks;
    locks.init(vm.totalPages());

    CompressStore store;
    store.init();

    CompressEngine engine;
    engine.init();

    PageMap page_map;
    page_map.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine, &page_map);

    // Allocate two adjacent spans
    void* span1_base = vm.allocatePages(4);
    void* span2_base = vm.allocatePages(4);
    CHECK(span1_base && span2_base, "span alloc failed");

    Span* span1 = newSpanDescriptor();
    span1->init(span1_base, 4, 0);
    span1->is_large = true;
    page_map.setRange(reinterpret_cast<uintptr_t>(span1_base), 4, span1);

    Span* span2 = newSpanDescriptor();
    span2->init(span2_base, 4, 1);
    span2->is_large = true;
    page_map.setRange(reinterpret_cast<uintptr_t>(span2_base), 4, span2);

    size_t s1_base = vm.pageIndex(reinterpret_cast<uintptr_t>(span1_base));
    size_t s2_base = vm.pageIndex(reinterpret_cast<uintptr_t>(span2_base));

    // Fill and compress all pages in both spans
    for (int p = 0; p < 4; ++p) {
        states.set(s1_base + p, PageState::ACTIVE);
        memset(vm.pageAddress(s1_base + p), 0xA0 + p, kPageSize);
        states.set(s2_base + p, PageState::ACTIVE);
        memset(vm.pageAddress(s2_base + p), 0xB0 + p, kPageSize);
    }

    compressor.compressTick();
    compressor.compressTick();

    // Fault on last page of span1 (page index s1_base+3)
    // Prefetch should NOT cross into span2
    bool handled = compressor.handleFault(
        reinterpret_cast<uintptr_t>(vm.pageAddress(s1_base + 3)));
    CHECK(handled, "handleFault should succeed");

    // Span2 pages should still be COMPRESSED (prefetch didn't cross span boundary)
    for (int p = 0; p < 4; ++p) {
        CHECK(states.get(s2_base + p) == PageState::COMPRESSED,
              "span2 page %d should still be COMPRESSED (prefetch shouldn't cross spans), got %d",
              p, static_cast<int>(states.get(s2_base + p)));
    }

    // Verify span1 faulted page data
    auto* bytes = static_cast<uint8_t*>(vm.pageAddress(s1_base + 3));
    bool data_ok = true;
    for (size_t i = 0; i < kPageSize; ++i) {
        if (bytes[i] != 0xA3) { data_ok = false; break; }
    }
    CHECK(data_ok, "span1 page 3 data mismatch");
}

int main() {
    testPrefetchAdjacentPages();
    testPrefetchBoundaryClipping();

    if (failures == 0) {
        fprintf(stderr, "prefetch: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "prefetch: %d failures\n", failures);
        return 1;
    }
}
