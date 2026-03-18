// test_free_zeroes.cpp - Check whether free() zeroes memory on this platform
//
// Allocates objects, fills with 0xAA, frees, then re-allocates the same size
// and checks how many bytes are zero. If the allocator zeroes on free (or on
// malloc), re-allocated memory will be mostly zeros.
//
// Usage: ./test_free_zeroes [size] [count]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    size_t size = argc > 1 ? atoi(argv[1]) : 256;
    int count = argc > 2 ? atoi(argv[2]) : 10000;

    // Phase 1: allocate, fill with 0xAA, free
    void** ptrs = (void**)malloc(count * sizeof(void*));
    for (int i = 0; i < count; i++) {
        ptrs[i] = malloc(size);
        memset(ptrs[i], 0xAA, size);
    }
    for (int i = 0; i < count; i++) {
        free(ptrs[i]);
    }

    // Phase 2: re-allocate same size, check for zeros
    int total_bytes = 0;
    int zero_bytes = 0;
    int fully_zeroed = 0;

    for (int i = 0; i < count; i++) {
        ptrs[i] = malloc(size);
        auto* p = static_cast<uint8_t*>(ptrs[i]);
        int obj_zeros = 0;
        for (size_t j = 0; j < size; j++) {
            if (p[j] == 0) obj_zeros++;
        }
        total_bytes += size;
        zero_bytes += obj_zeros;
        if (obj_zeros == (int)size) fully_zeroed++;
        free(ptrs[i]);
    }

    free(ptrs);

    double pct = 100.0 * zero_bytes / total_bytes;
    printf("size=%zu  count=%d\n", size, count);
    printf("total bytes checked: %d\n", total_bytes);
    printf("zero bytes:          %d (%.1f%%)\n", zero_bytes, pct);
    printf("fully zeroed objs:   %d / %d (%.1f%%)\n",
           fully_zeroed, count, 100.0 * fully_zeroed / count);

    if (pct > 95.0)
        printf("RESULT: allocator appears to zero memory on free or re-malloc\n");
    else if (pct > 50.0)
        printf("RESULT: allocator partially zeroes memory\n");
    else
        printf("RESULT: allocator does NOT zero memory\n");

    return 0;
}
