// smash/src/smash_heap.cpp - SmashHeap alloc8 integration + thread cache methods
#include "smash_heap.h"
#include "vm/syscall_compat.h"
#include <alloc8/alloc8.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

// ── Thread init counter for deferred compression start ──────────────────────
// pthread_create during early DYLD_INSERT init crashes the ObjC runtime's
// task_restartable_ranges_register on macOS. We count threadInit() calls
// and only start compression after the second call (first = main thread
// during early init, subsequent = real threads after init is complete).

std::atomic<int> smash::g_thread_init_count{0};

// ── System allocator function pointers for compress-only mode ───────────────
smash::SystemAllocFns smash::g_system_alloc;

// Early init: resolve system malloc/free before alloc8 interposition.
// Needed for compress-only mode and large-only mode (small alloc passthrough).
__attribute__((constructor(50)))  // Run before alloc8 (priority 100)
static void smash_resolve_system_alloc() {
    smash::g_system_alloc.resolve();
}

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

    // Bucket pointers by arena based on their span's arena_id.
    // arena_id may be in [0, kTotalArenas) when cold-arena feedback is on.
    void* buckets[kTotalArenas][kThreadCacheMaxPerClass];
    size_t counts[kTotalArenas]{};
    for (size_t i = start; i < c.count; ++i) {
        Span* span = page_map->get(reinterpret_cast<uintptr_t>(c.ptrs[i]));
        uint8_t arena = (span && !span->is_large) ? span->arena_id : 0;
        if (arena >= kTotalArenas) arena = 0;  // defensive
        buckets[arena][counts[arena]++] = c.ptrs[i];
    }
    for (int a = 0; a < kTotalArenas; ++a) {
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
            void* buckets[kTotalArenas][kThreadCacheMaxPerClass];
            size_t counts[kTotalArenas]{};
            for (size_t j = 0; j < c.count; ++j) {
                Span* span = page_map->get(reinterpret_cast<uintptr_t>(c.ptrs[j]));
                uint8_t arena = (span && !span->is_large) ? span->arena_id : 0;
                if (arena >= kTotalArenas) arena = 0;
                buckets[arena][counts[arena]++] = c.ptrs[j];
            }
            for (int a = 0; a < kTotalArenas; ++a) {
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

// Set when compression is initialized; nullptr in pass-through builds where
// SmashHeap never wires up page_states_. The mmap / Mach VM interposers
// gate every state mutation on this being non-null.
smash::PageStateTable* smash::g_smash_page_states_for_external = nullptr;

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
            smash::vm::pinPages(changelist, nchanges * sizeof(struct kevent), vm);
            smash::vm::warmPages(changelist, nchanges * sizeof(struct kevent), vm);
        }
        if (eventlist && nevents > 0) {
            smash::vm::pinPages(eventlist, nevents * sizeof(struct kevent), vm);
            smash::vm::warmPages(eventlist, nevents * sizeof(struct kevent), vm);
        }
    }
    int ret;
    for (int attempt = 0; ; ++attempt) {
        ret = reinterpret_cast<kevent_fn>(smash_interpose_smash_kevent.original)(
            kq, changelist, nchanges, eventlist, nevents, timeout);
        if (ret != -1 || errno != EFAULT || attempt >= 3) break;
        if (vm) {
            if (changelist && nchanges > 0)
                smash::vm::warmPages(changelist, nchanges * sizeof(struct kevent), vm);
            if (eventlist && nevents > 0)
                smash::vm::warmPages(eventlist, nevents * sizeof(struct kevent), vm);
        }
    }
    if (vm) {
        if (changelist && nchanges > 0)
            smash::vm::unpinPages(changelist, nchanges * sizeof(struct kevent), vm);
        if (eventlist && nevents > 0)
            smash::vm::unpinPages(eventlist, nevents * sizeof(struct kevent), vm);
    }
    return ret;
}

// ── kevent64 ─────────────────────────────────────────────────────────────────
// Firefox's IPC I/O thread uses kevent64 (not kevent) on macOS via
// message_pump_kqueue.cc. The events_ buffer is a std::vector<kevent64_s>
// allocated through our malloc, so it lives in our VmRegion and the
// compressor can mprotect its pages. Without this interposer the kernel
// returns EFAULT on the page-fault path and Firefox MOZ_CRASHes.

extern "C" int smash_kevent64(int kq,
                              const struct kevent64_s* changelist, int nchanges,
                              struct kevent64_s* eventlist, int nevents,
                              unsigned int flags,
                              const struct timespec* timeout);
SMASH_INTERPOSE(smash_kevent64, kevent64);

using kevent64_fn = int(*)(int, const struct kevent64_s*, int,
                           struct kevent64_s*, int, unsigned int,
                           const struct timespec*);

extern "C" int smash_kevent64(int kq,
                              const struct kevent64_s* changelist, int nchanges,
                              struct kevent64_s* eventlist, int nevents,
                              unsigned int flags,
                              const struct timespec* timeout) {
    auto* vm = smash::g_smash_vm_region;
    if (vm) {
        if (changelist && nchanges > 0) {
            smash::vm::pinPages(changelist, nchanges * sizeof(struct kevent64_s), vm);
            smash::vm::warmPages(changelist, nchanges * sizeof(struct kevent64_s), vm);
        }
        if (eventlist && nevents > 0) {
            smash::vm::pinPages(eventlist, nevents * sizeof(struct kevent64_s), vm);
            smash::vm::warmPages(eventlist, nevents * sizeof(struct kevent64_s), vm);
        }
    }
    int ret;
    for (int attempt = 0; ; ++attempt) {
        ret = reinterpret_cast<kevent64_fn>(smash_interpose_smash_kevent64.original)(
            kq, changelist, nchanges, eventlist, nevents, flags, timeout);
        if (ret != -1 || errno != EFAULT || attempt >= 3) break;
        if (vm) {
            if (changelist && nchanges > 0)
                smash::vm::warmPages(changelist, nchanges * sizeof(struct kevent64_s), vm);
            if (eventlist && nevents > 0)
                smash::vm::warmPages(eventlist, nevents * sizeof(struct kevent64_s), vm);
        }
    }
    if (vm) {
        if (changelist && nchanges > 0)
            smash::vm::unpinPages(changelist, nchanges * sizeof(struct kevent64_s), vm);
        if (eventlist && nevents > 0)
            smash::vm::unpinPages(eventlist, nevents * sizeof(struct kevent64_s), vm);
    }
    return ret;
}

// ── mach_msg ─────────────────────────────────────────────────────────────────
// CoreFoundation's runloop (CFRunLoopServiceMachPort) calls mach_msg with a
// caller-provided buffer. For receive, the kernel writes the message into the
// buffer; for send, it reads. If the buffer's pages are smash-managed and the
// compressor has stripped write access (Phase 3 PROT_READ for monitoring) or
// read access (Phase 2 PROT_NONE for compression), the kernel returns
// MACH_RCV_INVALID_DATA / MACH_SEND_INVALID_DATA and CF __builtin_traps via
// __CFRunLoopServiceMachPort.cold.1. Pin and warm the buffer pages so the
// compressor leaves them alone for the duration of the call.

using mach_msg_fn = mach_msg_return_t(*)(mach_msg_header_t*, mach_msg_option_t,
                                         mach_msg_size_t, mach_msg_size_t,
                                         mach_port_name_t, mach_msg_timeout_t,
                                         mach_port_name_t);

extern "C" mach_msg_return_t smash_mach_msg(mach_msg_header_t* msg,
                                            mach_msg_option_t option,
                                            mach_msg_size_t send_size,
                                            mach_msg_size_t rcv_size,
                                            mach_port_name_t rcv_name,
                                            mach_msg_timeout_t timeout,
                                            mach_port_name_t notify);
SMASH_INTERPOSE(smash_mach_msg, mach_msg);

// Up to this many OOL descriptors per message will get their target buffers
// pinned. CFRunLoop messages typically carry 0–1 OOL descriptors; raising
// this only matters for unusual senders. The cap keeps the unpin tracker
// on-stack and bounds worst-case latency.
static constexpr size_t kSmashMaxOolPins = 16;

struct OolPinTracker {
    struct Entry { void* addr; size_t size; };
    Entry entries[kSmashMaxOolPins];
    size_t count = 0;
    void add(void* addr, size_t size) {
        if (count < kSmashMaxOolPins) {
            entries[count].addr = addr;
            entries[count].size = size;
            ++count;
        }
    }
};

// Walk the descriptor body of a complex mach message, pin and warm each
// OOL descriptor's target buffer, and record it in `tracker` so we can
// unpin on return.
//
// Layout: after mach_msg_header_t comes mach_msg_body_t (a single u32
// descriptor count) when MACH_MSGH_BITS_COMPLEX is set in msgh_bits.
// Then `count` descriptors of varying sizes follow. Every descriptor
// shares the same prefix `mach_msg_type_descriptor_t` (12 bytes on
// LP64), with the type byte at offset 11. We use that to discriminate
// before reading the full descriptor struct.
static void smash_pin_ool_descriptors(mach_msg_header_t* msg,
                                      smash::VmRegion* vm,
                                      OolPinTracker& tracker) {
    if (!msg || !vm) return;
    if (!(msg->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return;

    auto* body = reinterpret_cast<mach_msg_body_t*>(msg + 1);
    uint32_t desc_count = body->msgh_descriptor_count;
    auto* p = reinterpret_cast<uint8_t*>(body + 1);
    // Don't walk past the message body — guard against a corrupt count.
    auto* msg_end = reinterpret_cast<uint8_t*>(msg) + msg->msgh_size;

    for (uint32_t i = 0; i < desc_count; ++i) {
        if (p + 12 > msg_end) return;
        uint8_t type = p[11];  // type byte: same offset across all variants
        size_t advance = 0;
        switch (type) {
        case MACH_MSG_PORT_DESCRIPTOR:
            advance = sizeof(mach_msg_port_descriptor_t);
            break;
        case MACH_MSG_OOL_DESCRIPTOR:
        case MACH_MSG_OOL_VOLATILE_DESCRIPTOR: {
            auto* d = reinterpret_cast<mach_msg_ool_descriptor_t*>(p);
            if (d->address && d->size > 0) {
                smash::vm::pinPages(d->address, d->size, vm);
                smash::vm::warmPages(d->address, d->size, vm);
                tracker.add(d->address, d->size);
            }
            advance = sizeof(mach_msg_ool_descriptor_t);
            break;
        }
        case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
            auto* d = reinterpret_cast<mach_msg_ool_ports_descriptor_t*>(p);
            if (d->address && d->count > 0) {
                size_t bytes = static_cast<size_t>(d->count) *
                               sizeof(mach_port_t);
                smash::vm::pinPages(d->address, bytes, vm);
                smash::vm::warmPages(d->address, bytes, vm);
                tracker.add(d->address, bytes);
            }
            advance = sizeof(mach_msg_ool_ports_descriptor_t);
            break;
        }
        case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
            advance = sizeof(mach_msg_guarded_port_descriptor_t);
            break;
        default:
            // Unknown descriptor type — bail rather than guess. The
            // already-pinned descriptors will be unpinned on return.
            return;
        }
        if (p + advance > msg_end) return;
        p += advance;
    }
}

extern "C" mach_msg_return_t smash_mach_msg(mach_msg_header_t* msg,
                                            mach_msg_option_t option,
                                            mach_msg_size_t send_size,
                                            mach_msg_size_t rcv_size,
                                            mach_port_name_t rcv_name,
                                            mach_msg_timeout_t timeout,
                                            mach_port_name_t notify) {
    auto* vm = smash::g_smash_vm_region;
    // The buffer holds either send_size (send) or rcv_size (receive); cover
    // both by using the max so a combined send/receive is also safe.
    size_t buf_size = send_size > rcv_size ? send_size : rcv_size;
    if (vm && msg && buf_size > 0) {
        smash::vm::pinPages(msg, buf_size, vm);
        smash::vm::warmPages(msg, buf_size, vm);
    }
    // Walk descriptor body for OOL targets — the kernel reads/writes
    // those during message dispatch (mach_msg2_trap), and a compressed
    // page hit there is unrecoverable (kernel checks protection
    // synchronously). Only relevant for SEND; on RECEIVE the kernel
    // writes new descriptor entries and any OOL data is kernel-mapped
    // into us, not user-side.
    OolPinTracker ool_tracker;
    if (vm && msg && (option & MACH_SEND_MSG)) {
        smash_pin_ool_descriptors(msg, vm, ool_tracker);
    }
    mach_msg_return_t ret;
    for (int attempt = 0; ; ++attempt) {
        ret = reinterpret_cast<mach_msg_fn>(smash_interpose_smash_mach_msg.original)(
            msg, option, send_size, rcv_size, rcv_name, timeout, notify);
        // Retry on the buffer-validity errors specifically; other returns
        // (timeout, port died, etc.) are not buffer-related.
        if (ret != MACH_RCV_INVALID_DATA && ret != MACH_SEND_INVALID_DATA)
            break;
        if (attempt >= 3) break;
        if (vm && msg && buf_size > 0)
            smash::vm::warmPages(msg, buf_size, vm);
        for (size_t i = 0; i < ool_tracker.count; ++i)
            smash::vm::warmPages(ool_tracker.entries[i].addr,
                                  ool_tracker.entries[i].size, vm);
    }
    // SMASH_IPC_STICKY_PIN=1: leave the pin counters incremented after the
    // call, so any page that ever held an IPC buffer is exempt from
    // compression (via the existing g_page_pins skip in the compressor).
    // Trades compression coverage for IPC robustness — pages used for
    // IPC stay readable, so parent↔child mach_msg dispatch never hits
    // a compressed page. Pin counter is uint8_t; saturates at 255, so
    // a page hit > 255 times rolls back to compressible. Acceptable
    // for early-startup IPC (the window where Firefox's flakiness lives).
    static const bool sticky_pin = []{
        const char* v = std::getenv("SMASH_IPC_STICKY_PIN");
        return v && v[0] == '1';
    }();
    if (!sticky_pin) {
        for (size_t i = 0; i < ool_tracker.count; ++i)
            smash::vm::unpinPages(ool_tracker.entries[i].addr,
                                   ool_tracker.entries[i].size, vm);
        if (vm && msg && buf_size > 0)
            smash::vm::unpinPages(msg, buf_size, vm);
    }
    return ret;
}

// ── mach_msg_overwrite ──────────────────────────────────────────────────────
// Same hazard as mach_msg, but called by libxpc / libdispatch on a separate
// path. __DATA_INTERPOSE only catches cross-dylib calls, so libsystem-internal
// chains (mach_msg → mach_msg_overwrite → mach_msg2_internal → trap) bypass
// our smash_mach_msg interposer entirely. Adding this interposer catches
// libxpc/libdispatch direct calls to mach_msg_overwrite. Same buffer-pinning
// strategy: warm + pin send buffer, walk OOL descriptors on send, warm the
// separate receive buffer if one was supplied.

using mach_msg_overwrite_fn = mach_msg_return_t(*)(
    mach_msg_header_t*, mach_msg_option_t,
    mach_msg_size_t, mach_msg_size_t,
    mach_port_name_t, mach_msg_timeout_t,
    mach_port_name_t, mach_msg_header_t*, mach_msg_size_t);

extern "C" mach_msg_return_t smash_mach_msg_overwrite(
    mach_msg_header_t* msg, mach_msg_option_t option,
    mach_msg_size_t send_size, mach_msg_size_t rcv_size,
    mach_port_name_t rcv_name, mach_msg_timeout_t timeout,
    mach_port_name_t notify, mach_msg_header_t* rcv_msg,
    mach_msg_size_t rcv_limit);
SMASH_INTERPOSE(smash_mach_msg_overwrite, mach_msg_overwrite);

extern "C" mach_msg_return_t smash_mach_msg_overwrite(
    mach_msg_header_t* msg, mach_msg_option_t option,
    mach_msg_size_t send_size, mach_msg_size_t rcv_size,
    mach_port_name_t rcv_name, mach_msg_timeout_t timeout,
    mach_port_name_t notify, mach_msg_header_t* rcv_msg,
    mach_msg_size_t rcv_limit) {
    auto* vm = smash::g_smash_vm_region;
    size_t send_buf_size = send_size;
    size_t rcv_buf_size = rcv_msg ? rcv_limit : rcv_size;
    if (vm) {
        if (msg && send_buf_size > 0) {
            smash::vm::pinPages(msg, send_buf_size, vm);
            smash::vm::warmPages(msg, send_buf_size, vm);
        }
        if (rcv_msg && rcv_buf_size > 0) {
            smash::vm::pinPages(rcv_msg, rcv_buf_size, vm);
            smash::vm::warmPages(rcv_msg, rcv_buf_size, vm);
        }
    }
    OolPinTracker ool_tracker;
    if (vm && msg && (option & MACH_SEND_MSG)) {
        smash_pin_ool_descriptors(msg, vm, ool_tracker);
    }
    mach_msg_return_t ret;
    for (int attempt = 0; ; ++attempt) {
        ret = reinterpret_cast<mach_msg_overwrite_fn>(
            smash_interpose_smash_mach_msg_overwrite.original)(
            msg, option, send_size, rcv_size, rcv_name, timeout, notify,
            rcv_msg, rcv_limit);
        if (ret != MACH_RCV_INVALID_DATA && ret != MACH_SEND_INVALID_DATA)
            break;
        if (attempt >= 3) break;
        if (vm) {
            if (msg && send_buf_size > 0)
                smash::vm::warmPages(msg, send_buf_size, vm);
            if (rcv_msg && rcv_buf_size > 0)
                smash::vm::warmPages(rcv_msg, rcv_buf_size, vm);
            for (size_t i = 0; i < ool_tracker.count; ++i)
                smash::vm::warmPages(ool_tracker.entries[i].addr,
                                      ool_tracker.entries[i].size, vm);
        }
    }
    static const bool sticky_pin_ow = []{
        const char* v = std::getenv("SMASH_IPC_STICKY_PIN");
        return v && v[0] == '1';
    }();
    if (!sticky_pin_ow) {
        for (size_t i = 0; i < ool_tracker.count; ++i)
            smash::vm::unpinPages(ool_tracker.entries[i].addr,
                                   ool_tracker.entries[i].size, vm);
        if (vm && msg && send_buf_size > 0)
            smash::vm::unpinPages(msg, send_buf_size, vm);
        if (vm && rcv_msg && rcv_buf_size > 0)
            smash::vm::unpinPages(rcv_msg, rcv_buf_size, vm);
    }
    return ret;
}

// ── mach_msg2_internal ───────────────────────────────────────────────────────
// The internal entry that mach_msg / mach_msg_overwrite / mach_msg2 all
// route through before invoking the kernel trap. Some libsystem-resident
// callers (libxpc, libdispatch) reach this via paths we can't see — but
// any *cross-dylib* call to mach_msg2_internal goes through interposition,
// so adding this catches the direct callers.
//
// Signature is from Apple's libsyscall/mach/mach_msg.c:
//   mach_msg_return_t mach_msg2_internal(void *data, uint64_t options,
//       uint64_t msgh_bits_and_send_size,
//       uint64_t msgh_remote_and_local_port,
//       uint64_t msgh_voucher_and_id,
//       uint64_t desc_count_and_rcv_size,
//       uint64_t rcv_name_and_timeout,
//       uint32_t priority);
//
// Send size is the low 32 bits of msgh_bits_and_send_size; receive size is
// the low 32 bits of desc_count_and_rcv_size. We pin the buffer over the
// max of the two so both directions are covered.

// mach_msg2_internal is exported from libsystem_kernel but not declared in
// any public header. Declare here so we can take its address for the
// interpose entry.
extern "C" mach_msg_return_t mach_msg2_internal(
    void* data, uint64_t options, uint64_t bits_and_send_size,
    uint64_t remote_and_local_port, uint64_t voucher_and_id,
    uint64_t desc_count_and_rcv_size, uint64_t rcv_name_and_timeout,
    uint32_t priority);

using mach_msg2_internal_fn = mach_msg_return_t(*)(
    void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint32_t);

extern "C" mach_msg_return_t smash_mach_msg2_internal(
    void* data, uint64_t options, uint64_t bits_and_send_size,
    uint64_t remote_and_local_port, uint64_t voucher_and_id,
    uint64_t desc_count_and_rcv_size, uint64_t rcv_name_and_timeout,
    uint32_t priority);
SMASH_INTERPOSE(smash_mach_msg2_internal, mach_msg2_internal);

extern "C" mach_msg_return_t smash_mach_msg2_internal(
    void* data, uint64_t options, uint64_t bits_and_send_size,
    uint64_t remote_and_local_port, uint64_t voucher_and_id,
    uint64_t desc_count_and_rcv_size, uint64_t rcv_name_and_timeout,
    uint32_t priority) {
    auto* vm = smash::g_smash_vm_region;
    uint32_t send_size = static_cast<uint32_t>(bits_and_send_size);
    uint32_t rcv_size = static_cast<uint32_t>(desc_count_and_rcv_size);
    size_t buf_size = send_size > rcv_size ? send_size : rcv_size;
    if (vm && data && buf_size > 0) {
        smash::vm::pinPages(data, buf_size, vm);
        smash::vm::warmPages(data, buf_size, vm);
    }
    // OOL descriptor walking only makes sense if `data` looks like a
    // mach_msg_header_t with the COMPLEX bit set; cheap to check.
    OolPinTracker ool_tracker;
    if (vm && data && send_size >= sizeof(mach_msg_header_t)) {
        auto* hdr = static_cast<mach_msg_header_t*>(data);
        if (hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
            smash_pin_ool_descriptors(hdr, vm, ool_tracker);
        }
    }
    mach_msg_return_t ret;
    for (int attempt = 0; ; ++attempt) {
        ret = reinterpret_cast<mach_msg2_internal_fn>(
            smash_interpose_smash_mach_msg2_internal.original)(
            data, options, bits_and_send_size, remote_and_local_port,
            voucher_and_id, desc_count_and_rcv_size, rcv_name_and_timeout,
            priority);
        if (ret != MACH_RCV_INVALID_DATA && ret != MACH_SEND_INVALID_DATA)
            break;
        if (attempt >= 3) break;
        if (vm && data && buf_size > 0)
            smash::vm::warmPages(data, buf_size, vm);
        for (size_t i = 0; i < ool_tracker.count; ++i)
            smash::vm::warmPages(ool_tracker.entries[i].addr,
                                  ool_tracker.entries[i].size, vm);
    }
    static const bool sticky_pin_2i = []{
        const char* v = std::getenv("SMASH_IPC_STICKY_PIN");
        return v && v[0] == '1';
    }();
    if (!sticky_pin_2i) {
        for (size_t i = 0; i < ool_tracker.count; ++i)
            smash::vm::unpinPages(ool_tracker.entries[i].addr,
                                   ool_tracker.entries[i].size, vm);
        if (vm && data && buf_size > 0)
            smash::vm::unpinPages(data, buf_size, vm);
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
    if (vm && buf && len) { smash::vm::pinPages(buf, len, vm); smash::vm::warmPages(buf, len, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<recv_fn>(smash_interpose_smash_recv.original)(s, buf, len, flags); },
        [&] { if (vm && buf && len) smash::vm::warmPages(buf, len, vm); });
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

extern "C" ssize_t smash_send(int s, const void* buf, size_t len, int flags);
SMASH_INTERPOSE(smash_send, send);
extern "C" ssize_t smash_send(int s, const void* buf, size_t len, int flags) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::pinPages(buf, len, vm); smash::vm::warmPages(buf, len, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<send_fn>(smash_interpose_smash_send.original)(s, buf, len, flags); },
        [&] { if (vm && buf && len) smash::vm::warmPages(buf, len, vm); });
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
    if (vm && buf && len) { smash::vm::pinPages(buf, len, vm); smash::vm::warmPages(buf, len, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<recvfrom_fn>(smash_interpose_smash_recvfrom.original)(
            s, buf, len, flags, from, fromlen); },
        [&] { if (vm && buf && len) smash::vm::warmPages(buf, len, vm); });
    if (vm && buf && len) smash::vm::unpinPages(buf, len, vm);
    return ret;
}

extern "C" ssize_t smash_sendto(int s, const void* buf, size_t len, int flags,
                                 const struct sockaddr* to, socklen_t tolen);
SMASH_INTERPOSE(smash_sendto, sendto);
extern "C" ssize_t smash_sendto(int s, const void* buf, size_t len, int flags,
                                 const struct sockaddr* to, socklen_t tolen) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && len) { smash::vm::pinPages(buf, len, vm); smash::vm::warmPages(buf, len, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<sendto_fn>(smash_interpose_smash_sendto.original)(
            s, buf, len, flags, to, tolen); },
        [&] { if (vm && buf && len) smash::vm::warmPages(buf, len, vm); });
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
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<recvmsg_fn>(smash_interpose_smash_recvmsg.original)(s, msg, flags); },
        [&] {
            if (vm && msg && msg->msg_iov && msg->msg_iovlen > 0)
                warmIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
        });
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
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<sendmsg_fn>(smash_interpose_smash_sendmsg.original)(s, msg, flags); },
        [&] {
            if (vm && msg && msg->msg_iov && msg->msg_iovlen > 0)
                warmIovec(msg->msg_iov, static_cast<int>(msg->msg_iovlen), vm);
        });
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
    size_t fds_bytes = nfds * sizeof(struct pollfd);
    if (vm && fds && nfds > 0) {
        smash::vm::pinPages(fds, fds_bytes, vm);
        smash::vm::warmPages(fds, fds_bytes, vm);
    }
    int ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<poll_fn>(smash_interpose_smash_poll.original)(fds, nfds, timeout); },
        [&] { if (vm && fds && nfds > 0) smash::vm::warmPages(fds, fds_bytes, vm); });
    if (vm && fds && nfds > 0)
        smash::vm::unpinPages(fds, fds_bytes, vm);
    return ret;
}

// epoll_wait interposition is Linux-only (see linux_syscall_wrappers.cpp)

// ── read / write ────────────────────────────────────────────────────────────

using read_fn = ssize_t(*)(int, void*, size_t);
using write_fn = ssize_t(*)(int, const void*, size_t);

extern "C" ssize_t smash_read(int fd, void* buf, size_t count);
SMASH_INTERPOSE(smash_read, read);
extern "C" ssize_t smash_read(int fd, void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::pinPages(buf, count, vm); smash::vm::warmPages(buf, count, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<read_fn>(smash_interpose_smash_read.original)(fd, buf, count); },
        [&] { if (vm && buf && count) smash::vm::warmPages(buf, count, vm); });
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t smash_write(int fd, const void* buf, size_t count);
SMASH_INTERPOSE(smash_write, write);
extern "C" ssize_t smash_write(int fd, const void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::pinPages(buf, count, vm); smash::vm::warmPages(buf, count, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<write_fn>(smash_interpose_smash_write.original)(fd, buf, count); },
        [&] { if (vm && buf && count) smash::vm::warmPages(buf, count, vm); });
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
    if (vm && buf && count) { smash::vm::pinPages(buf, count, vm); smash::vm::warmPages(buf, count, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<pread_fn>(smash_interpose_smash_pread.original)(fd, buf, count, offset); },
        [&] { if (vm && buf && count) smash::vm::warmPages(buf, count, vm); });
    if (vm && buf && count) smash::vm::unpinPages(buf, count, vm);
    return ret;
}

extern "C" ssize_t smash_pwrite(int fd, const void* buf, size_t count, off_t offset);
SMASH_INTERPOSE(smash_pwrite, pwrite);
extern "C" ssize_t smash_pwrite(int fd, const void* buf, size_t count, off_t offset) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && buf && count) { smash::vm::pinPages(buf, count, vm); smash::vm::warmPages(buf, count, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<pwrite_fn>(smash_interpose_smash_pwrite.original)(fd, buf, count, offset); },
        [&] { if (vm && buf && count) smash::vm::warmPages(buf, count, vm); });
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
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<readv_fn>(smash_interpose_smash_readv.original)(fd, iov, iovcnt); },
        [&] { if (vm && iov && iovcnt > 0) warmIovec(iov, iovcnt, vm); });
    if (vm && iov && iovcnt > 0) smash::vm::unpinIovec(iov, iovcnt, vm);
    return ret;
}

extern "C" ssize_t smash_writev(int fd, const struct iovec* iov, int iovcnt);
SMASH_INTERPOSE(smash_writev, writev);
extern "C" ssize_t smash_writev(int fd, const struct iovec* iov, int iovcnt) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && iov && iovcnt > 0) { warmIovec(iov, iovcnt, vm); smash::vm::pinIovec(iov, iovcnt, vm); }
    ssize_t ret = smash::vm::retryOnEfault(
        [&] { return reinterpret_cast<writev_fn>(smash_interpose_smash_writev.original)(fd, iov, iovcnt); },
        [&] { if (vm && iov && iovcnt > 0) warmIovec(iov, iovcnt, vm); });
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
        if (ptr && total) { smash::vm::pinPages(ptr, total, vm); smash::vm::warmPages(ptr, total, vm); }
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
        if (str && size > 0) { smash::vm::pinPages(str, size, vm); smash::vm::warmPages(str, size, vm); }
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

// ── External-mapping interposers ────────────────────────────────────────────
//
// Standard smash's main allocator hands out memory from a single big
// MAP_ANON reservation that smash itself owns; the compressor compresses
// pages within that reservation. Application code that calls mmap() /
// vm_allocate() *directly* (SpiderMonkey JS GC arenas, Skia surfaces via
// mozalloc_aligned, etc.) bypasses the malloc path entirely and so escapes
// the compressor.
//
// The interposers below register such mappings via VmRegion's external-
// page hash so the compressor's tick can see them. Strict filters apply:
//
//   • mmap     — only MAP_ANON | PROT_WRITE. File-backed mappings would
//                break msync semantics under compression and the OS
//                already evicts them; non-writable mappings won't dirty.
//   • mach_vm_allocate — anonymous by design; tracked when caller-
//                requested protection includes VM_PROT_WRITE.
//
// munmap / vm_deallocate counterparts mark the pages EMPTY so the
// compressor stops scanning them. Compressed-page leakage on unmap is
// possible (we don't free associated compressed buffers) but bounded
// by the workload's churn rate.
//
// SMASH_NO_EXTERNAL_TRACKING=1 disables the registration path entirely
// — useful as a kill switch if a target trips on the new code.

namespace {

inline bool externalTrackingEnabled() {
    static const bool disabled = []{
        const char* v = std::getenv("SMASH_NO_EXTERNAL_TRACKING");
        return v && v[0] == '1';
    }();
    return !disabled;
}

// Register every page in [base, base+len) with the VmRegion's external
// tracker and set its initial PageState to ACTIVE.
inline void registerExternalRange(smash::VmRegion* vm, void* base, size_t len) {
    if (!vm || !base || !len) return;
    if (!externalTrackingEnabled()) return;
    auto start = reinterpret_cast<uintptr_t>(base) & ~(uintptr_t{smash::kPageSize} - 1);
    auto end = (reinterpret_cast<uintptr_t>(base) + len + smash::kPageSize - 1)
               & ~(uintptr_t{smash::kPageSize} - 1);
    for (uintptr_t p = start; p < end; p += smash::kPageSize) {
        size_t idx = vm->trackExternalPage(p);
        if (idx == 0) continue;  // table full or page in contiguous arena
        smash::g_smash_page_states_for_external
            ? smash::g_smash_page_states_for_external->set(idx, smash::PageState::ACTIVE)
            : (void)0;
    }
}

inline void deregisterExternalRange(smash::VmRegion* vm, void* base, size_t len) {
    if (!vm || !base || !len) return;
    if (!externalTrackingEnabled()) return;
    auto start = reinterpret_cast<uintptr_t>(base) & ~(uintptr_t{smash::kPageSize} - 1);
    auto end = (reinterpret_cast<uintptr_t>(base) + len + smash::kPageSize - 1)
               & ~(uintptr_t{smash::kPageSize} - 1);
    for (uintptr_t p = start; p < end; p += smash::kPageSize) {
        size_t idx = vm->pageIndex(p);
        if (idx == 0) continue;
        if (smash::g_smash_page_states_for_external)
            smash::g_smash_page_states_for_external->set(idx, smash::PageState::EMPTY);
        vm->untrackExternalPage(p);
    }
}

}  // namespace

// ── mmap / munmap ───────────────────────────────────────────────────────────

using mmap_fn = void*(*)(void*, size_t, int, int, int, off_t);
using munmap_fn = int(*)(void*, size_t);

extern "C" void* smash_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
SMASH_INTERPOSE(smash_mmap, mmap);
extern "C" void* smash_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    void* ret = reinterpret_cast<mmap_fn>(smash_interpose_smash_mmap.original)(
        addr, len, prot, flags, fd, offset);
    if (ret == MAP_FAILED) return ret;
    // Filter to anonymous, writable, non-zero-length mappings. File-backed
    // (fd != -1 or !MAP_ANON) and read-only mappings are ineligible.
    bool anon = (flags & MAP_ANON) != 0;
    bool writable = (prot & PROT_WRITE) != 0;
    if (!anon || !writable || len == 0) return ret;
    auto* vm = smash::g_smash_vm_region;
    if (vm) registerExternalRange(vm, ret, len);
    return ret;
}

extern "C" int smash_munmap(void* addr, size_t len);
SMASH_INTERPOSE(smash_munmap, munmap);
extern "C" int smash_munmap(void* addr, size_t len) {
    auto* vm = smash::g_smash_vm_region;
    if (vm && addr && len) deregisterExternalRange(vm, addr, len);
    return reinterpret_cast<munmap_fn>(smash_interpose_smash_munmap.original)(addr, len);
}

// ── mach_vm_allocate / mach_vm_deallocate ────────────────────────────────────
//
// SpiderMonkey on macOS uses mach_vm_allocate (not POSIX mmap) for its GC
// arenas. CoreGraphics/IOSurface paths also reach the kernel via Mach VM.
// mach_vm_allocate is anonymous-by-design (file-backed mappings go through
// mach_vm_map with a memory object); tracking is unconditional within Mach
// VM, gated only on the requested protection including write.
//
// Note: a few mach_vm_allocate callers immediately mach_vm_protect down to
// PROT_NONE. Tracking those is harmless — the compressor will see them as
// EMPTY-equivalent (no write-fault → no dirty bit → no compress).

using mach_vm_allocate_fn = kern_return_t(*)(vm_map_t, mach_vm_address_t*,
                                              mach_vm_size_t, int);
using mach_vm_deallocate_fn = kern_return_t(*)(vm_map_t, mach_vm_address_t,
                                                mach_vm_size_t);
using vm_allocate_fn = kern_return_t(*)(vm_map_t, vm_address_t*, vm_size_t, int);
using vm_deallocate_fn = kern_return_t(*)(vm_map_t, vm_address_t, vm_size_t);

extern "C" kern_return_t smash_mach_vm_allocate(vm_map_t target,
                                                 mach_vm_address_t* address,
                                                 mach_vm_size_t size, int flags);
SMASH_INTERPOSE(smash_mach_vm_allocate, mach_vm_allocate);
extern "C" kern_return_t smash_mach_vm_allocate(vm_map_t target,
                                                 mach_vm_address_t* address,
                                                 mach_vm_size_t size, int flags) {
    kern_return_t kr = reinterpret_cast<mach_vm_allocate_fn>(
        smash_interpose_smash_mach_vm_allocate.original)(target, address, size, flags);
    if (kr != KERN_SUCCESS || !address || size == 0) return kr;
    // Only track allocations in our own task — cross-task allocations
    // belong to a child process and we can't compress remote pages.
    if (target != mach_task_self()) return kr;
    auto* vm = smash::g_smash_vm_region;
    if (vm) registerExternalRange(vm, reinterpret_cast<void*>(*address),
                                   static_cast<size_t>(size));
    return kr;
}

extern "C" kern_return_t smash_mach_vm_deallocate(vm_map_t target,
                                                   mach_vm_address_t address,
                                                   mach_vm_size_t size);
SMASH_INTERPOSE(smash_mach_vm_deallocate, mach_vm_deallocate);
extern "C" kern_return_t smash_mach_vm_deallocate(vm_map_t target,
                                                   mach_vm_address_t address,
                                                   mach_vm_size_t size) {
    if (target == mach_task_self() && address && size) {
        auto* vm = smash::g_smash_vm_region;
        if (vm)
            deregisterExternalRange(vm, reinterpret_cast<void*>(address),
                                     static_cast<size_t>(size));
    }
    return reinterpret_cast<mach_vm_deallocate_fn>(
        smash_interpose_smash_mach_vm_deallocate.original)(target, address, size);
}

extern "C" kern_return_t smash_vm_allocate(vm_map_t target, vm_address_t* address,
                                            vm_size_t size, int flags);
SMASH_INTERPOSE(smash_vm_allocate, vm_allocate);
extern "C" kern_return_t smash_vm_allocate(vm_map_t target, vm_address_t* address,
                                            vm_size_t size, int flags) {
    kern_return_t kr = reinterpret_cast<vm_allocate_fn>(
        smash_interpose_smash_vm_allocate.original)(target, address, size, flags);
    if (kr != KERN_SUCCESS || !address || size == 0) return kr;
    if (target != mach_task_self()) return kr;
    auto* vm = smash::g_smash_vm_region;
    if (vm) registerExternalRange(vm, reinterpret_cast<void*>(*address),
                                   static_cast<size_t>(size));
    return kr;
}

extern "C" kern_return_t smash_vm_deallocate(vm_map_t target, vm_address_t address,
                                              vm_size_t size);
SMASH_INTERPOSE(smash_vm_deallocate, vm_deallocate);
extern "C" kern_return_t smash_vm_deallocate(vm_map_t target, vm_address_t address,
                                              vm_size_t size) {
    if (target == mach_task_self() && address && size) {
        auto* vm = smash::g_smash_vm_region;
        if (vm)
            deregisterExternalRange(vm, reinterpret_cast<void*>(address),
                                     static_cast<size_t>(size));
    }
    return reinterpret_cast<vm_deallocate_fn>(
        smash_interpose_smash_vm_deallocate.original)(target, address, size);
}

#endif // __APPLE__

// Linux syscall interposition is in src/linux_syscall_wrappers.cpp

// ── alloc8 integration ───────────────────────────────────────────────────────

// Required by alloc8's thread interposition for thread-created flag
extern "C" {
volatile int xxthread_created_flag = 0;
}

using SmashRedirect = alloc8::HeapRedirect<smash::SmashHeap>;
ALLOC8_REDIRECT_WITH_THREADS(SmashRedirect);

// ── Start compressor from constructor ─────────────────────────────────────────
// On macOS, threadInit() requires two calls before starting compression (to
// avoid crashing the ObjC runtime during early DYLD init — pthread_create in
// startCompression() triggers task_restartable_ranges_register before ObjC is
// ready). The alloc8 pthread hooks constructor at priority 200 guarantees that
// any threads created by the ObjC runtime during early init pass through
// without calling xxthread_init(). By priority 201, DYLD init is complete and
// it is safe to start the compressor. We call xxthread_init() twice to satisfy
// the >= 1 guard, ensuring compression starts even for non-ObjC programs
// (e.g. Python via DYLD_INSERT_LIBRARIES) and single-threaded programs.
// On Linux, the same applies: no threads are created during LD_PRELOAD init.
__attribute__((constructor(201)))  // After alloc8 pthread hooks init (200)
static void smash_start_main_thread() {
    xxthread_init();
#ifdef __APPLE__
    // Second call satisfies the macOS >= 1 guard in threadInit().
    xxthread_init();
#endif
}
