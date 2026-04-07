// Test real fault handler: allocate → compress → read (triggers SIGSEGV) → verify data
#include "vm/vm_region.h"
#include "vm/page_state.h"
#include "vm/fault_handler.h"
#include "compress/compress_store.h"
#include "compress/compress_engine.h"
#include "compress/compressor_thread.h"
#include "core/page_map.h"
#include "core/span.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <signal.h>

using namespace smash;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

// Global state for fault handler callback
static CompressorThread* g_compressor = nullptr;

static bool faultCallback(uintptr_t fault_addr, void* ctx) {
    (void)ctx;
    return g_compressor->handleFault(fault_addr);
}

static void testFaultCycleBasic() {
    // Set up components
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

    PageMap page_map;
    page_map.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine, &page_map);
    g_compressor = &compressor;

    // Register fault handler
    vm::FaultHandler fault_handler;
    fault_handler.start(faultCallback, nullptr);

    // Allocate a page and fill with known data
    void* page = vm.allocatePages(1);
    CHECK(page != nullptr, "page alloc failed");

    size_t page_idx = vm.pageIndex(reinterpret_cast<uintptr_t>(page));

    // Register in page map so prefetch boundary checks work.
    // Mark as large so zeroFreeSlots skips it (this is raw test data, not a slab page).
    Span* span = newSpanDescriptor();
    span->init(page, 1, 0);
    span->is_large = true;
    page_map.setRange(reinterpret_cast<uintptr_t>(page), 1, span);

    states.set(page_idx, PageState::ACTIVE);

    // Fill with a pattern
    auto* bytes = static_cast<volatile uint8_t*>(page);
    for (size_t i = 0; i < kPageSize; ++i) {
        bytes[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Compress the page (3 ticks: monitoring + deep monitoring + compression)
    compressor.compressTick();
    compressor.compressTick();
    compressor.compressTick();
    CHECK(states.get(page_idx) == PageState::COMPRESSED,
          "page should be COMPRESSED after 3 ticks, got %d",
          static_cast<int>(states.get(page_idx)));

    // Now READ the page — this triggers a real SIGSEGV/SIGBUS
    // The fault handler should decompress and restore the page
    volatile uint8_t val = bytes[0];
    CHECK(val == 0x00, "first byte should be 0x00, got 0x%02x", val);

    val = bytes[255];
    CHECK(val == 0xFF, "byte 255 should be 0xFF, got 0x%02x", val);

    // Verify full page data integrity
    bool data_ok = true;
    for (size_t i = 0; i < kPageSize; ++i) {
        if (bytes[i] != static_cast<uint8_t>(i & 0xFF)) {
            data_ok = false;
            fprintf(stderr, "FAIL: data mismatch at offset %zu: got 0x%02x expected 0x%02x\n",
                    i, bytes[i], static_cast<uint8_t>(i & 0xFF));
            break;
        }
    }
    CHECK(data_ok, "full page data verification failed");
    CHECK(states.get(page_idx) == PageState::ACTIVE,
          "page should be ACTIVE after fault, got %d",
          static_cast<int>(states.get(page_idx)));

    // Clean up
    fault_handler.stop();
    g_compressor = nullptr;
}

static void testFaultCycleMultipleRounds() {
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

    PageMap page_map;
    page_map.init();

    CompressorThread compressor;
    compressor.init(&vm, &states, &locks, &store, &engine, &page_map);
    g_compressor = &compressor;

    vm::FaultHandler fault_handler;
    fault_handler.start(faultCallback, nullptr);

    void* page = vm.allocatePages(1);
    CHECK(page != nullptr, "page alloc failed");

    Span* span = newSpanDescriptor();
    span->init(page, 1, 0);
    span->is_large = true;
    page_map.setRange(reinterpret_cast<uintptr_t>(page), 1, span);

    size_t page_idx = vm.pageIndex(reinterpret_cast<uintptr_t>(page));
    states.set(page_idx, PageState::ACTIVE);

    auto* bytes = static_cast<volatile uint8_t*>(page);

    // Multiple compress → fault → verify cycles
    for (int round = 0; round < 5; ++round) {
        // Write new pattern
        for (size_t i = 0; i < kPageSize; ++i) {
            bytes[i] = static_cast<uint8_t>((i + round) & 0xFF);
        }

        // Compress (3 ticks: monitoring + deep monitoring + compression)
        compressor.compressTick();
        compressor.compressTick();
        compressor.compressTick();
        CHECK(states.get(page_idx) == PageState::COMPRESSED,
              "round %d: page should be COMPRESSED", round);

        // Trigger fault by reading
        volatile uint8_t check = bytes[42];
        uint8_t expected = static_cast<uint8_t>((42 + round) & 0xFF);
        CHECK(check == expected,
              "round %d: byte 42 should be 0x%02x, got 0x%02x",
              round, expected, check);

        CHECK(states.get(page_idx) == PageState::ACTIVE,
              "round %d: page should be ACTIVE after fault", round);

        // Verify full data
        bool data_ok = true;
        for (size_t i = 0; i < kPageSize; ++i) {
            if (bytes[i] != static_cast<uint8_t>((i + round) & 0xFF)) {
                data_ok = false;
                break;
            }
        }
        CHECK(data_ok, "round %d: data integrity check failed", round);
    }

    fault_handler.stop();
    g_compressor = nullptr;
}

int main() {
    testFaultCycleBasic();
    testFaultCycleMultipleRounds();

    if (failures == 0) {
        fprintf(stderr, "fault_cycle: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "fault_cycle: %d failures\n", failures);
        return 1;
    }
}
