// Test SpillStore: file-backed blob storage for very-cold compressed blobs.
// Exercises store/release/reuse/split/free-list, region drain (hole-punch),
// pointer-in-mapping checks, and file exhaustion → fallback (nullptr).
#include "compress/spill_store.h"
#include "smash/config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <vector>

using namespace smash;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

namespace {
constexpr size_t kMapSlots = 3;                       // 3 × 16 MB = 48 MB
const size_t kMapSize = kMapSlots * (16 * 1024 * 1024);
}  // namespace

int main() {
    // The metadata array is sized via metadataBytesFor(); SpillStore manages
    // its (public, opaque) Region structs internally.
    int fd = -1;
    {
        char path[] = "spill_test.XXXXXX";
        fd = mkstemp(path);
        CHECK(fd >= 0, "mkstemp failed");
        if (fd < 0) return 1;
        unlink(path);
    }
    CHECK(ftruncate(fd, (off_t)kMapSize) == 0, "ftruncate failed");
    void* map = mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED, "mmap failed");
    if (map == MAP_FAILED) return 1;

    size_t meta_bytes = SpillStore::metadataBytesFor(kMapSize);
    CHECK(meta_bytes > 0, "metadataBytesFor returned 0");
    void* meta = calloc(1, meta_bytes);
    CHECK(meta != nullptr, "metadata calloc failed");

    SpillStore store;
    bool ok = store.init(fd, map,
                         kMapSize,
                         reinterpret_cast<SpillStore::Region*>(meta),
                         kMapSlots);
    CHECK(ok && store.ready(), "SpillStore::init failed");

    // --- store / contains / readback ---
    const char pattern[256] = {0};  // zero blob is fine; we check identity
    char blobA[200];
    for (int i = 0; i < 200; ++i) blobA[i] = (char)(i * 7 + 1);
    size_t aszA = 0;
    void* pA = store.store(blobA, sizeof(blobA), &aszA, /*page_idx*/ 0);
    CHECK(pA != nullptr, "store(blobA) returned null");
    CHECK(store.contains(pA), "stored blob not within mapping");
    CHECK(aszA >= sizeof(blobA), "alloc_size < requested");
    CHECK(memcmp(pA, blobA, sizeof(blobA)) == 0, "blobA readback mismatch");
    (void)pattern;

    // A second blob in the same shard must not overlap the first.
    char blobB[64];
    memset(blobB, 0xCD, sizeof(blobB));
    size_t aszB = 0;
    void* pB = store.store(blobB, sizeof(blobB), &aszB, 0);
    CHECK(pB != nullptr, "store(blobB) returned null");
    CHECK(pB != pA, "blobB overlaps blobA");
    CHECK(memcmp(pA, blobA, sizeof(blobA)) == 0, "blobA corrupted by blobB store");
    CHECK(memcmp(pB, blobB, sizeof(blobB)) == 0, "blobB readback mismatch");

    // --- free-list reuse: release B then allocate a same-size blob → reuse slot ---
    store.release(pB, aszB, 0);
    char blobC[64];
    memset(blobC, 0xEE, sizeof(blobC));
    size_t aszC = 0;
    void* pC = store.store(blobC, sizeof(blobC), &aszC, 0);
    CHECK(pC == pB, "expected free-list reuse of B's slot for C (got %p vs %p)", pC, pB);
    CHECK(memcmp(pC, blobC, sizeof(blobC)) == 0, "blobC readback mismatch");

    // --- split: release a large slot, allocate a smaller one, remainder reused ---
    char big[1024]; memset(big, 0x11, sizeof(big));
    size_t aszBig = 0;
    void* pBig = store.store(big, sizeof(big), &aszBig, 0);
    CHECK(pBig != nullptr, "store(big) failed");
    store.release(pBig, aszBig, 0);
    char small1[128]; memset(small1, 0x22, sizeof(small1));
    size_t aszS1 = 0;
    void* pS1 = store.store(small1, sizeof(small1), &aszS1, 0);
    CHECK(pS1 == pBig, "small1 should reuse front of freed big slot");
    char small2[128]; memset(small2, 0x33, sizeof(small2));
    size_t aszS2 = 0;
    void* pS2 = store.store(small2, sizeof(small2), &aszS2, 0);
    // small2 should come from the split remainder of big (right after small1),
    // not corrupt small1.
    CHECK(pS2 != pS1, "small2 overlaps small1");
    CHECK(memcmp(pS1, small1, sizeof(small1)) == 0, "small1 corrupted by split");
    CHECK(memcmp(pS2, small2, sizeof(small2)) == 0, "small2 readback mismatch");

    // --- region drain: fill ~one region, release all, expect reset/reuse ---
    // (Functional check: after releasing everything in shard 0, a fresh store
    // succeeds and lands within the mapping. Hole-punch is best-effort.)
    {
        std::vector<std::pair<void*, size_t>> live;
        const size_t blob_sz = 4096;
        char buf[4096]; memset(buf, 0x5A, sizeof(buf));
        // 16MB region / ~4KB ≈ 4096 blobs; store enough to span >1 region.
        for (int i = 0; i < 5000; ++i) {
            size_t a = 0;
            void* p = store.store(buf, blob_sz, &a, /*shard*/ 0);
            if (!p) break;  // exhaustion handled below
            CHECK(store.contains(p), "drain-phase blob outside mapping");
            live.emplace_back(p, a);
        }
        CHECK(!live.empty(), "could not store any drain-phase blobs");
        for (auto& kv : live) store.release(kv.first, kv.second, 0);
        // After full release, a new store must still succeed.
        size_t a = 0;
        void* p = store.store(buf, blob_sz, &a, 0);
        CHECK(p != nullptr, "store after full drain failed");
        CHECK(store.contains(p), "post-drain blob outside mapping");
    }

    // --- exhaustion: a fresh shard cannot exceed the file; eventually nullptr ---
    {
        // Use shard 1 and store large blobs until exhausted. Total file is 48MB
        // across slots assigned round-robin; once slots run out, store→nullptr.
        char buf[1 << 20]; memset(buf, 0x77, sizeof(buf));  // 1 MB
        int stored = 0;
        bool saw_null = false;
        for (int i = 0; i < 200; ++i) {  // 200 MB attempted >> 48 MB file
            size_t a = 0;
            void* p = store.store(buf, sizeof(buf), &a, /*shard*/ 1);
            if (!p) { saw_null = true; break; }
            CHECK(store.contains(p), "exhaustion-phase blob outside mapping");
            ++stored;
        }
        CHECK(saw_null, "expected store() to return null when file exhausted");
        CHECK(stored > 0, "expected some stores to succeed before exhaustion");
    }

    munmap(map, kMapSize);
    free(meta);
    close(fd);

    if (failures == 0) {
        fprintf(stderr, "spill_store: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "spill_store: %d failures\n", failures);
    return 1;
}
