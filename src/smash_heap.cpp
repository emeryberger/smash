// smash/src/smash_heap.cpp - SmashHeap alloc8 integration + thread cache methods
#include "smash_heap.h"
#include "vm/syscall_compat.h"
#include <alloc8/alloc8.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>
#include <unistd.h>
#endif

// ── Thread init counter for deferred compression start ──────────────────────
// pthread_create during early DYLD_INSERT init crashes the ObjC runtime's
// task_restartable_ranges_register on macOS. We count threadInit() calls
// and only start compression after the second call (first = main thread
// during early init, subsequent = real threads after init is complete).

std::atomic<int> smash::g_thread_init_count{0};

// ── Thread cache methods that depend on Slab ─────────────────────────────────

namespace smash {

void* ThreadCache::refill(uint8_t sc, Slab* slab) {
    // Batch-allocate from slab into our cache
    auto& c = caches_[sc];
    size_t batch = kThreadCacheBatchSize;
    if (batch > kThreadCacheMaxPerClass) batch = kThreadCacheMaxPerClass;

    size_t got = slab->allocateBatch(c.ptrs, batch);
    if (got == 0) return nullptr;

    // Return one, keep the rest in cache
    c.count = static_cast<uint32_t>(got - 1);
    return c.ptrs[got - 1];
}

void ThreadCache::drain(uint8_t sc, Slab* all_slabs, PageMap* page_map) {
    auto& c = caches_[sc];
    // Drain half the cache
    size_t to_drain = c.count / 2;
    if (to_drain == 0) to_drain = c.count;
    size_t start = c.count - to_drain;

    // Bucket pointers by arena based on their span's arena_id
    // Max drained per call = ceil(kThreadCacheMaxPerClass/2)
    void* buckets[kNumArenas][kThreadCacheMaxPerClass];
    size_t counts[kNumArenas]{};
    for (size_t i = start; i < c.count; ++i) {
        Span* span = page_map->get(reinterpret_cast<uintptr_t>(c.ptrs[i]));
        uint8_t arena = (span && !span->is_large) ? span->arena_id : 0;
        buckets[arena][counts[arena]++] = c.ptrs[i];
    }
    for (int a = 0; a < kNumArenas; ++a) {
        if (counts[a] > 0)
            all_slabs[a * kNumClasses + sc].deallocateBatch(buckets[a], counts[a]);
    }
    c.count = static_cast<uint32_t>(start);
}

void ThreadCache::drainAll(Slab* all_slabs, PageMap* page_map) {
    for (int i = 0; i < kNumClasses; ++i) {
        auto& c = caches_[i];
        if (c.count > 0) {
            // Bucket by arena — each bucket may receive up to kThreadCacheMaxPerClass ptrs
            void* buckets[kNumArenas][kThreadCacheMaxPerClass];
            size_t counts[kNumArenas]{};
            for (size_t j = 0; j < c.count; ++j) {
                Span* span = page_map->get(reinterpret_cast<uintptr_t>(c.ptrs[j]));
                uint8_t arena = (span && !span->is_large) ? span->arena_id : 0;
                buckets[arena][counts[arena]++] = c.ptrs[j];
            }
            for (int a = 0; a < kNumArenas; ++a) {
                if (counts[a] > 0)
                    all_slabs[a * kNumClasses + i].deallocateBatch(buckets[a], counts[a]);
            }
            c.count = 0;
        }
    }
}

} // namespace smash

// ── Global VmRegion pointer for syscall wrappers ─────────────────────────────

smash::VmRegion* smash::g_smash_vm_region = nullptr;

// ── Syscall interposition for kernel buffer compatibility ────────────────────
//
// Kernel syscalls access userspace buffers directly without triggering SIGSEGV.
// We interpose on syscalls that read/write userspace buffers and touch each
// Smash-managed page first to ensure PROT_READ|PROT_WRITE.

#ifdef __APPLE__

// ── Interpose data structure (same pattern as alloc8) ────────────────────────

typedef struct {
    void* replacement;
    void* original;
} smash_interpose_t;

#define SMASH_INTERPOSE(replacement, original) \
    __attribute__((used)) \
    static const smash_interpose_t smash_interpose_##replacement \
    __attribute__((section("__DATA, __interpose"))) = { \
        reinterpret_cast<void*>(replacement), \
        reinterpret_cast<void*>(original) \
    }

// Helper: warm iovec array buffers
static inline void warmIovec(const struct iovec* iov, int iovcnt, smash::VmRegion* vm) {
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base && iov[i].iov_len)
            smash::vm::warmPages(iov[i].iov_base, iov[i].iov_len, vm);
    }
}

// To call the real (original) function from within an interpose wrapper, we
// read the .original field of the interpose struct. The linker resolves this
// to the real function address at load time, before dyld processes the section.
// This avoids dlsym(RTLD_NEXT) which returns our own wrapper on macOS.
//
// Pattern: forward-declare wrapper, define interpose entry, define wrapper
// body that reads interpose_entry.original.

// ── kevent ───────────────────────────────────────────────────────────────────

extern "C" int smash_kevent(int kq, const struct kevent* changelist, int nchanges,
                            struct kevent* eventlist, int nevents,
                            const struct timespec* timeout);
SMASH_INTERPOSE(smash_kevent, kevent);

using kevent_fn = int(*)(int, const struct kevent*, int, struct kevent*, int, const struct timespec*);

extern "C" int smash_kevent(int kq, const struct kevent* changelist, int nchanges,
                            struct kevent* eventlist, int nevents,
                            const struct timespec* timeout) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) {
        if (changelist && nchanges > 0) {
            smash::vm::warmPages(changelist, nchanges * sizeof(struct kevent), vm);
            smash::vm::pinPages(changelist, nchanges * sizeof(struct kevent), vm);
        }
        if (eventlist && nevents > 0) {
            smash::vm::warmPages(eventlist, nevents * sizeof(struct kevent), vm);
            smash::vm::pinPages(eventlist, nevents * sizeof(struct kevent), vm);
        }
    }
    int ret = reinterpret_cast<kevent_fn>(smash_interpose_smash_kevent.original)(
        kq, changelist, nchanges, eventlist, nevents, timeout);
    if (vm) {
        if (changelist && nchanges > 0)
            smash::vm::unpinPages(changelist, nchanges * sizeof(struct kevent), vm);
        if (eventlist && nevents > 0)
            smash::vm::unpinPages(eventlist, nevents * sizeof(struct kevent), vm);
    }
    return ret;
}

// ── recv / send ──────────────────────────────────────────────────────────────

using recv_fn = ssize_t(*)(int, void*, size_t, int);
using send_fn = ssize_t(*)(int, const void*, size_t, int);

extern "C" ssize_t smash_recv(int s, void* buf, size_t len, int flags);
SMASH_INTERPOSE(smash_recv, recv);
extern "C" ssize_t smash_recv(int s, void* buf, size_t len, int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = reinterpret_cast<recv_fn>(smash_interpose_smash_recv.original)(s, buf, len, flags);
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

extern "C" ssize_t smash_send(int s, const void* buf, size_t len, int flags);
SMASH_INTERPOSE(smash_send, send);
extern "C" ssize_t smash_send(int s, const void* buf, size_t len, int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = reinterpret_cast<send_fn>(smash_interpose_smash_send.original)(s, buf, len, flags);
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

// ── recvfrom / sendto ────────────────────────────────────────────────────────

using recvfrom_fn = ssize_t(*)(int, void*, size_t, int, struct sockaddr*, socklen_t*);
using sendto_fn = ssize_t(*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);

extern "C" ssize_t smash_recvfrom(int s, void* buf, size_t len, int flags,
                                   struct sockaddr* from, socklen_t* fromlen);
SMASH_INTERPOSE(smash_recvfrom, recvfrom);
extern "C" ssize_t smash_recvfrom(int s, void* buf, size_t len, int flags,
                                   struct sockaddr* from, socklen_t* fromlen) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = reinterpret_cast<recvfrom_fn>(smash_interpose_smash_recvfrom.original)(
        s, buf, len, flags, from, fromlen);
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

extern "C" ssize_t smash_sendto(int s, const void* buf, size_t len, int flags,
                                 const struct sockaddr* to, socklen_t tolen);
SMASH_INTERPOSE(smash_sendto, sendto);
extern "C" ssize_t smash_sendto(int s, const void* buf, size_t len, int flags,
                                 const struct sockaddr* to, socklen_t tolen) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::warmPages(buf, len, vm); smash::vm::pinPages(buf, len, vm); }
    ssize_t ret = reinterpret_cast<sendto_fn>(smash_interpose_smash_sendto.original)(
        s, buf, len, flags, to, tolen);
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

// ── recvmsg / sendmsg ────────────────────────────────────────────────────────

using recvmsg_fn = ssize_t(*)(int, struct msghdr*, int);
using sendmsg_fn = ssize_t(*)(int, const struct msghdr*, int);

extern "C" ssize_t smash_recvmsg(int s, struct msghdr* msg, int flags);
SMASH_INTERPOSE(smash_recvmsg, recvmsg);
extern "C" ssize_t smash_recvmsg(int s, struct msghdr* msg, int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && msg && msg->msg_iov && msg->msg_iovlen > 0) {
        warmIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
        smash::vm::pinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    }
    ssize_t ret = reinterpret_cast<recvmsg_fn>(smash_interpose_smash_recvmsg.original)(s, msg, flags);
    if (vm && msg && msg->msg_iov && msg->msg_iovlen > 0)
        smash::vm::unpinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    return ret;
}

extern "C" ssize_t smash_sendmsg(int s, const struct msghdr* msg, int flags);
SMASH_INTERPOSE(smash_sendmsg, sendmsg);
extern "C" ssize_t smash_sendmsg(int s, const struct msghdr* msg, int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && msg && msg->msg_iov && msg->msg_iovlen > 0) {
        warmIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
        smash::vm::pinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    }
    ssize_t ret = reinterpret_cast<sendmsg_fn>(smash_interpose_smash_sendmsg.original)(s, msg, flags);
    if (vm && msg && msg->msg_iov && msg->msg_iovlen > 0)
        smash::vm::unpinIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
    return ret;
}

// ── poll ─────────────────────────────────────────────────────────────────────

using poll_fn = int(*)(struct pollfd*, nfds_t, int);

extern "C" int smash_poll(struct pollfd* fds, nfds_t nfds, int timeout);
SMASH_INTERPOSE(smash_poll, poll);
extern "C" int smash_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && fds && nfds > 0) {
        smash::vm::warmPages(fds, nfds * sizeof(struct pollfd), vm);
        smash::vm::pinPages(fds, nfds * sizeof(struct pollfd), vm);
    }
    int ret = reinterpret_cast<poll_fn>(smash_interpose_smash_poll.original)(fds, nfds, timeout);
    if (vm && fds && nfds > 0)
        smash::vm::unpinPages(fds, nfds * sizeof(struct pollfd), vm);
    return ret;
}

// ── read / write ────────────────────────────────────────────────────────────

using read_fn = ssize_t(*)(int, void*, size_t);
using write_fn = ssize_t(*)(int, const void*, size_t);

extern "C" ssize_t smash_read(int fd, void* buf, size_t count);
SMASH_INTERPOSE(smash_read, read);
extern "C" ssize_t smash_read(int fd, void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = reinterpret_cast<read_fn>(smash_interpose_smash_read.original)(fd, buf, count);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t smash_write(int fd, const void* buf, size_t count);
SMASH_INTERPOSE(smash_write, write);
extern "C" ssize_t smash_write(int fd, const void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = reinterpret_cast<write_fn>(smash_interpose_smash_write.original)(fd, buf, count);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

// ── pread / pwrite ──────────────────────────────────────────────────────────

using pread_fn = ssize_t(*)(int, void*, size_t, off_t);
using pwrite_fn = ssize_t(*)(int, const void*, size_t, off_t);

extern "C" ssize_t smash_pread(int fd, void* buf, size_t count, off_t offset);
SMASH_INTERPOSE(smash_pread, pread);
extern "C" ssize_t smash_pread(int fd, void* buf, size_t count, off_t offset) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = reinterpret_cast<pread_fn>(smash_interpose_smash_pread.original)(fd, buf, count, offset);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t smash_pwrite(int fd, const void* buf, size_t count, off_t offset);
SMASH_INTERPOSE(smash_pwrite, pwrite);
extern "C" ssize_t smash_pwrite(int fd, const void* buf, size_t count, off_t offset) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::warmPages(buf, count, vm); smash::vm::pinPages(buf, count, vm); }
    ssize_t ret = reinterpret_cast<pwrite_fn>(smash_interpose_smash_pwrite.original)(fd, buf, count, offset);
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

// ── readv / writev ──────────────────────────────────────────────────────────

using readv_fn = ssize_t(*)(int, const struct iovec*, int);
using writev_fn = ssize_t(*)(int, const struct iovec*, int);

extern "C" ssize_t smash_readv(int fd, const struct iovec* iov, int iovcnt);
SMASH_INTERPOSE(smash_readv, readv);
extern "C" ssize_t smash_readv(int fd, const struct iovec* iov, int iovcnt) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) { warmIovec(iov, iovcnt, vm); smash::vm::pinIovec(iov, iovcnt, vm); }
    ssize_t ret = reinterpret_cast<readv_fn>(smash_interpose_smash_readv.original)(fd, iov, iovcnt);
    if (vm && iov && iovcnt > 0) smash::vm::unpinIovec(iov, iovcnt, vm);
    return ret;
}

extern "C" ssize_t smash_writev(int fd, const struct iovec* iov, int iovcnt);
SMASH_INTERPOSE(smash_writev, writev);
extern "C" ssize_t smash_writev(int fd, const struct iovec* iov, int iovcnt) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) { warmIovec(iov, iovcnt, vm); smash::vm::pinIovec(iov, iovcnt, vm); }
    ssize_t ret = reinterpret_cast<writev_fn>(smash_interpose_smash_writev.original)(fd, iov, iovcnt);
    if (vm && iov && iovcnt > 0) smash::vm::unpinIovec(iov, iovcnt, vm);
    return ret;
}

// ── fread / fgets (buffered I/O) ────────────────────────────────────────────
//
// DYLD interposition can't intercept intra-dylib calls. libc's fread/fgets
// call read() internally within libSystem, bypassing our smash_read.
// We interpose the higher-level functions to warm/pin the FILE's internal
// buffer before they call read() under the hood.

// Helper: warm and pin a FILE's internal read buffer
static inline void warmFileBuffer(FILE* stream, smash::VmRegion* vm) {
    // macOS FILE struct: _bf._base is the buffer, _bf._size is its length
    if (stream && stream->_bf._base && stream->_bf._size > 0) {
        smash::vm::warmPages(stream->_bf._base, stream->_bf._size, vm);
        smash::vm::pinPages(stream->_bf._base, stream->_bf._size, vm);
    }
}

static inline void unpinFileBuffer(FILE* stream, smash::VmRegion* vm) {
    if (stream && stream->_bf._base && stream->_bf._size > 0) {
        smash::vm::unpinPages(stream->_bf._base, stream->_bf._size, vm);
    }
}

using fread_fn = size_t(*)(void*, size_t, size_t, FILE*);
using fgets_fn = char*(*)(char*, int, FILE*);

extern "C" size_t smash_fread(void* ptr, size_t size, size_t nitems, FILE* stream);
SMASH_INTERPOSE(smash_fread, fread);
extern "C" size_t smash_fread(void* ptr, size_t size, size_t nitems, FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    size_t total = size * nitems;
    if (vm) {
        if (ptr && total) { smash::vm::warmPages(ptr, total, vm); smash::vm::pinPages(ptr, total, vm); }
        warmFileBuffer(stream, vm);
    }
    size_t ret = reinterpret_cast<fread_fn>(smash_interpose_smash_fread.original)(ptr, size, nitems, stream);
    if (vm) {
        if (ptr && total) smash::vm::unpinPages(ptr, total, vm);
        unpinFileBuffer(stream, vm);
    }
    return ret;
}

extern "C" char* smash_fgets(char* str, int size, FILE* stream);
SMASH_INTERPOSE(smash_fgets, fgets);
extern "C" char* smash_fgets(char* str, int size, FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) {
        if (str && size > 0) { smash::vm::warmPages(str, size, vm); smash::vm::pinPages(str, size, vm); }
        warmFileBuffer(stream, vm);
    }
    char* ret = reinterpret_cast<fgets_fn>(smash_interpose_smash_fgets.original)(str, size, stream);
    if (vm) {
        if (str && size > 0) smash::vm::unpinPages(str, size, vm);
        unpinFileBuffer(stream, vm);
    }
    return ret;
}

// ── fgetc / getc (character-at-a-time input) ────────────────────────────────
//
// DuckDB and C++ iostream (std::cin) use fgetc/getc for character reads.
// These internally call read() within libSystem when the FILE buffer is empty.
// We warm/pin the FILE buffer before each call to prevent kernel EFAULT.

using fgetc_fn = int(*)(FILE*);
using getc_fn = int(*)(FILE*);

extern "C" int smash_fgetc(FILE* stream);
SMASH_INTERPOSE(smash_fgetc, fgetc);
extern "C" int smash_fgetc(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = reinterpret_cast<fgetc_fn>(smash_interpose_smash_fgetc.original)(stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

extern "C" int smash_getc(FILE* stream);
SMASH_INTERPOSE(smash_getc, getc);
extern "C" int smash_getc(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = reinterpret_cast<getc_fn>(smash_interpose_smash_getc.original)(stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

// ── fwrite / fflush (buffered output) ───────────────────────────────────────
//
// libc's fwrite/fflush internally call write() when flushing the buffer.
// If the FILE output buffer page is PROT_NONE (compressed), the kernel's
// copyin from the buffer would fail. Pin the FILE buffer during these calls.

using fwrite_fn = size_t(*)(const void*, size_t, size_t, FILE*);
using fflush_fn = int(*)(FILE*);

extern "C" size_t smash_fwrite(const void* ptr, size_t size, size_t nitems, FILE* stream);
SMASH_INTERPOSE(smash_fwrite, fwrite);
extern "C" size_t smash_fwrite(const void* ptr, size_t size, size_t nitems, FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    size_t total = size * nitems;
    if (vm) {
        if (ptr && total) smash::vm::warmPages(ptr, total, vm);
        warmFileBuffer(stream, vm);
    }
    size_t ret = reinterpret_cast<fwrite_fn>(smash_interpose_smash_fwrite.original)(ptr, size, nitems, stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

extern "C" int smash_fflush(FILE* stream);
SMASH_INTERPOSE(smash_fflush, fflush);
extern "C" int smash_fflush(FILE* stream) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) warmFileBuffer(stream, vm);
    int ret = reinterpret_cast<fflush_fn>(smash_interpose_smash_fflush.original)(stream);
    if (vm) unpinFileBuffer(stream, vm);
    return ret;
}

#endif // __APPLE__

// ── alloc8 integration ───────────────────────────────────────────────────────

// Required by alloc8's mac_threads.cpp for thread-created flag
extern "C" volatile int xxthread_created_flag = 0;

using SmashRedirect = alloc8::HeapRedirect<smash::SmashHeap>;
ALLOC8_REDIRECT_WITH_THREADS(SmashRedirect);
