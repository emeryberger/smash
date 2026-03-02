// Simple program to test smash interposition via DYLD_INSERT_LIBRARIES
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    // Various allocation patterns
    void* p1 = malloc(16);
    void* p2 = malloc(256);
    void* p3 = malloc(4096);
    void* p4 = malloc(65536);

    if (!p1 || !p2 || !p3 || !p4) {
        fprintf(stderr, "FAIL: allocation returned null\n");
        return 1;
    }

    // Write patterns
    memset(p1, 0xAA, 16);
    memset(p2, 0xBB, 256);
    memset(p3, 0xCC, 4096);
    memset(p4, 0xDD, 65536);

    // Realloc
    p2 = realloc(p2, 512);
    if (!p2) {
        fprintf(stderr, "FAIL: realloc returned null\n");
        return 1;
    }

    // Calloc
    void* p5 = calloc(100, 64);
    if (!p5) {
        fprintf(stderr, "FAIL: calloc returned null\n");
        return 1;
    }
    // Verify calloc zeroed
    auto* bytes = static_cast<unsigned char*>(p5);
    for (int i = 0; i < 100 * 64; ++i) {
        if (bytes[i] != 0) {
            fprintf(stderr, "FAIL: calloc not zeroed at byte %d\n", i);
            return 1;
        }
    }

    // Aligned alloc
    void* p6 = nullptr;
    if (posix_memalign(&p6, 256, 1024) != 0 || !p6) {
        fprintf(stderr, "FAIL: posix_memalign failed\n");
        return 1;
    }
    if (reinterpret_cast<uintptr_t>(p6) % 256 != 0) {
        fprintf(stderr, "FAIL: alignment not satisfied\n");
        return 1;
    }

    free(p1);
    free(p2);
    free(p3);
    free(p4);
    free(p5);
    free(p6);

    // Stress: many small allocs
    for (int i = 0; i < 10000; ++i) {
        void* p = malloc((i % 256) + 1);
        if (!p) {
            fprintf(stderr, "FAIL: stress alloc %d failed\n", i);
            return 1;
        }
        free(p);
    }

    fprintf(stderr, "interpose_test: PASSED\n");
    return 0;
}
