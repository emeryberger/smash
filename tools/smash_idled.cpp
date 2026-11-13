// smash-idled: root helper that gives unprivileged smash processes per-page
// read tracking via the kernel idle-page bitmap. Protocol and safety rules:
// include/smash/idled_protocol.h.
//
//   sudo smash-idled [-s /run/smash-idled.sock] [-p PID] [-m MODE] [-v]
//
// -p: the only process (with its descendants) the daemon will serve.
// Default: the daemon's parent, or the parent's parent when that is a
// privilege wrapper (sudo, doas, su, pkexec). The socket is chowned to that
// process's uid with mode 0600 unless -m is given.
//
// Each connection gets a thread. The client is identified by SO_PEERCRED;
// its pid is pinned with SO_PEERPIDFD, and /proc/<pid>/pagemap is opened and
// then the pidfd is checked to be alive, so a recycled pid can never hand a
// client another process's pagemap. Requests touch only pages inside the
// client's registered range, and only present + anonymous + exclusively
// mapped ones.
#include "smash/idled_protocol.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

using namespace smash::idled;

namespace {

constexpr int kMaxClients = 256;
std::atomic<int> g_clients{0};
int g_bitmap = -1;
bool g_verbose = false;

// The launcher: pid plus its start time (clock ticks since boot), so a
// recycled pid never matches.
pid_t g_root_pid = 0;
unsigned long long g_root_start = 0;
std::atomic<uint64_t> g_refused{0};

// /proc/<pid>/stat → ppid and starttime. comm may hold spaces and ')', so
// parse after the LAST ')'.
bool procStat(pid_t pid, pid_t* ppid, unsigned long long* start) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    const char* p = std::strrchr(buf, ')');
    if (!p) return false;
    // Fields after ')': 3 state, 4 ppid, ..., 22 starttime.
    int field = 2;
    long long pp = -1;
    unsigned long long st = 0;
    for (const char* q = p + 1; *q; ++q) {
        if (*q != ' ') continue;
        ++field;
        if (field == 4) pp = std::strtoll(q + 1, nullptr, 10);
        if (field == 22) { st = std::strtoull(q + 1, nullptr, 10); break; }
    }
    if (pp < 0 || field != 22) return false;
    *ppid = static_cast<pid_t>(pp);
    *start = st;
    return true;
}

std::string procComm(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return {};
    while (n > 0 && buf[n - 1] == '\n') --n;
    return std::string(buf, static_cast<size_t>(n));
}

// Is `pid` the launcher or one of its descendants? Walks the ppid chain.
// Each parent must have started no later than its child: if a process in
// the chain exited and its pid was reused while we walked, the newcomer
// started later and the walk fails closed. A client whose parent chain was
// broken (re-parented to init or a subreaper) is refused.
bool inScope(pid_t pid) {
    unsigned long long child_start = ~0ull;
    for (int depth = 0; depth < 4096 && pid > 0; ++depth) {
        pid_t pp = 0;
        unsigned long long st = 0;
        if (!procStat(pid, &pp, &st)) return false;
        if (st > child_start) return false;
        if (pid == g_root_pid) return st == g_root_start;
        child_start = st;
        pid = pp;
    }
    return false;
}

constexpr uint64_t kPfnMask = (1ull << 55) - 1;
constexpr uint64_t kExclusive = 1ull << 56, kFile = 1ull << 61, kPresent = 1ull << 63;

bool readFull(int fd, void* p, size_t n) {
    auto* c = static_cast<char*>(p);
    while (n) {
        ssize_t r = read(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        c += r; n -= static_cast<size_t>(r);
    }
    return true;
}
bool writeFull(int fd, const void* p, size_t n) {
    auto* c = static_cast<const char*>(p);
    while (n) {
        ssize_t r = send(fd, c, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        c += r; n -= static_cast<size_t>(r);
    }
    return true;
}

struct Client {
    int fd = -1, pagemap = -1;
    pid_t pid = 0;
    bool scoped = false;
    uint64_t base = 0, npages = 0;
    std::vector<uint32_t> idx;
    std::vector<uint64_t> pfn;   // per index; 0 = not eligible
    std::vector<uint8_t> absent; // per index; 1 = not present
    std::vector<uint8_t> out;
    std::vector<uint64_t> pm;    // pagemap window
    uint64_t armed = 0, asked = 0, queried = 0, idle = 0;

    // PFN of each requested page, 0 if the page is not present + anon +
    // exclusive. Indices arrive sorted (the client scans in order), so one
    // 512-entry pagemap read usually covers several.
    void resolve(uint32_t n) {
        constexpr uint64_t kWin = 512;
        pfn.assign(n, 0);
        absent.assign(n, 0);
        pm.resize(kWin);
        uint64_t w0 = ~0ull, wn = 0;
        for (uint32_t k = 0; k < n; ++k) {
            const uint64_t i = idx[k];
            if (i < w0 || i >= w0 + wn) {
                w0 = i;
                const uint64_t cnt = std::min<uint64_t>(kWin, npages - i);
                const off_t off = static_cast<off_t>(((base >> 12) + i) * 8);
                ssize_t r = pread(pagemap, pm.data(), cnt * 8, off);
                wn = r > 0 ? static_cast<uint64_t>(r) / 8 : 0;
                if (wn == 0) continue;
            }
            const uint64_t e = pm[i - w0];
            if (!(e & kPresent)) { absent[k] = 1; continue; }
            if ((e & kPresent) && (e & kExclusive) && !(e & kFile)) pfn[k] = e & kPfnMask;
        }
    }

    uint32_t arm(uint32_t n) {
        resolve(n);
        std::vector<uint64_t> p;
        p.reserve(n);
        for (uint32_t k = 0; k < n; ++k) if (pfn[k]) p.push_back(pfn[k]);
        std::sort(p.begin(), p.end());
        // Writing a bitmap word marks only its SET bits idle (zero bits are
        // ignored by the kernel), so no read-modify-write is needed.
        for (size_t a = 0; a < p.size();) {
            const uint64_t word = p[a] / 64;
            uint64_t bits = 0;
            while (a < p.size() && p[a] / 64 == word) bits |= 1ull << (p[a++] % 64);
            (void)!pwrite(g_bitmap, &bits, 8, static_cast<off_t>(word * 8));
        }
        armed += p.size(); asked += n;
        return static_cast<uint32_t>(p.size());
    }

    void query(uint32_t n) {
        resolve(n);
        out.assign(n, 1);
        // One bitmap read per page, as the in-process path does. Reusing a
        // word read for the next page in the same word looked equivalent but
        // measured 2-6x more refaults on Redis (2026-11-09; not explained).
        for (uint32_t k = 0; k < n; ++k) {
            if (!pfn[k]) { if (absent[k]) out[k] = 2; continue; }
            uint64_t v = 0;
            if (pread(g_bitmap, &v, 8, static_cast<off_t>((pfn[k] / 64) * 8)) != 8) continue;
            out[k] = ((v >> (pfn[k] % 64)) & 1) ? 0 : 1;
        }
        queried += n;
        for (uint32_t k = 0; k < n; ++k) idle += out[k] == 0;
    }

    bool identify() {
        struct ucred cr{};
        socklen_t len = sizeof(cr);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0 || cr.pid <= 0) return false;
        int pidfd = -1;
        len = sizeof(pidfd);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) != 0 || pidfd < 0) return false;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/pagemap", cr.pid);
        pagemap = open(path, O_RDONLY | O_CLOEXEC);
        // Pid pinned: if the peer had exited (and the pid been reused) before
        // the open, the pidfd would report it gone.
        const bool alive = syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0) == 0;
        // Ancestry is checked while the pidfd still pins the client, and
        // re-checked alive after, so the answer is about this process.
        scoped = alive && inScope(cr.pid) &&
                 syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0) == 0;
        close(pidfd);
        if (pagemap < 0 || !alive) return false;
        pid = cr.pid;
        return true;
    }

    // Out of scope: answer the first request with -EPERM (so the client can
    // say why) and hang up. Nothing of the client's memory is read.
    void refuse() {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "smash-idled: refused pid %d (not launcher %d or its descendant)\n", pid, g_root_pid);
        Req q{};
        if (!readFull(fd, &q, sizeof(q)) || q.magic != kMagic) return;
        Resp r{-EPERM, 0};
        (void)writeFull(fd, &r, sizeof(r));
    }

    void serve() {
        if (!identify()) return;
        if (!scoped) {
            if (pagemap >= 0) { close(pagemap); pagemap = -1; }
            refuse();
            pid = 0;  // no "done" stats line for a refused peer
            return;
        }
        if (g_verbose) std::fprintf(stderr, "smash-idled: client pid %d\n", pid);
        for (;;) {
            Req q{};
            if (!readFull(fd, &q, sizeof(q)) || q.magic != kMagic) return;
            Resp r{0, 0};
            if (q.op == kOpRegister) {
                uint64_t a[2];
                if (!readFull(fd, a, sizeof(a))) return;
                if ((a[0] & 4095) || a[1] == 0 || a[1] > kMaxPages) r.status = -EINVAL;
                else { base = a[0]; npages = a[1]; }
                if (!writeFull(fd, &r, sizeof(r))) return;
                continue;
            }
            if ((q.op != kOpArm && q.op != kOpQuery) || q.n > kMaxBatch) return;
            idx.resize(q.n);
            if (q.n && !readFull(fd, idx.data(), q.n * 4ull)) return;
            bool ok = npages != 0;
            for (uint32_t k = 0; ok && k < q.n; ++k) ok = idx[k] < npages;
            if (!ok) {
                r.status = -EINVAL;
                if (q.op == kOpQuery) { r.n = q.n; out.assign(q.n, 1); }
                if (!writeFull(fd, &r, sizeof(r)) || (r.n && !writeFull(fd, out.data(), r.n))) return;
                continue;
            }
            if (q.op == kOpArm) {
                r.n = arm(q.n);
                if (!writeFull(fd, &r, sizeof(r))) return;
            } else {
                query(q.n);
                r.n = q.n;
                if (!writeFull(fd, &r, sizeof(r)) || (q.n && !writeFull(fd, out.data(), q.n))) return;
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* sock = kDefaultSock;
    mode_t mode = 0600;
    bool mode_set = false;
    pid_t root = 0;
    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "-s") && a + 1 < argc) sock = argv[++a];
        else if (!std::strcmp(argv[a], "-m") && a + 1 < argc) { mode = static_cast<mode_t>(std::strtol(argv[++a], nullptr, 8)); mode_set = true; }
        else if (!std::strcmp(argv[a], "-p") && a + 1 < argc) root = static_cast<pid_t>(std::strtol(argv[++a], nullptr, 10));
        else if (!std::strcmp(argv[a], "-v")) g_verbose = true;
        else { std::fprintf(stderr, "usage: %s [-s socket] [-p pid] [-m mode] [-v]\n", argv[0]); return 2; }
    }
    if (root <= 0) {
        root = getppid();
        const std::string c = procComm(root);
        if (c == "sudo" || c == "doas" || c == "su" || c == "pkexec") {
            pid_t pp = 0;
            unsigned long long st = 0;
            if (procStat(root, &pp, &st) && pp > 1) root = pp;
        }
    }
    {
        pid_t pp = 0;
        if (root <= 1 || !procStat(root, &pp, &g_root_start)) {
            std::fprintf(stderr, "smash-idled: launcher pid %d not found (use -p)\n", root);
            return 1;
        }
        g_root_pid = root;
    }
    // Pin the launcher: readable when it exits.
    const int root_fd = static_cast<int>(syscall(SYS_pidfd_open, g_root_pid, 0));
    {
        pid_t pp = 0;
        unsigned long long st = 0;
        if (root_fd < 0 || !procStat(g_root_pid, &pp, &st) || st != g_root_start) {
            std::fprintf(stderr, "smash-idled: cannot pin launcher pid %d\n", g_root_pid);
            return 1;
        }
    }
    uid_t root_uid = 0;
    {
        struct stat sb{};
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d", g_root_pid);
        if (stat(path, &sb) == 0) root_uid = sb.st_uid;
    }
    g_bitmap = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR | O_CLOEXEC);
    if (g_bitmap < 0) { std::perror("smash-idled: page_idle/bitmap (need root + CONFIG_IDLE_PAGE_TRACKING)"); return 1; }
    std::signal(SIGPIPE, SIG_IGN);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (std::strlen(sock) >= sizeof(sa.sun_path)) { std::fprintf(stderr, "smash-idled: socket path too long\n"); return 1; }
    std::strcpy(sa.sun_path, sock);
    unlink(sock);
    // Owner = the launcher's uid, so with the default 0600 only that user
    // (and root) can even connect; the ancestry check applies regardless.
    if (ls < 0 || bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 ||
        (!mode_set && chown(sock, root_uid, static_cast<gid_t>(-1)) != 0) || chmod(sock, mode) != 0 ||
        listen(ls, 64) != 0) {
        std::perror("smash-idled: socket");
        unlink(sock);
        return 1;
    }
    if (g_verbose)
        std::fprintf(stderr, "smash-idled: listening on %s for pid %d (%s) and descendants\n", sock, g_root_pid,
                     procComm(g_root_pid).c_str());
    for (;;) {
        struct pollfd pf[2] = {{ls, POLLIN, 0}, {root_fd, POLLIN, 0}};
        if (poll(pf, 2, -1) < 0) { if (errno == EINTR) continue; std::perror("smash-idled: poll"); continue; }
        if (pf[1].revents) {
            // Launcher gone: nobody new can be in scope. Finish current clients.
            unlink(sock);
            close(ls);
            if (g_verbose) std::fprintf(stderr, "smash-idled: launcher %d exited; draining %d client(s)\n",
                                        g_root_pid, g_clients.load());
            while (g_clients.load() > 0) usleep(100000);
            if (g_verbose) std::fprintf(stderr, "smash-idled: exit (refused %llu)\n",
                                        (unsigned long long)g_refused.load());
            return 0;
        }
        if (!pf[0].revents) continue;
        int fd = accept4(ls, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; std::perror("smash-idled: accept"); continue; }
        if (g_clients.fetch_add(1) >= kMaxClients) { g_clients.fetch_sub(1); close(fd); continue; }
        std::thread([fd] {
            Client c;
            c.fd = fd;
            c.serve();
            if (g_verbose && c.pid)
                std::fprintf(stderr, "smash-idled: pid %d done: armed %llu/%llu, queried %llu (%llu idle)\n", c.pid,
                             (unsigned long long)c.armed, (unsigned long long)c.asked,
                             (unsigned long long)c.queried, (unsigned long long)c.idle);
            if (c.pagemap >= 0) close(c.pagemap);
            close(fd);
            g_clients.fetch_sub(1);
        }).detach();
    }
}
