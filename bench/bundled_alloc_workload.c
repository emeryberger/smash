// Workload for bench_bundled_alloc.py: allocate a compressible object graph
// through a *bundled* allocator's own entry points (mi_malloc / tc_malloc), so
// that none of it goes through the interposed malloc — the situation of an
// application that links jemalloc/mimalloc/tcmalloc statically. Under
// LD_PRELOAD=libsmash the only way smash can reach this memory is by tracking
// the allocator's mmap'd arenas (SMASH_TRACK_EXTERNAL=1).
//
//   bundled_alloc_workload <mi|tc|libc> <MiB> <cool_sec>
//
// Phases (stdout lines the driver waits on):
//   FILLED            objects allocated + written; driver samples peak RSS
//   (sleep cool_sec)  driver samples RSS every second
//   CHECK <ok|bad> <checksum>   every object re-read and verified
// A quarter of the objects are freed after the fill so the allocator purges
// (madvise) part of its arenas — the pattern that interacts with compressed
// pages.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(USE_MIMALLOC)
#include <mimalloc.h>
#define XMALLOC(n) mi_malloc(n)
#define XFREE(p) mi_free(p)
#define XNAME "mi"
#elif defined(USE_TCMALLOC)
void* tc_malloc(size_t);
void tc_free(void*);
#define XMALLOC(n) tc_malloc(n)
#define XFREE(p) tc_free(p)
#define XNAME "tc"
#else
#define XMALLOC(n) malloc(n)
#define XFREE(p) free(p)
#define XNAME "libc"
#endif

static uint64_t mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return x;
}

// Object i: size in [64, 4096), contents = a short record repeated (compressible
// like typical heap data but not all-zero), keyed by i so torn/stale data shows.
static size_t objSize(size_t i) { return 64 + (mix(i) % 4032); }
static void fillObj(unsigned char* p, size_t n, size_t i) {
    char rec[32];
    int len = snprintf(rec, sizeof(rec), "rec:%zu;", i);
    for (size_t k = 0; k < n; ++k) p[k] = (unsigned char)rec[k % (size_t)len];
}
static int checkObj(const unsigned char* p, size_t n, size_t i) {
    char rec[32];
    int len = snprintf(rec, sizeof(rec), "rec:%zu;", i);
    for (size_t k = 0; k < n; ++k)
        if (p[k] != (unsigned char)rec[k % (size_t)len]) return 0;
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <mi|tc|libc> <MiB> <cool_sec>\n", argv[0]); return 2; }
    size_t target = (size_t)atol(argv[2]) << 20;
    int cool = atoi(argv[3]);
    size_t cap = target / 64 + 16, n = 0, bytes = 0;
    unsigned char** objs = (unsigned char**)XMALLOC(cap * sizeof(*objs));
    while (bytes < target && n < cap) {
        size_t sz = objSize(n);
        objs[n] = (unsigned char*)XMALLOC(sz);
        fillObj(objs[n], sz, n);
        bytes += sz;
        ++n;
    }
    // Free every 4th object: the allocator purges dirty pages it can release.
    for (size_t i = 0; i < n; i += 4) { XFREE(objs[i]); objs[i] = NULL; }
    printf("FILLED %s objects=%zu bytes=%zu\n", XNAME, n, bytes);
    fflush(stdout);
    sleep((unsigned)cool);
    int ok = 1;
    size_t nbad = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        size_t sz = objSize(i);
        if (!checkObj(objs[i], sz, i)) {
            char rec[32];
            int len = snprintf(rec, sizeof(rec), "rec:%zu;", i);
            size_t k = 0;
            while (k < sz && objs[i][k] == (unsigned char)rec[k % (size_t)len]) ++k;
            if (ok) fprintf(stderr, "bad object %zu at %p size %zu: first mismatch at +%zu (%p) got 0x%02x want 0x%02x\n",
                    i, (void*)objs[i], sz, k, (void*)(objs[i] + k), objs[i][k],
                    (unsigned char)rec[k % (size_t)len]);
            if (ok) fprintf(stderr, "(further bad objects counted below)\n");
            ok = 0;
            ++nbad;
            if (nbad <= 8 && nbad > 1)
                fprintf(stderr, "  bad object %zu at %p\n", i, (void*)objs[i]);
            continue;
        }
        sum += objs[i][sz / 2];
    }
    if (nbad) fprintf(stderr, "bad objects: %zu\n", nbad);
    printf("CHECK %s %llu\n", ok ? "ok" : "bad", (unsigned long long)sum);
    fflush(stdout);
    return ok ? 0 : 1;
}
