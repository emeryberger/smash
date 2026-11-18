// test_stop_no_drain.cpp — CompressorThread::stop() (the exit path) leaves
// compressed pages compressed and the fault handler installed, so an access
// after shutdown (another thread, a late destructor) decompresses on demand.
//
// Before this, stop() decompressed every page back into memory and
// uninstalled the handler: at exit RSS grew to the uncompressed heap plus the
// not-yet-trimmed store (Redis on SIGTERM: 40 -> 208 MB).
//
// Run with SMASH_COLD_TIMEOUT_SEC=1 (the coordinator compresses the pages
// within a few ticks) and SMASH_NO_ATEXIT=1 (no exit hook pointing at the
// heap-allocated CompressorThread).
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
#include <cstdlib>
#include <memory>
#include <unistd.h>

using namespace smash;

static CompressorThread* g_compressor = nullptr;

static bool faultCallback(uintptr_t addr, void*) {
    return g_compressor && g_compressor->handleFault(addr);
}

static uint8_t pattern(size_t page, size_t off) {
    return static_cast<uint8_t>((page * 7 + (off >> 5)) & 0xff);
}

int main() {
    constexpr size_t kPages = 16;
    VmRegion vm;
    if (!vm.init(256 * kPageSize)) { fprintf(stderr, "FAIL: vm init\n"); return 1; }
    PageStateTable states; states.init(vm.totalPages());
    PageLockTable locks;   locks.init(vm.totalPages());
    CompressStore store;   store.init();
    CompressEngine engine; engine.init();
    PageMap page_map;      page_map.init();

    vm::FaultHandler fault_handler;
    auto owner = std::make_unique<CompressorThread>();   // several MB: heap
    CompressorThread& ct = *owner;
    ct.init(&vm, &states, &locks, &store, &engine, &page_map, &fault_handler);
    g_compressor = &ct;
    fault_handler.start(faultCallback, nullptr);

    auto* base = static_cast<volatile uint8_t*>(vm.allocatePages(kPages));
    if (!base) { fprintf(stderr, "FAIL: page alloc\n"); return 1; }
    Span* span = newSpanDescriptor();
    span->init(const_cast<uint8_t*>(base), kPages, 0);
    span->is_large = true;   // raw data, not slab slots
    page_map.setRange(reinterpret_cast<uintptr_t>(base), kPages, span);
    const size_t first = vm.pageIndex(reinterpret_cast<uintptr_t>(base));
    for (size_t p = 0; p < kPages; ++p) {
        states.set(first + p, PageState::ACTIVE);
        for (size_t i = 0; i < kPageSize; ++i) base[p * kPageSize + i] = pattern(p, i);
    }

    // Let the coordinator compress the pages (fully, not just shadowed).
    ct.start();
    size_t compressed = 0;
    for (int t = 0; t < 600 && compressed < kPages; ++t) {   // up to 60 s
        usleep(100 * 1000);
        compressed = 0;
        for (size_t p = 0; p < kPages; ++p)
            compressed += states.get(first + p) == PageState::COMPRESSED;
    }
    if (compressed == 0) {
        fprintf(stderr, "FAIL: no page reached COMPRESSED before stop()\n");
        ct.stop();   // join the coordinator before `owner` frees it
        fault_handler.stop();
        return 1;
    }

    ct.stop();

    int failures = 0;
    size_t still = 0;
    for (size_t p = 0; p < kPages; ++p)
        still += states.get(first + p) == PageState::COMPRESSED;
    if (still != compressed) {
        fprintf(stderr, "FAIL: stop() decompressed pages: %zu compressed before, %zu after\n",
                compressed, still);
        ++failures;
    }
    // Every byte reads back, through the fault handler for compressed pages.
    for (size_t p = 0; p < kPages; ++p)
        for (size_t i = 0; i < kPageSize; ++i)
            if (base[p * kPageSize + i] != pattern(p, i)) {
                fprintf(stderr, "FAIL: page %zu byte %zu after stop()\n", p, i);
                ++failures;
                break;
            }
    for (size_t p = 0; p < kPages; ++p) {
        const PageState st = states.get(first + p);
        if (st == PageState::COMPRESSED) {
            fprintf(stderr, "FAIL: page %zu still COMPRESSED after being read\n", p);
            ++failures;
        }
    }

    fault_handler.stop();
    g_compressor = nullptr;
    if (failures) { fprintf(stderr, "stop_no_drain: %d failures\n", failures); return 1; }
    fprintf(stderr, "stop_no_drain: %zu/%zu pages compressed, intact after stop() OK\n",
            compressed, kPages);
    return 0;
}
