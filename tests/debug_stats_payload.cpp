// Payload for test_debug_stats: allocate slab objects, then stay alive until
// SMASH_DEBUG prints its first periodic stats line from the compressor's
// coordinator thread (every 5 ticks: ~5 s locally, much longer on a loaded
// macOS CI runner whose 10 ms tick sleeps get coalesced), or 60 s at most.
// Before the fix, printing that line overflowed the 128 KiB coordinator
// stack (an inlined ~240 KB bucket-summary array) and SIGBUSed.
//
// stderr is redirected into a pipe that this process reads, so it can exit
// as soon as the line appears; everything read is copied to the real stderr.
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

int main() {
    for (int i = 0; i < 4096; ++i) {
        void* p = std::malloc(256);
        std::memset(p, i, 256);
    }
    int real_err = dup(2);
    int fds[2];
    if (real_err < 0 || pipe(fds) != 0 || dup2(fds[1], 2) < 0) return 2;
    close(fds[1]);

    static const char kNeedle[] = "[smash stats]";
    char window[512] = {0};  // tail of what has been read, for line matching
    size_t wlen = 0;
    const time_t deadline = time(nullptr) + 60;
    while (time(nullptr) < deadline) {
        pollfd pfd{fds[0], POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) continue;
        char buf[256];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        (void)!write(real_err, buf, static_cast<size_t>(n));
        for (ssize_t i = 0; i < n; ++i) {
            if (wlen == sizeof(window) - 1) {
                std::memmove(window, window + 256, wlen - 256);
                wlen -= 256;
            }
            window[wlen++] = buf[i];
        }
        window[wlen] = '\0';
        if (std::strstr(window, kNeedle)) {
            dup2(real_err, 2);  // later output (e.g. at exit) goes to the real stderr
            return 0;
        }
    }
    dup2(real_err, 2);
    return 0;  // the test script reports the missing line
}
