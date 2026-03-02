// Test size class mapping correctness
#include "core/size_classes.h"
#include "smash/config.h"

#include <cstdio>
#include <cstdlib>

using namespace smash;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

static void testSizeToClass() {
    // Every size 1..kMaxSmallSize should map to a class whose size >= requested
    for (size_t size = 1; size <= kMaxSmallSize; ++size) {
        uint8_t sc = sizeToClass(size);
        CHECK(sc < kNumClasses, "size %zu mapped to invalid class %d", size, sc);
        CHECK(classSize(sc) >= size, "size %zu mapped to class %d (size %u) which is too small",
              size, sc, classSize(sc));

        // Verify it's the tightest fit: previous class should be too small
        if (sc > 0) {
            CHECK(classSize(sc - 1) < size,
                  "size %zu mapped to class %d but class %d (size %u) would also fit",
                  size, sc, sc - 1, classSize(sc - 1));
        }
    }

    // Size 0 should map to class 0 (minimum allocation)
    CHECK(sizeToClass(0) == 0, "size 0 should map to class 0");

    // Sizes > kMaxSmallSize should return kNumClasses
    CHECK(sizeToClass(kMaxSmallSize + 1) == kNumClasses,
          "size %zu should be large", kMaxSmallSize + 1);
}

static void testClassSizes() {
    // All class sizes should be multiples of kMinAlignment
    for (int i = 0; i < kNumClasses; ++i) {
        CHECK(classSize(i) % kMinAlignment == 0,
              "class %d size %u not aligned to %zu", i, classSize(i), kMinAlignment);
    }

    // Classes should be in strictly increasing order
    for (int i = 1; i < kNumClasses; ++i) {
        CHECK(classSize(i) > classSize(i - 1),
              "class %d size %u <= class %d size %u", i, classSize(i), i - 1, classSize(i - 1));
    }

    // First class should be kMinAlignment (16)
    CHECK(classSize(0) == 16, "first class should be 16, got %u", classSize(0));

    // Last class should be kMaxSmallSize (16384)
    CHECK(classSize(kNumClasses - 1) == kMaxSmallSize,
          "last class should be %zu, got %u", kMaxSmallSize, classSize(kNumClasses - 1));
}

static void testSpanInfo() {
    // Every class should have at least 1 page and at least 1 object per span
    for (int i = 0; i < kNumClasses; ++i) {
        const auto& info = kSizeClasses[i];
        CHECK(info.pages >= 1, "class %d has 0 pages", i);
        CHECK(info.objects >= 1, "class %d has 0 objects", i);
        CHECK(info.pages <= static_cast<uint32_t>(kMaxSpanPages),
              "class %d has %u pages > max %d", i, info.pages, kMaxSpanPages);
        CHECK(info.objects == (info.pages * kPageSize) / info.size,
              "class %d object count mismatch", i);
    }
}

// Minimal test runner
int main() {
    testSizeToClass();
    testClassSizes();
    testSpanInfo();

    if (failures == 0) {
        fprintf(stderr, "size_classes: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "size_classes: %d failures\n", failures);
        return 1;
    }
}
