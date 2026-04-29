// Verify the application-direct mmap / Mach VM interposers register
// and round-trip pages through the compressor without corrupting data.
//
// Run under DYLD_INSERT_LIBRARIES=...libsmash.dylib (macOS) or
// LD_PRELOAD=...libsmash.so (Linux). Env vars:
//
//   SMASH_LARGE_ONLY=0
//   SMASH_COLD_TIMEOUT_SEC=1
//   SMASH_DEFER_PHASES_MS=0
//
// Test sequence (one per allocator surface):
//   1. allocate an anon writable region
//   2. write a deterministic per-page pattern
//   3. sleep long enough for the compressor to tick (≥ COLD_TIMEOUT * 2)
//   4. read the pattern back — every page faults if compressed, the
//      fault handler decompresses, the application sees its bytes
//   5. release the region
//
// Pass criterion: every byte read in step 4 matches what step 2 wrote.
// If smash mistracked, dropped writes, or corrupted compressed buffers,
// the byte comparison fails.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {

// macOS arm64 uses 16 KB pages; Linux/x86_64 uses 4 KB. sysconf at runtime.
size_t pageSize() {
    long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? static_cast<size_t>(v) : 4096;
}

// Fill page i with byte (i & 0xff). Each page becomes its own pattern
// so a torn read from neighbouring pages is detectable.
void fillPattern(void* base, size_t bytes, size_t page) {
    auto* b = static_cast<unsigned char*>(base);
    size_t pages = bytes / page;
    for (size_t i = 0; i < pages; ++i) {
        std::memset(b + i * page, static_cast<int>(i & 0xff), page);
    }
}

bool checkPattern(void* base, size_t bytes, size_t page, const char* tag) {
    auto* b = static_cast<unsigned char*>(base);
    size_t pages = bytes / page;
    for (size_t i = 0; i < pages; ++i) {
        unsigned char want = static_cast<unsigned char>(i & 0xff);
        for (size_t j = 0; j < page; ++j) {
            if (b[i * page + j] != want) {
                fprintf(stderr,
                        "FAIL [%s]: page %zu byte %zu got 0x%02x want 0x%02x\n",
                        tag, i, j, b[i * page + j], want);
                return false;
            }
        }
    }
    return true;
}

bool runMmapTest(size_t page) {
    constexpr size_t kBytes = 4 * 1024 * 1024;  // 4 MiB → 256 pages @ 16 KB
    void* p = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "FAIL [mmap]: mmap returned MAP_FAILED\n");
        return false;
    }
    fillPattern(p, kBytes, page);

    // Wait for the compressor: COLD_TIMEOUT_SEC=1 → first eligible after
    // 1 s; tick interval is sub-second; 3 s is comfortably past.
    sleep(3);

    // Read back. Compressed pages refault on the first byte of each
    // page; the handler restores RW and we keep going.
    if (!checkPattern(p, kBytes, page, "mmap")) {
        munmap(p, kBytes);
        return false;
    }
    if (munmap(p, kBytes) != 0) {
        fprintf(stderr, "FAIL [mmap]: munmap returned non-zero\n");
        return false;
    }
    fprintf(stderr, "external_mapping_test: mmap PASSED\n");
    return true;
}

// Negative: a file-backed mmap must NOT be tracked. We can't query
// smash's tracker directly from outside, so we verify behaviour: the
// region remains usable, and msync succeeds (which would fail or
// produce wrong data if compression corrupted the in-memory copy
// before write-back).
bool runFileBackedTest() {
    char path[] = "/tmp/smash-ext-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "FAIL [file]: mkstemp errno=%d\n", errno);
        return false;
    }
    unlink(path);  // anonymous on disk
    constexpr size_t kBytes = 256 * 1024;
    if (ftruncate(fd, kBytes) != 0) {
        fprintf(stderr, "FAIL [file]: ftruncate errno=%d\n", errno);
        close(fd);
        return false;
    }
    void* p = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "FAIL [file]: file-backed mmap failed errno=%d\n", errno);
        close(fd);
        return false;
    }
    std::memset(p, 0x55, kBytes);
    sleep(2);
    // If smash had compressed this (it must not), msync would race
    // against the compressed-then-decompressed path.
    if (msync(p, kBytes, MS_SYNC) != 0) {
        fprintf(stderr, "FAIL [file]: msync errno=%d\n", errno);
        munmap(p, kBytes);
        close(fd);
        return false;
    }
    auto* b = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < kBytes; ++i) {
        if (b[i] != 0x55) {
            fprintf(stderr, "FAIL [file]: byte %zu = 0x%02x want 0x55\n",
                    i, b[i]);
            munmap(p, kBytes);
            close(fd);
            return false;
        }
    }
    munmap(p, kBytes);
    close(fd);
    fprintf(stderr, "external_mapping_test: file-backed PASSED\n");
    return true;
}

// Negative: PROT_READ-only mmap is filtered out. Mostly we check that
// it doesn't crash on the read path (a tracked + later-compressed RO
// page would fault on read; without tracking, no fault).
bool runReadOnlyTest(size_t page) {
    constexpr size_t kBytes = 64 * 1024;
    // Use MAP_PRIVATE|MAP_ANON so the kernel zeros it; PROT_READ then.
    void* p = mmap(nullptr, kBytes, PROT_READ,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "FAIL [ro]: mmap returned MAP_FAILED\n");
        return false;
    }
    sleep(2);
    auto* b = static_cast<const unsigned char*>(p);
    size_t pages = kBytes / page;
    for (size_t i = 0; i < pages; ++i) {
        if (b[i * page] != 0) {
            fprintf(stderr, "FAIL [ro]: page %zu byte 0 = 0x%02x want 0\n",
                    i, b[i * page]);
            munmap(p, kBytes);
            return false;
        }
    }
    munmap(p, kBytes);
    fprintf(stderr, "external_mapping_test: read-only PASSED\n");
    return true;
}

#ifdef __APPLE__
bool runMachVmTest(size_t page) {
    constexpr size_t kBytes = 4 * 1024 * 1024;  // 4 MiB
    mach_vm_address_t va = 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &va, kBytes,
                                         VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "FAIL [mach_vm]: allocate kr=%d\n", kr);
        return false;
    }
    void* p = reinterpret_cast<void*>(va);
    fillPattern(p, kBytes, page);

    sleep(3);

    if (!checkPattern(p, kBytes, page, "mach_vm")) {
        mach_vm_deallocate(mach_task_self(), va, kBytes);
        return false;
    }
    kr = mach_vm_deallocate(mach_task_self(), va, kBytes);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "FAIL [mach_vm]: deallocate kr=%d\n", kr);
        return false;
    }
    fprintf(stderr, "external_mapping_test: mach_vm PASSED\n");
    return true;
}
#endif

}  // namespace

int main() {
    size_t page = pageSize();

    if (!runMmapTest(page)) return 1;
    if (!runFileBackedTest()) return 1;
    if (!runReadOnlyTest(page)) return 1;
#ifdef __APPLE__
    if (!runMachVmTest(page)) return 1;
#endif

    fprintf(stderr, "external_mapping_test: ALL PASSED\n");
    return 0;
}
