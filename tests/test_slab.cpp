// Test slab allocator
#include "core/slab.h"
#include "core/page_map.h"
#include "core/size_classes.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>

using namespace smash;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

static void testSlabAllocFree() {
    PageMap pm;
    pm.init();

    Slab slab;
    slab.init(0, &pm); // class 0 = 16 bytes

    // Allocate several objects
    constexpr int N = 1000;
    void* ptrs[N];
    for (int i = 0; i < N; ++i) {
        ptrs[i] = slab.allocate();
        CHECK(ptrs[i] != nullptr, "slab alloc %d failed", i);
        // Verify we can write to it
        memset(ptrs[i], 0xAA, 16);
    }

    // Free all
    for (int i = 0; i < N; ++i) {
        Span* span = pm.get(reinterpret_cast<uintptr_t>(ptrs[i]));
        CHECK(span != nullptr, "page map lookup failed for alloc %d", i);
        slab.deallocate(span, ptrs[i]);
    }
}

static void testSlabMultipleClasses() {
    PageMap pm;
    pm.init();

    Slab slabs[kNumClasses];
    for (int i = 0; i < kNumClasses; ++i) {
        slabs[i].init(static_cast<uint8_t>(i), &pm);
    }

    // Allocate from each class
    void* ptrs[kNumClasses];
    for (int i = 0; i < kNumClasses; ++i) {
        ptrs[i] = slabs[i].allocate();
        CHECK(ptrs[i] != nullptr, "alloc from class %d failed", i);
        // Write pattern to verify no overlap
        memset(ptrs[i], static_cast<unsigned char>(i), kSizeClasses[i].size);
    }

    // Verify patterns
    for (int i = 0; i < kNumClasses; ++i) {
        auto* bytes = static_cast<unsigned char*>(ptrs[i]);
        for (uint32_t j = 0; j < kSizeClasses[i].size; ++j) {
            CHECK(bytes[j] == static_cast<unsigned char>(i),
                  "data corruption in class %d at offset %u", i, j);
        }
    }

    // Free all
    for (int i = 0; i < kNumClasses; ++i) {
        Span* span = pm.get(reinterpret_cast<uintptr_t>(ptrs[i]));
        CHECK(span != nullptr, "page map lookup failed for class %d", i);
        slabs[i].deallocate(span, ptrs[i]);
    }
}

static void testSlabBatchOps() {
    PageMap pm;
    pm.init();

    Slab slab;
    slab.init(2, &pm); // class 2 = 48 bytes

    void* batch[64];
    size_t got = slab.allocateBatch(batch, 64);
    CHECK(got == 64, "batch alloc got %zu, expected 64", got);

    // Free them back via batch
    slab.deallocateBatch(batch, got);
}

int main() {
    testSlabAllocFree();
    testSlabMultipleClasses();
    testSlabBatchOps();

    if (failures == 0) {
        fprintf(stderr, "slab: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "slab: %d failures\n", failures);
        return 1;
    }
}
