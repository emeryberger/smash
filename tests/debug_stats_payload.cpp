// Payload for test_debug_stats: allocate slab objects, then stay alive past
// SMASH_DEBUG's first periodic stats line (every 5 compressor ticks, ~5 s).
// Before the fix, printing that line from the 128 KiB coordinator stack
// overflowed it (an inlined ~240 KB bucket-summary array) and SIGBUSed.
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main() {
    for (int i = 0; i < 4096; ++i) {
        void* p = std::malloc(256);
        std::memset(p, i, 256);
    }
    sleep(7);
    return 0;
}
