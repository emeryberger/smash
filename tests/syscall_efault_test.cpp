// Verify the EFAULT-driven decompress-and-retry path actually works:
// allocate a buffer, force the compressor to compress its pages, then
// issue real syscalls (read, write, readv, writev) that pass kernel-level
// buffer access. If retryWithDecompress / retryMachOnInvalidData work,
// the kernel's EFAULT is caught, the handler decompresses on the
// userspace re-touch, and the syscall succeeds. If they do NOT work,
// the syscall returns -1 with errno=EFAULT and the test fails.
//
// Run under DYLD_INSERT_LIBRARIES=...libsmash.dylib (macOS) or
// LD_PRELOAD=...libsmash.so (Linux). Required env:
//   SMASH_LARGE_ONLY=0
//   SMASH_COLD_TIMEOUT_SEC=1
//   SMASH_DEFER_PHASES_MS=0
//
// Test sequence:
//   1. Allocate a ~16 MiB buffer of compressible data.
//   2. Sleep ≥ 2 × COLD_TIMEOUT_SEC; kick SIGUSR2 to confirm compressed > 0.
//   3. read(/dev/urandom, buf, ...): kernel writes random bytes into buf.
//      With pages still potentially compressed, the kernel hits EFAULT;
//      our wrapper retries. Verify the read returned the requested size
//      AND the contents differ from the original deterministic pattern
//      (proving the kernel actually wrote into the user-visible region).
//   4. Recompress (sleep again, SIGUSR2 confirm), then writev() FROM the
//      buffer into /dev/null in two iovec slices. Verify the write
//      returned the full length (kernel read all bytes; if EFAULT-retry
//      were broken, write would return short).
//   5. Recompress, then readv() to fill two iovec buffers from
//      /dev/urandom. Verify both buffers got distinct data (proving
//      both iovec slots were touched, exercising the iovec walk path).

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/uio.h>
#include <unistd.h>

namespace {

constexpr size_t kBufBytes  = 16 * 1024 * 1024;  // 16 MiB
constexpr size_t kSleepSec  = 5;                  // ≥ 2 × COLD_TIMEOUT_SEC=1
constexpr size_t kSliceSize = 4 * 1024 * 1024;    // 4 MiB per iovec slot

void fillCompressible(unsigned char* p, size_t bytes, unsigned char tag) {
    // Single byte repeated — every algorithm collapses this to ~tens of bytes.
    std::memset(p, tag, bytes);
}

// Capture the next SIGUSR2 stats line emitted by smash to stderr.
std::string captureSigusr2Stats() {
    char path[] = "/tmp/smash-syscall-efault-stats-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return {};
    unlink(path);
    int saved = dup(2);
    if (saved < 0) { close(fd); return {}; }
    fflush(stderr);
    if (dup2(fd, 2) < 0) { close(saved); close(fd); return {}; }

    kill(getpid(), SIGUSR2);
    fsync(fd);

    fflush(stderr);
    dup2(saved, 2);
    close(saved);

    lseek(fd, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

long parseField(const std::string& s, const char* needle) {
    auto pos = s.find(needle);
    if (pos == std::string::npos) return -1;
    pos += std::strlen(needle);
    char* end = nullptr;
    long v = std::strtol(s.c_str() + pos, &end, 10);
    if (end == s.c_str() + pos) return -1;
    return v;
}

bool waitForCompression(const char* phase) {
    fprintf(stderr, "syscall_efault_test: [%s] sleeping %zus for compressor\n",
            phase, kSleepSec);
    sleep(kSleepSec);
    std::string stats = captureSigusr2Stats();
    long compressed = parseField(stats, "compressed=");
    long committed  = parseField(stats, "committed=");
    fprintf(stderr,
            "syscall_efault_test: [%s] compressed=%ld committed=%ld\n",
            phase, compressed, committed);
    if (compressed <= 0) {
        fprintf(stderr,
                "FAIL: [%s] compressor never compressed any page; cannot "
                "exercise EFAULT path\n",
                phase);
        return false;
    }
    return true;
}

bool bytesAllEqual(const unsigned char* p, size_t bytes, unsigned char want) {
    for (size_t i = 0; i < bytes; ++i)
        if (p[i] != want) return false;
    return true;
}

}  // namespace

int main() {
    // ── Phase 1: allocate + fill ──────────────────────────────────────────
    auto* buf = static_cast<unsigned char*>(std::malloc(kBufBytes));
    if (!buf) { fprintf(stderr, "FAIL: malloc\n"); return 1; }
    fillCompressible(buf, kBufBytes, 0xAA);

    // Warmup: nudge smash past its compression-start threshold.
    for (int i = 0; i < 5000; ++i) {
        void* p = std::malloc(64);
        if (p) std::free(p);
    }

    // ── Phase 2: read() into compressed buffer ────────────────────────────
    if (!waitForCompression("pre-read")) return 1;

    int urand = open("/dev/urandom", O_RDONLY);
    if (urand < 0) { fprintf(stderr, "FAIL: open /dev/urandom\n"); return 1; }

    // Read a chunk from /dev/urandom. The kernel writes into our buffer
    // — exactly the protected-page hazard the EFAULT wrapper exists for.
    constexpr size_t kReadBytes = 1 * 1024 * 1024;  // 1 MiB
    size_t total = 0;
    while (total < kReadBytes) {
        ssize_t n = read(urand, buf + total, kReadBytes - total);
        if (n < 0) {
            fprintf(stderr,
                    "FAIL: read() returned -1 errno=%d (%s) — EFAULT-retry "
                    "wrapper broken? Kernel hit a compressed page and the "
                    "userspace retry path failed to recover.\n",
                    errno, std::strerror(errno));
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "FAIL: unexpected EOF on /dev/urandom\n");
            return 1;
        }
        total += static_cast<size_t>(n);
    }
    close(urand);

    // Sanity-check: the read region should NOT be all 0xAA anymore.
    if (bytesAllEqual(buf, kReadBytes, 0xAA)) {
        fprintf(stderr,
                "FAIL: read() returned %zu bytes but buffer still contains "
                "the pre-read pattern. The kernel did not actually write "
                "into our pages — the wrapper masked an EFAULT silently.\n",
                total);
        return 1;
    }
    fprintf(stderr, "syscall_efault_test: read() into compressed buffer PASSED\n");

    // Refill the read region so the next phase has something compressible.
    fillCompressible(buf, kBufBytes, 0xBB);

    // ── Phase 3: writev() FROM compressed buffer ──────────────────────────
    if (!waitForCompression("pre-writev")) return 1;

    int devnull_w = open("/dev/null", O_WRONLY);
    if (devnull_w < 0) { fprintf(stderr, "FAIL: open /dev/null write\n"); return 1; }

    iovec wiov[2];
    wiov[0].iov_base = buf;
    wiov[0].iov_len  = kSliceSize;
    wiov[1].iov_base = buf + kSliceSize;
    wiov[1].iov_len  = kSliceSize;
    ssize_t wrote = writev(devnull_w, wiov, 2);
    if (wrote < 0) {
        fprintf(stderr,
                "FAIL: writev() returned -1 errno=%d (%s) — EFAULT iovec "
                "walk in retry callback broken?\n",
                errno, std::strerror(errno));
        return 1;
    }
    if (static_cast<size_t>(wrote) != 2 * kSliceSize) {
        fprintf(stderr,
                "FAIL: writev() short write %zd of expected %zu — kernel "
                "stopped reading mid-iovec, suggests EFAULT was returned "
                "instead of being retried.\n",
                wrote, 2 * kSliceSize);
        return 1;
    }
    close(devnull_w);
    fprintf(stderr, "syscall_efault_test: writev() from compressed buffer PASSED\n");

    // Refill again.
    fillCompressible(buf, kBufBytes, 0xCC);

    // ── Phase 4: readv() into two compressed iovec slots ──────────────────
    if (!waitForCompression("pre-readv")) return 1;

    int urand2 = open("/dev/urandom", O_RDONLY);
    if (urand2 < 0) { fprintf(stderr, "FAIL: open /dev/urandom #2\n"); return 1; }

    // /dev/urandom may return short on a single readv(); loop until both
    // slices are filled, rebuilding the iovec each pass to skip already
    // satisfied slots.
    size_t filled[2] = {0, 0};
    while (filled[0] < kSliceSize || filled[1] < kSliceSize) {
        iovec live[2];
        int nlive = 0;
        if (filled[0] < kSliceSize) {
            live[nlive].iov_base = buf + filled[0];
            live[nlive].iov_len  = kSliceSize - filled[0];
            ++nlive;
        }
        size_t base1 = (filled[0] >= kSliceSize) ? (kSliceSize + filled[1]) : 0;
        if (filled[1] < kSliceSize) {
            live[nlive].iov_base = buf + (kSliceSize + filled[1]);
            live[nlive].iov_len  = kSliceSize - filled[1];
            ++nlive;
            (void)base1;
        }
        ssize_t n = readv(urand2, live, nlive);
        if (n < 0) {
            fprintf(stderr,
                    "FAIL: readv() returned -1 errno=%d (%s)\n",
                    errno, std::strerror(errno));
            return 1;
        }
        // Split n across slices in order.
        for (int k = 0; k < nlive && n > 0; ++k) {
            size_t take = std::min(static_cast<size_t>(n), live[k].iov_len);
            if (live[k].iov_base == buf + filled[0])
                filled[0] += take;
            else
                filled[1] += take;
            n -= static_cast<ssize_t>(take);
        }
    }
    close(urand2);

    if (bytesAllEqual(buf, kSliceSize, 0xCC) ||
        bytesAllEqual(buf + kSliceSize, kSliceSize, 0xCC)) {
        fprintf(stderr,
                "FAIL: readv() returned but at least one iovec slot still "
                "contains the pre-read pattern.\n");
        return 1;
    }
    fprintf(stderr, "syscall_efault_test: readv() into compressed buffer PASSED\n");

    std::free(buf);
    fprintf(stderr, "syscall_efault_test: ALL PASSED\n");
    return 0;
}
