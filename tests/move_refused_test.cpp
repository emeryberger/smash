// Linux uffd mode: UFFDIO_MOVE refuses (EBUSY) an anonymous page that is not
// exclusively owned, e.g. one still shared copy-on-write with a fork() child.
// reclaimBatch() retried such pages every tick, leaving them COMPRESSED_SHADOW
// with their blob AND the page resident (compression costing memory). A
// refused page now drops its shadow copy and backs off.
//
// With a sleeping child holding the COW sharing for 8 s, the blob bytes smash
// holds must stay small, and every byte must read back intact. Once the child
// has exited the same pages must be reclaimed (they are still not
// PageAnonExclusive: smash unshares them first). (Reclaiming
// such pages -- fork also clears PageAnonExclusive until a write fault -- is
// not attempted; see agents/syscall-compat.md.) Exits 77 if libsmash is not
// preloaded or smash is not reclaiming through userfaultfd (Linux < 6.8).
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "smash/smash.h"

using StatsFn = int (*)(SmashStats*);
static StatsFn get_stats;

static SmashStats stats() { SmashStats s{}; get_stats(&s); return s; }

// smash opens its userfaultfd at heap init, before main().
static bool uffdOpen() {
    char path[64], link[128];
    for (int fd = 0; fd < 1024; ++fd) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, link, sizeof(link) - 1);
        if (n <= 0) continue;
        link[n] = 0;
        if (strstr(link, "userfaultfd")) return true;
    }
    return false;
}

int main() {
    get_stats = reinterpret_cast<StatsFn>(dlsym(RTLD_DEFAULT, "smash_get_stats"));
    if (!get_stats) { fprintf(stderr, "move_refused_test: libsmash not preloaded; skipping\n"); return 77; }
    if (!uffdOpen()) { fprintf(stderr, "move_refused_test: smash is not using userfaultfd; skipping\n"); return 77; }
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
    // The child is gone: the pages are mapped by this process alone, but fork
    // cleared PageAnonExclusive and nothing has written them since, so MOVE
    // still refuses them. They must become reclaimable again (smash unshares
    // a page only this process maps before retrying the MOVE).
    const time_t t0 = time(nullptr);
    unsigned long long comp = 0;
    while (time(nullptr) - t0 < 90) {
        comp = stats().compressed_bytes;
        if (comp >= kN * kChunk / 2) break;
        sleep(1);
    }
    fprintf(stderr, "move_refused_test: %llu bytes reclaimed after the child exited (%lld s)\n", comp,
            static_cast<long long>(time(nullptr) - t0));
    for (size_t i = 0; i < kN; ++i)
        for (size_t j = 0; j < kChunk; ++j)
            if (c[i][j] != static_cast<unsigned char>((i * 13 + j / 256) & 0x3f)) {
                fprintf(stderr, "FAIL: chunk %zu byte %zu after the child exited\n", i, j);
                return 1;
            }
    if (comp < kN * kChunk / 2) {
        fprintf(stderr, "FAIL: formerly COW-shared pages never reclaimed after the child exited\n");
        return 1;
    }
    fprintf(stderr, "move_refused_test: PASS\n");
    return 0;
}
