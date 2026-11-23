// io_uring fixed buffers under SMASH_TRACK_EXTERNAL=1 (Linux, LD_PRELOAD).
//
// IORING_REGISTER_BUFFERS pins the buffer's pages (FOLL_LONGTERM); later
// READ_FIXED / WRITE_FIXED requests use the pinned frames directly and never
// go through the app's page tables. A pinned page that smash reclaims the
// classic way (PROT_NONE + MADV_DONTNEED) loses its mapping while the kernel
// keeps writing the old frame: the app then reads the stale decompressed
// blob, not the I/O.
//
//   pin       register the buffer with a RAW io_uring_register syscall (what
//             liburing does; libsmash never sees it) while it is resident,
//             let it go cold, READ_FIXED a file into it: the app must see
//             the file's bytes. In uffd mode smash reclaims external pages
//             with UFFDIO_MOVE, which the kernel refuses for a maybe-pinned
//             page (EBUSY); the shadow copy is discarded. The classic path
//             has no way to see the pin (exits 77 there: a documented
//             hazard, see agents/external-tracking.md).
//   liburing  (built when liburing is found) register a buffer smash has
//             ALREADY compressed through liburing's io_uring_register_buffers,
//             which libsmash interposes: the registration must succeed (it
//             used to fail with EFAULT) and READ_FIXED must land.
//
//   iouring_pin_test [pin|liburing]     exit 77 = skipped
#include <fcntl.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef SMASH_TEST_LIBURING
#include <liburing.h>
#endif

namespace {

size_t g_page = 4096;
constexpr size_t kLen = 1u << 20;

double nowSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void* mapAnon(size_t len) {
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

void fill(void* p, size_t len, unsigned seed) {
    auto* b = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < len; ++i) b[i] = static_cast<unsigned char>(((i / 64) * 7 + seed) | 1);
}
bool check(const void* p, size_t len, unsigned seed) {
    auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < len; ++i)
        if (b[i] != static_cast<unsigned char>(((i / 64) * 7 + seed) | 1)) {
            fprintf(stderr, "  mismatch at byte %zu (page %zu)\n", i, i / g_page);
            return false;
        }
    return true;
}

// Non-resident pages (uffd reclaim) or PROT_NONE (classic) of a filled region.
size_t compressedPages(const void* base, size_t len) {
    static int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    const size_t np = len / g_page;
    std::vector<uint64_t> e(np);
    if (fd < 0 || pread(fd, e.data(), np * 8, static_cast<off_t>(reinterpret_cast<uintptr_t>(base) / g_page * 8)) !=
                      static_cast<ssize_t>(np * 8))
        return 0;
    size_t n = 0;
    for (uint64_t v : e) n += !(v & (3ULL << 62));
    return n;
}

bool uffdActive() {
    char path[64], link[128];
    for (int fd = 0; fd < 1024; ++fd) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, link, sizeof(link) - 1);
        if (n <= 0) continue;
        link[n] = 0;
        if (std::strstr(link, "userfaultfd")) return true;
    }
    return false;
}

// Wait until a control region filled now is half compressed (enough cold
// ticks have passed for the buffer under test too), plus a few reclaim ticks.
bool waitCold(void* ctl) {
    double t0 = nowSec();
    while (compressedPages(ctl, kLen) * 2 < kLen / g_page) {
        if (nowSec() - t0 > 40) return false;
        usleep(100 * 1000);
    }
    sleep(2);
    return true;
}

// File holding pattern `seed` over kLen bytes.
int patternFile(unsigned seed) {
    char path[] = "/tmp/smash-iouring-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    std::vector<unsigned char> b(kLen);
    fill(b.data(), kLen, seed);
    if (pwrite(fd, b.data(), kLen, 0) != static_cast<ssize_t>(kLen)) { close(fd); return -1; }
    return fd;
}

// Minimal raw io_uring: one READ_FIXED at a time.
struct Ring {
    int fd = -1;
    unsigned *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask;
    io_uring_sqe* sqes;
    io_uring_cqe* cqes;
};
bool ringInit(Ring& r) {
    io_uring_params p{};
    r.fd = static_cast<int>(syscall(__NR_io_uring_setup, 4, &p));
    if (r.fd < 0) return false;
    const size_t sq_len = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    const size_t cq_len = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
    auto* sq = static_cast<char*>(mmap(nullptr, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r.fd,
                                       IORING_OFF_SQ_RING));
    auto* cq = static_cast<char*>(mmap(nullptr, cq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r.fd,
                                       IORING_OFF_CQ_RING));
    r.sqes = static_cast<io_uring_sqe*>(mmap(nullptr, p.sq_entries * sizeof(io_uring_sqe), PROT_READ | PROT_WRITE,
                                             MAP_SHARED | MAP_POPULATE, r.fd, IORING_OFF_SQES));
    if (sq == MAP_FAILED || cq == MAP_FAILED || r.sqes == MAP_FAILED) return false;
    r.sq_tail = reinterpret_cast<unsigned*>(sq + p.sq_off.tail);
    r.sq_mask = reinterpret_cast<unsigned*>(sq + p.sq_off.ring_mask);
    r.sq_array = reinterpret_cast<unsigned*>(sq + p.sq_off.array);
    r.cq_head = reinterpret_cast<unsigned*>(cq + p.cq_off.head);
    r.cq_tail = reinterpret_cast<unsigned*>(cq + p.cq_off.tail);
    r.cq_mask = reinterpret_cast<unsigned*>(cq + p.cq_off.ring_mask);
    r.cqes = reinterpret_cast<io_uring_cqe*>(cq + p.cq_off.cqes);
    return true;
}
int readFixed(Ring& r, int file, void* buf, unsigned len) {
    const unsigned tail = *r.sq_tail, idx = tail & *r.sq_mask;
    io_uring_sqe* e = &r.sqes[idx];
    std::memset(e, 0, sizeof(*e));
    e->opcode = IORING_OP_READ_FIXED;
    e->fd = file;
    e->addr = reinterpret_cast<uint64_t>(buf);
    e->len = len;
    e->off = 0;
    e->buf_index = 0;
    r.sq_array[idx] = idx;
    __atomic_store_n(r.sq_tail, tail + 1, __ATOMIC_RELEASE);
    if (syscall(__NR_io_uring_enter, r.fd, 1, 1, IORING_ENTER_GETEVENTS, nullptr, 0) < 0) return -errno;
    const unsigned head = __atomic_load_n(r.cq_head, __ATOMIC_ACQUIRE);
    if (head == __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE)) return -EAGAIN;
    const int res = r.cqes[head & *r.cq_mask].res;
    __atomic_store_n(r.cq_head, head + 1, __ATOMIC_RELEASE);
    return res;
}

int testPin() {
    if (!uffdActive()) {
        fprintf(stderr, "iouring_pin_test pin: SKIPPED (smash not in uffd mode: the classic reclaim "
                        "cannot see a pinned page)\n");
        return 77;
    }
    Ring r;
    if (!ringInit(r)) {
        fprintf(stderr, "iouring_pin_test pin: SKIPPED (io_uring unavailable, errno=%d)\n", errno);
        return 77;
    }
    void* buf = mapAnon(kLen);
    fill(buf, kLen, 3);
    iovec iov{buf, kLen};
    if (syscall(__NR_io_uring_register, r.fd, IORING_REGISTER_BUFFERS, &iov, 1) != 0) {
        fprintf(stderr, "iouring_pin_test pin: FAIL register errno=%d\n", errno);
        return 1;
    }
    void* ctl = mapAnon(kLen);
    fill(ctl, kLen, 4);
    if (!waitCold(ctl)) { fprintf(stderr, "iouring_pin_test pin: FAIL control never compressed\n"); return 1; }
    const size_t comp = compressedPages(buf, kLen);
    const int f = patternFile(9);
    if (f < 0) { fprintf(stderr, "iouring_pin_test pin: FAIL tmp file\n"); return 1; }
    const int res = readFixed(r, f, buf, kLen);
    if (res != static_cast<int>(kLen)) {
        fprintf(stderr, "iouring_pin_test pin: FAIL READ_FIXED res=%d\n", res);
        return 1;
    }
    if (!check(buf, kLen, 9)) {
        fprintf(stderr, "iouring_pin_test pin: FAIL the app does not see the READ_FIXED data "
                        "(%zu pinned pages were reclaimed)\n", comp);
        return 1;
    }
    if (!check(ctl, kLen, 4)) { fprintf(stderr, "iouring_pin_test pin: FAIL control corrupted\n"); return 1; }
    fprintf(stderr, "iouring_pin_test pin: PASS (%zu of %zu pinned pages non-resident before the read)\n",
            comp, kLen / g_page);
    return comp == 0 ? 0 : 1;
}

#ifdef SMASH_TEST_LIBURING
int testLiburing() {
    io_uring ring;
    if (io_uring_queue_init(4, &ring, 0) != 0) {
        fprintf(stderr, "iouring_pin_test liburing: SKIPPED (io_uring unavailable)\n");
        return 77;
    }
    void* buf = mapAnon(kLen);
    fill(buf, kLen, 5);
    if (!waitCold(buf)) { fprintf(stderr, "iouring_pin_test liburing: FAIL never compressed\n"); return 1; }
    const size_t before = compressedPages(buf, kLen);
    iovec iov{buf, kLen};
    const int rc = io_uring_register_buffers(&ring, &iov, 1);
    if (rc != 0) {
        fprintf(stderr, "iouring_pin_test liburing: FAIL io_uring_register_buffers of a compressed "
                        "buffer = %d (%zu pages compressed)\n", rc, before);
        return 1;
    }
    if (!check(buf, kLen, 5)) { fprintf(stderr, "iouring_pin_test liburing: FAIL data lost\n"); return 1; }
    void* ctl = mapAnon(kLen);
    fill(ctl, kLen, 6);
    if (!waitCold(ctl)) { fprintf(stderr, "iouring_pin_test liburing: FAIL control never compressed\n"); return 1; }
    const size_t after = compressedPages(buf, kLen);
    const int f = patternFile(11);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read_fixed(sqe, f, buf, kLen, 0, 0);
    io_uring_submit(&ring);
    io_uring_cqe* cqe = nullptr;
    if (io_uring_wait_cqe(&ring, &cqe) != 0 || cqe->res != static_cast<int>(kLen)) {
        fprintf(stderr, "iouring_pin_test liburing: FAIL READ_FIXED res=%d\n", cqe ? cqe->res : -1);
        return 1;
    }
    io_uring_cqe_seen(&ring, cqe);
    if (!check(buf, kLen, 11)) {
        fprintf(stderr, "iouring_pin_test liburing: FAIL READ_FIXED data not visible (%zu pages reclaimed)\n", after);
        return 1;
    }
    fprintf(stderr, "iouring_pin_test liburing: PASS (%zu pages compressed before registering, %zu after)\n",
            before, after);
    return after == 0 ? 0 : 1;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) g_page = static_cast<size_t>(ps);
    const char* c = argc > 1 ? argv[1] : "pin";
    if (std::strcmp(c, "pin") == 0) return testPin();
#ifdef SMASH_TEST_LIBURING
    if (std::strcmp(c, "liburing") == 0) return testLiburing();
#endif
    fprintf(stderr, "iouring_pin_test: case %s not built; SKIPPED\n", c);
    return 77;
}
