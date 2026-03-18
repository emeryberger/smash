// test_free_zeroes2.cpp - Check whether free() itself zeroes, or malloc() zeroes on alloc
//
// Fills memory with 0xAA, frees it, then reads the memory at the same address
// (technically UB, but diagnostic). If bytes are zero, free() did the zeroing.
// If bytes are 0xAA, zeroing happens at malloc time instead.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    // Use a large-ish size to avoid tiny-object caching effects
  constexpr size_t size = 32;
    constexpr int count = 100;

    printf("Testing whether free() zeroes memory in-place...\n\n");

    for (int trial = 0; trial < count; trial++) {
        auto* p = static_cast<uint8_t*>(malloc(size));
        memset(p, 0xAA, size);

        // Save pointer value before free
        volatile uint8_t* saved = p;

        free(p);

        // Read memory at old address (UB but diagnostic)
        int zero_after_free = 0;
        int aa_after_free = 0;
        for (size_t i = 0; i < size; i++) {
            if (saved[i] == 0x00) zero_after_free++;
            else if (saved[i] == 0xAA) aa_after_free++;
        }

        if (trial < 5) {
            printf("trial %d: zero=%d  0xAA=%d  other=%d\n",
                   trial, zero_after_free, aa_after_free,
                   (int)size - zero_after_free - aa_after_free);
        }
    }

    printf("\nIf zero counts are high: free() zeroes memory\n");
    printf("If 0xAA counts are high: zeroing happens at malloc() time\n");

    return 0;
}
