// Linux uffd mode: UFFDIO_MOVE refuses (EBUSY) an anonymous page that is not
// exclusively owned, e.g. one still shared copy-on-write with a fork() child.
// reclaimBatch() retried such pages every tick, leaving them COMPRESSED_SHADOW
// with their blob AND the page resident (compression costing memory). A
// refused page now drops its shadow copy and backs off.
//
// With a sleeping child holding the COW sharing for 8 s, the blob bytes smash
// holds must stay small, and every byte must read back intact. (Reclaiming
// such pages -- fork also clears PageAnonExclusive until a write fault -- is
// not attempted; see agents/syscall-compat.md.) Exits 77 if libsmash is not
// preloaded.
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "smash/smash.h"

using StatsFn = int (*)(SmashStats*);
static StatsFn get_stats;

static SmashStats stats() { SmashStats s{}; get_stats(&s); return s; }

int main() {
    get_stats = reinterpret_cast<StatsFn>(dlsym(RTLD_DEFAULT, "smash_get_stats"));
    if (!get_stats) { fprintf(stderr, "move_refused_test: libsmash not preloaded; skipping\n"); return 77; }
    constexpr size_t kChunk = 1u << 20, kN = 32;  // 32 MiB, managed in both modes
    unsigned char* c[kN];
    for (size_t i = 0; i < kN; ++i) {
        c[i] = static_cast<unsigned char*>(malloc(kChunk));
        if (!c[i]) return 2;
        for (size_t j = 0; j < kChunk; ++j) c[i][j] = static_cast<unsigned char>((i * 13 + j / 256) & 0x3f);
    }
    pid_t child = fork();
    if (child == 0) { pause(); _exit(0); }  // hold the COW sharing
    if (child < 0) return 2;

    unsigned long long max_stored = 0;
    for (int t = 0; t < 8; ++t) {
        sleep(1);
        SmashStats st = stats();
        if (st.stored_bytes > max_stored) max_stored = st.stored_bytes;
    }
    const unsigned long long held = stats().stored_bytes;
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    fprintf(stderr, "move_refused_test: blob bytes held with the child alive: %llu now (peak %llu)\n",
            held, max_stored);
    for (size_t i = 0; i < kN; ++i)
        for (size_t j = 0; j < kChunk; ++j)
            if (c[i][j] != static_cast<unsigned char>((i * 13 + j / 256) & 0x3f)) {
                fprintf(stderr, "FAIL: chunk %zu byte %zu\n", i, j);
                return 1;
            }
    // A blob kept past its refusal is the bug; allow one tick's batch in flight.
    if (held > kN * kChunk / 256) {
        fprintf(stderr, "FAIL: %llu blob bytes still held for unreclaimable (COW-shared) pages\n", held);
        return 1;
    }
    fprintf(stderr, "move_refused_test: PASS\n");
    return 0;
}
