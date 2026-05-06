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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

constexpr size_t kBufBytes  = 16 * 1024 * 1024;  // 16 MiB
constexpr size_t kSleepSec  = 3;                  // ≥ 2 × COLD_TIMEOUT_SEC=1
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

    // ── Phase 5: send + recv via socketpair ───────────────────────────────
    // Tests two interposed syscalls in a single round-trip. Use a small
    // message that fits in the default SO_SNDBUF (typically 8 KiB on
    // macOS), so blocking send completes in one shot, followed by blocking
    // recv. Send reads from compressed buf; recv writes into compressed
    // recv_buf — both kernel-side buffer touches.
    constexpr size_t kMsgBytes = 4096;  // single page, fits any SO_SNDBUF
    fillCompressible(buf, kBufBytes, 0xDD);
    auto* recv_buf = static_cast<unsigned char*>(std::malloc(kBufBytes));
    if (!recv_buf) { fprintf(stderr, "FAIL: malloc recv_buf\n"); return 1; }
    fillCompressible(recv_buf, kBufBytes, 0x99);

    if (!waitForCompression("pre-send/recv")) return 1;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        fprintf(stderr, "FAIL: socketpair errno=%d\n", errno);
        return 1;
    }
    ssize_t s_n = send(sv[0], buf, kMsgBytes, 0);
    if (s_n != static_cast<ssize_t>(kMsgBytes)) {
        fprintf(stderr, "FAIL: send() returned %zd errno=%d (%s)\n",
                s_n, errno, std::strerror(errno));
        return 1;
    }
    ssize_t r_n = recv(sv[1], recv_buf, kMsgBytes, MSG_WAITALL);
    if (r_n != static_cast<ssize_t>(kMsgBytes)) {
        fprintf(stderr, "FAIL: recv() returned %zd errno=%d (%s)\n",
                r_n, errno, std::strerror(errno));
        return 1;
    }
    close(sv[0]);
    close(sv[1]);
    if (std::memcmp(buf, recv_buf, kMsgBytes) != 0) {
        fprintf(stderr, "FAIL: send/recv roundtrip data mismatch\n");
        return 1;
    }
    fprintf(stderr, "syscall_efault_test: send/recv via socketpair PASSED\n");

    // ── Phase 6: pread() into compressed buffer ───────────────────────────
    // Same hazard as read() but the kernel goes through the SYS_pread path
    // — separate wrapper, separate code path through retryWithDecompress.
    fillCompressible(buf, kBufBytes, 0xEE);
    if (!waitForCompression("pre-pread")) return 1;

    char tmp_path[] = "/tmp/smash-efault-pread-XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) { fprintf(stderr, "FAIL: mkstemp\n"); return 1; }
    unlink(tmp_path);
    // Write a deterministic 1 MiB into the temp file.
    constexpr size_t kPreadBytes = 1 * 1024 * 1024;
    auto* src = static_cast<unsigned char*>(std::malloc(kPreadBytes));
    if (!src) { fprintf(stderr, "FAIL: malloc src\n"); return 1; }
    for (size_t i = 0; i < kPreadBytes; ++i)
        src[i] = static_cast<unsigned char>((i * 31) & 0xff);
    if (write(tmp_fd, src, kPreadBytes) != static_cast<ssize_t>(kPreadBytes)) {
        fprintf(stderr, "FAIL: write to temp file\n");
        return 1;
    }
    // Now pread back into the (still potentially compressed) buf.
    size_t pread_total = 0;
    while (pread_total < kPreadBytes) {
        ssize_t n = pread(tmp_fd, buf + pread_total,
                          kPreadBytes - pread_total,
                          static_cast<off_t>(pread_total));
        if (n < 0) {
            fprintf(stderr, "FAIL: pread() errno=%d (%s)\n",
                    errno, std::strerror(errno));
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "FAIL: pread returned 0 mid-read\n");
            return 1;
        }
        pread_total += static_cast<size_t>(n);
    }
    close(tmp_fd);
    if (std::memcmp(buf, src, kPreadBytes) != 0) {
        fprintf(stderr, "FAIL: pread() data mismatch — kernel did not "
                "fully populate the buffer through the EFAULT path\n");
        return 1;
    }
    std::free(src);
    fprintf(stderr, "syscall_efault_test: pread() into compressed buffer PASSED\n");

    // ── Phase 7: poll() with heap-allocated pollfd array ──────────────────
    // The pollfd array itself sits in heap memory. The kernel writes
    // back revents when a fd becomes ready. If the array is on a
    // compressed page, the kernel write hits EFAULT → retry → walk →
    // succeed.
    constexpr size_t kPollFdCount = 1024;  // ~16 KiB — at least one full page
    auto* fds = static_cast<struct pollfd*>(
        std::calloc(kPollFdCount, sizeof(struct pollfd)));
    if (!fds) { fprintf(stderr, "FAIL: calloc fds\n"); return 1; }
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
        fprintf(stderr, "FAIL: socketpair (poll)\n"); return 1;
    }
    // Make the first slot a real fd that is writable (always ready);
    // the rest are dup'd no-op fds at -1 (poll skips them).
    fds[0].fd = sp[0];
    fds[0].events = POLLOUT;
    for (size_t i = 1; i < kPollFdCount; ++i) {
        fds[i].fd = -1;  // skipped
        fds[i].events = 0;
    }
    if (!waitForCompression("pre-poll")) return 1;

    int poll_ret = poll(fds, kPollFdCount, /*timeout_ms=*/100);
    if (poll_ret < 0) {
        fprintf(stderr, "FAIL: poll() errno=%d (%s)\n",
                errno, std::strerror(errno));
        return 1;
    }
    if (!(fds[0].revents & POLLOUT)) {
        fprintf(stderr, "FAIL: poll() did not write revents into compressed "
                "pollfd array (revents=0x%x)\n", fds[0].revents);
        return 1;
    }
    close(sp[0]);
    close(sp[1]);
    std::free(fds);
    fprintf(stderr, "syscall_efault_test: poll() with heap pollfd PASSED\n");

    // ── Phase 8: recvmsg/sendmsg via socketpair (multi-iovec) ─────────────
    // Like phase 5 but driven through the iovec walk path of recvmsg/
    // sendmsg's rewarm callback. Small total to fit in default SO_SNDBUF.
    constexpr size_t kIovSlice = 1024;          // 1 KiB per slot
    constexpr size_t kIovTotal = kIovSlice * 4; // 4 KiB across 4 iovecs
    fillCompressible(buf, kBufBytes, 0x77);
    fillCompressible(recv_buf, kBufBytes, 0x88);
    if (!waitForCompression("pre-recvmsg/sendmsg")) return 1;

    int sm[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sm) < 0) {
        fprintf(stderr, "FAIL: socketpair (msg)\n"); return 1;
    }
    iovec siov[4];
    for (int i = 0; i < 4; ++i) {
        siov[i].iov_base = buf + i * kIovSlice;
        siov[i].iov_len  = kIovSlice;
    }
    iovec riov[4];
    for (int i = 0; i < 4; ++i) {
        riov[i].iov_base = recv_buf + i * kIovSlice;
        riov[i].iov_len  = kIovSlice;
    }
    msghdr smsg{}; smsg.msg_iov = siov; smsg.msg_iovlen = 4;
    msghdr rmsg{}; rmsg.msg_iov = riov; rmsg.msg_iovlen = 4;
    ssize_t sm_n = sendmsg(sm[0], &smsg, 0);
    if (sm_n != static_cast<ssize_t>(kIovTotal)) {
        fprintf(stderr, "FAIL: sendmsg() returned %zd errno=%d (%s)\n",
                sm_n, errno, std::strerror(errno));
        return 1;
    }
    ssize_t rm_n = recvmsg(sm[1], &rmsg, MSG_WAITALL);
    if (rm_n != static_cast<ssize_t>(kIovTotal)) {
        fprintf(stderr, "FAIL: recvmsg() returned %zd errno=%d (%s)\n",
                rm_n, errno, std::strerror(errno));
        return 1;
    }
    close(sm[0]);
    close(sm[1]);
    if (std::memcmp(buf, recv_buf, kIovTotal) != 0) {
        fprintf(stderr, "FAIL: sendmsg/recvmsg roundtrip mismatch\n");
        return 1;
    }
    fprintf(stderr, "syscall_efault_test: sendmsg/recvmsg via socketpair PASSED\n");

    // ── Phase 9: fstat() into heap-allocated struct stat ──────────────────
    // fstat is NOT historically interposed by smash. Now that the wrapper
    // pattern is trivial (5 lines of retryWithDecompress), we add it.
    // The kernel writes a struct stat into a userspace buffer; if that
    // buffer is on a compressed page, EFAULT.
    // Allocate the struct stat *inside* the big heap region so it shares
    // pages with the rest of buf — guarantees the page is compressed.
    auto* st = reinterpret_cast<struct stat*>(buf);
    std::memset(st, 0, sizeof(struct stat));
    fillCompressible(buf + sizeof(struct stat),
                     kBufBytes - sizeof(struct stat), 0x55);
    if (!waitForCompression("pre-fstat")) return 1;

    int fstat_fd = open("/dev/null", O_RDONLY);
    if (fstat_fd < 0) { fprintf(stderr, "FAIL: open /dev/null for fstat\n"); return 1; }
    if (fstat(fstat_fd, st) != 0) {
        fprintf(stderr, "FAIL: fstat() errno=%d (%s) — wrapper missing or "
                "EFAULT path broken\n", errno, std::strerror(errno));
        return 1;
    }
    close(fstat_fd);
    if (st->st_mode == 0) {
        fprintf(stderr, "FAIL: fstat() returned 0 but st_mode==0 — kernel "
                "did not actually populate the struct\n");
        return 1;
    }
    fprintf(stderr, "syscall_efault_test: fstat() with heap struct stat PASSED\n");

    std::free(recv_buf);
    std::free(buf);
    fprintf(stderr, "syscall_efault_test: ALL PASSED\n");
    return 0;
}
