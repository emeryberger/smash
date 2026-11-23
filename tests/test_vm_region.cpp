// Test VmRegion allocation, release, and reuse
#include "vm/vm_region.h"
#include "vm/page_state.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

using namespace smash;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

static void testBasicAlloc() {
    VmRegion vm;
    bool ok = vm.init(64 * kPageSize);
    CHECK(ok, "VmRegion init failed");
    // contigPages() is the bump-arena capacity — the original "total"
    // before VmRegion grew an external-page tail for application-direct
    // mmap / Mach VM tracking. totalPages() now also includes that tail.
    CHECK(vm.contigPages() == 64, "expected 64 contig pages, got %zu", vm.contigPages());
    CHECK(vm.committedPages() == 0, "expected 0 committed pages initially");

    // Allocate 4 pages
    void* p1 = vm.allocatePages(4);
    CHECK(p1 != nullptr, "allocatePages(4) failed");
    CHECK(vm.committedPages() == 4, "expected 4 committed pages");

    // Verify we can read/write the pages
    memset(p1, 0xAA, 4 * kPageSize);
    auto* bytes = static_cast<uint8_t*>(p1);
    CHECK(bytes[0] == 0xAA, "write/read failed at offset 0");
    CHECK(bytes[4 * kPageSize - 1] == 0xAA, "write/read failed at last byte");

    // Allocate more
    void* p2 = vm.allocatePages(2);
    CHECK(p2 != nullptr, "allocatePages(2) failed");
    CHECK(vm.committedPages() == 6, "expected 6 committed pages");

    // Verify no overlap
    memset(p2, 0xBB, 2 * kPageSize);
    CHECK(bytes[0] == 0xAA, "p1 corrupted after p2 alloc");
}

static void testContains() {
    VmRegion vm;
    vm.init(16 * kPageSize);

    void* p = vm.allocatePages(1);
    CHECK(p != nullptr, "alloc failed");

    auto addr = reinterpret_cast<uintptr_t>(p);
    CHECK(vm.contains(addr), "contains(base of alloc) should be true");
    CHECK(vm.contains(addr + kPageSize - 1), "contains(end of page) should be true");
    CHECK(!vm.contains(addr - 1), "contains(before region) should be false");

    size_t idx = vm.pageIndex(addr);
    CHECK(idx == 0, "first allocation should be page index 0, got %zu", idx);

    void* back = vm.pageAddress(idx);
    CHECK(back == p, "pageAddress(pageIndex(addr)) != addr");
}

static void testReleaseAndReuse() {
    VmRegion vm;
    vm.init(32 * kPageSize);

    // Allocate and release
    void* p1 = vm.allocatePages(4);
    CHECK(p1 != nullptr, "alloc p1 failed");
    memset(p1, 0x42, 4 * kPageSize);

    vm.releasePages(p1, 4);

    // Allocate again — should reuse the freed pages
    void* p2 = vm.allocatePages(4);
    CHECK(p2 != nullptr, "realloc failed");

    // Should be able to use the memory
    memset(p2, 0x55, 4 * kPageSize);
}

static void testPageStateTable() {
    PageStateTable pst;
    pst.init(64);

    // All pages should start as EMPTY (zeroed bootstrap memory)
    CHECK(pst.get(0) == PageState::EMPTY, "page 0 should be EMPTY");
    CHECK(pst.get(63) == PageState::EMPTY, "page 63 should be EMPTY");

    // Set and get
    pst.set(10, PageState::ACTIVE);
    CHECK(pst.get(10) == PageState::ACTIVE, "page 10 should be ACTIVE");

    pst.set(10, PageState::COMPRESSED);
    CHECK(pst.get(10) == PageState::COMPRESSED, "page 10 should be COMPRESSED");

    // CAS transition
    bool ok = pst.transition(10, PageState::COMPRESSED, PageState::ACTIVE);
    CHECK(ok, "transition COMPRESSED→ACTIVE should succeed");
    CHECK(pst.get(10) == PageState::ACTIVE, "page 10 should be ACTIVE after transition");

    // Failed CAS (wrong expected state)
    ok = pst.transition(10, PageState::COMPRESSED, PageState::EMPTY);
    CHECK(!ok, "transition with wrong expected state should fail");
    CHECK(pst.get(10) == PageState::ACTIVE, "page 10 should still be ACTIVE");
}

static void testPageLockTable() {
    PageLockTable plt;
    plt.init(16);

    // Basic lock/unlock
    plt.lock(5);
    plt.unlock(5);

    // Lock multiple pages
    plt.lock(0);
    plt.lock(1);
    plt.unlock(0);
    plt.unlock(1);
}

// External-page tracking hash: tracking a few thousand contiguous pages
// (RocksDB carried ~4.7K external pages) must not fault in one 4 KiB page of
// the 32 MiB table per tracked page (it did, ~19 MB, while pages hashed one
// by one); lookups and untracking must still work.
static void testTrackHashLocality() {
    VmRegion vm;
    if (!vm.init(64 * kPageSize)) { CHECK(false, "init"); return; }
    constexpr size_t kN = 4096;
    void* r = mmap(nullptr, kN * kPageSize, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    CHECK(r != MAP_FAILED, "reserve");
    const uintptr_t base = reinterpret_cast<uintptr_t>(r);
    const size_t before = vm.trackHashResidentBytes();
    for (size_t i = 0; i < kN; ++i)
        CHECK(vm.trackExternalPage(base + i * kPageSize) != 0, "track page %zu", i);
    const size_t grown = vm.trackHashResidentBytes() - before;
    const size_t os_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    fprintf(stderr, "vm_region: tracking %zu pages made %zu bytes (%zu OS pages) of the hash resident\n", kN,
            grown, grown / os_page);
    // At least 32 tracked pages per resident table page on average.
    CHECK(grown / os_page <= kN / 32, "tracking %zu pages faulted in %zu bytes of hash table", kN, grown);
    for (size_t i = 0; i < kN; ++i) {
        const size_t idx = vm.pageIndex(base + i * kPageSize);
        CHECK(idx >= vm.contigPages() && vm.pageAddress(idx) == reinterpret_cast<void*>(base + i * kPageSize),
              "lookup of page %zu", i);
    }
    for (size_t i = 0; i < kN; i += 2) vm.untrackExternalPage(base + i * kPageSize, true);
    for (size_t i = 0; i < kN; ++i)
        CHECK(vm.contains(base + i * kPageSize) == (i % 2 == 1), "contains after untrack, page %zu", i);
    munmap(r, kN * kPageSize);
}

int main() {
    testTrackHashLocality();
    testBasicAlloc();
    testContains();
    testReleaseAndReuse();
    testPageStateTable();
    testPageLockTable();

    if (failures == 0) {
        fprintf(stderr, "vm_region: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "vm_region: %d failures\n", failures);
        return 1;
    }
}
