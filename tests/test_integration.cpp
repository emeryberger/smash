// Integration test: exercise the full SmashHeap path
// (without alloc8 interposition — direct method calls)
#include "smash_heap.h"
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

// Access the SmashHeap singleton the same way alloc8's HeapRedirect does
static SmashHeap* getHeap() {
    alignas(SmashHeap) static char buf[sizeof(SmashHeap)];
    static SmashHeap* heap = new (buf) SmashHeap;
    return heap;
}

static void testSmallAllocs() {
    auto* heap = getHeap();

    // Test various small sizes
    size_t sizes[] = {1, 8, 16, 32, 48, 64, 100, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    void* ptrs[sizeof(sizes) / sizeof(sizes[0])];

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        ptrs[i] = heap->malloc(sizes[i]);
        CHECK(ptrs[i] != nullptr, "malloc(%zu) failed", sizes[i]);

        size_t usable = heap->getSize(ptrs[i]);
        CHECK(usable >= sizes[i], "getSize returned %zu for malloc(%zu)", usable, sizes[i]);

        // Write and read back
        memset(ptrs[i], 0x42, sizes[i]);
    }

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        heap->free(ptrs[i]);
    }
}

static void testLargeAllocs() {
    auto* heap = getHeap();

    size_t sizes[] = {32768, 65536, 1024 * 1024, 4 * 1024 * 1024};
    for (size_t size : sizes) {
        void* p = heap->malloc(size);
        CHECK(p != nullptr, "large malloc(%zu) failed", size);
        memset(p, 0xBB, size);
        size_t usable = heap->getSize(p);
        CHECK(usable >= size, "large getSize returned %zu for %zu", usable, size);
        heap->free(p);
    }
}

static void testMedalign() {
    auto* heap = getHeap();

    // Test various alignments
    size_t aligns[] = {16, 32, 64, 128, 256, 512, 1024, 4096};
    for (size_t align : aligns) {
        void* p = heap->memalign(align, 100);
        CHECK(p != nullptr, "memalign(%zu, 100) failed", align);
        CHECK(reinterpret_cast<uintptr_t>(p) % align == 0,
              "memalign(%zu, 100) not aligned: %p", align, p);
        heap->free(p);
    }
}

static void testAllocFreeStress() {
    auto* heap = getHeap();

    constexpr int N = 10000;
    void* ptrs[N];

    // Allocate all
    for (int i = 0; i < N; ++i) {
        size_t size = (i % 256) + 1;
        ptrs[i] = heap->malloc(size);
        CHECK(ptrs[i] != nullptr, "stress alloc %d (size %zu) failed", i, size);
    }

    // Free in reverse
    for (int i = N - 1; i >= 0; --i) {
        heap->free(ptrs[i]);
    }

    // Allocate again (tests reuse)
    for (int i = 0; i < N; ++i) {
        size_t size = (i % 256) + 1;
        ptrs[i] = heap->malloc(size);
        CHECK(ptrs[i] != nullptr, "stress re-alloc %d failed", i);
    }

    // Free even indices
    for (int i = 0; i < N; i += 2) {
        heap->free(ptrs[i]);
    }

    // Allocate where we freed
    for (int i = 0; i < N; i += 2) {
        ptrs[i] = heap->malloc(64);
        CHECK(ptrs[i] != nullptr, "stress re-alloc %d after partial free failed", i);
    }

    // Free all
    for (int i = 0; i < N; ++i) {
        heap->free(ptrs[i]);
    }
}

static void testZeroSize() {
    auto* heap = getHeap();
    void* p = heap->malloc(0);
    CHECK(p != nullptr, "malloc(0) should return non-null");
    heap->free(p);
}

static void testFreeNull() {
    auto* heap = getHeap();
    heap->free(nullptr); // should not crash
}

int main() {
    testSmallAllocs();
    testLargeAllocs();
    testMedalign();
    testAllocFreeStress();
    testZeroSize();
    testFreeNull();

    if (failures == 0) {
        fprintf(stderr, "integration: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "integration: %d failures\n", failures);
        return 1;
    }
}
