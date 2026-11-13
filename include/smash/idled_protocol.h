// smash-idled wire protocol (Linux). A root daemon (tools/smash_idled.cpp)
// gives unprivileged smash processes per-page READ tracking through the
// kernel idle-page bitmap, which needs CAP_SYS_ADMIN twice: to see PFNs in
// pagemap and to open /sys/kernel/mm/page_idle/bitmap. PFNs never leave the
// daemon; a client only learns whether its own pages were accessed.
//
// Stream socket, little-endian, one request → one response, in order:
//   Req  { magic, op, n, 0 } + payload
//   REGISTER  payload: uint64 base, uint64 npages     resp: Resp{status, 0}
//   ARM       payload: n × uint32 page index (< npages) resp: Resp{status, armed}
//   QUERY     payload: n × uint32 page index           resp: Resp{status, n} + n bytes
//             (byte 1 = accessed since armed, or not eligible; 0 = still
//             idle; 2 = not present, i.e. nothing to read)
// Page index i is the page at base + i * 4096. The daemon arms/queries only
// pages that are present, anonymous and exclusively mapped in the client
// (a shared page's idle bit would mislead reclaim in other processes);
// anything else answers "accessed", which the client treats as hot.
//
// Scope: each daemon serves only the process that launched it (-p PID;
// by default its parent, looking through sudo/doas/su) and that process's
// descendants. Anyone else gets Resp{-EPERM} to its first request and is
// disconnected. When the launcher exits the daemon stops accepting, removes
// its socket, and exits once its last client disconnects.
#pragma once
#include <cstdint>

namespace smash::idled {

constexpr uint32_t kMagic = 0x534d4931;  // "SMI1"
constexpr uint32_t kOpRegister = 1, kOpArm = 2, kOpQuery = 3;
constexpr uint32_t kMaxBatch = 1u << 16;        // indices per request
constexpr uint64_t kMaxPages = 1ull << 28;      // 1 TiB of 4 KiB pages
constexpr const char* kDefaultSock = "/run/smash-idled.sock";

struct Req {
    uint32_t magic, op, n, pad;
};
struct Resp {
    int32_t status;  // 0 ok, <0 -errno
    uint32_t n;
};

}  // namespace smash::idled
