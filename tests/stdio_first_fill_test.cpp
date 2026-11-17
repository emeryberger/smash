// Regression test: a stdio stream's buffer is malloc'd lazily on its first
// read, inside libc and after smash's fread()/fgets() wrapper has warmed
// whatever it could see. If that malloc returns memory on a compressed page,
// libc's internal read() into it fails with EFAULT (no signal) and the stream
// reports an error. Seen as Valkey's "Short read or OOM loading DB" on every
// DEBUG RELOAD under full-mode smash.
//
// Run under LD_PRELOAD / DYLD_INSERT_LIBRARIES with SMASH_COLD_TIMEOUT_SEC=1.
// Opens, reads and closes a file several times with a pause longer than the
// cold timeout in between, checking every byte.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    char path[] = "/tmp/smash-stdio-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    static char want[1 << 16];
    for (size_t i = 0; i < sizeof(want); ++i) want[i] = static_cast<char>('a' + i % 23);
    if (write(fd, want, sizeof(want)) != static_cast<ssize_t>(sizeof(want))) { perror("write"); return 1; }
    close(fd);

    // The pattern that hit Valkey: a stream is opened, read and closed, its
    // buffer chunk goes back to the allocator's per-thread cache, the page
    // goes cold and is compressed while the chunk sits there, and the next
    // fopen() gets that same chunk back as its stdio buffer.
    int bad = 0;
    for (int round = 0; round < 6 && !bad; ++round) {
        FILE* f = fopen(path, "r");
        if (!f) { fprintf(stderr, "FAIL: fopen errno=%d\n", errno); bad = 1; break; }
        static char got[sizeof(want)];
        size_t n = fread(got, 1, 100, f);           // small first read: fills the stdio buffer
        n += fread(got + 100, 1, sizeof(got) - 100, f);
        if (n != sizeof(want) || memcmp(got, want, sizeof(want)) != 0) {
            fprintf(stderr, "FAIL: round %d read %zu of %zu bytes (ferror=%d errno=%d)\n",
                    round, n, sizeof(want), ferror(f), errno);
            bad = 1;
        }
        fclose(f);
        sleep(3);  // > cold timeout: the freed buffer's page compresses
    }
    unlink(path);
    if (bad) return 1;
    fprintf(stderr, "stdio_first_fill_test: PASSED\n");
    return 0;
}
