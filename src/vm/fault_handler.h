// smash/src/vm/fault_handler.h - Platform-specific page fault interception
//
// macOS:
//   - Default: SIGSEGV/SIGBUS signal handlers (compatible with ObjC
//     restartable ranges).
//   - SMASH_USE_MACH_EXCEPTIONS=1: task-level Mach exception ports for
//     EXC_BAD_ACCESS, with a dedicated handler thread. Lets smash
//     intercept protection faults before they're converted to signals,
//     which avoids the SpiderMonkey/V8 signal-handler-chain races.
// Linux: SIGSEGV/SIGBUS signal handler (userfaultfd in Phase 5)
// Windows: VEH (stub for now)
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <execinfo.h>
#include "platform_mem.h"  // g_uffd_fd (SIGBUS ownership in uffd mode)
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include "smash/config.h"
#include "../util/safe_printf.h"  // signal-handler-safe snprintf

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/exception_types.h>
#include <mach/task.h>
#include <pthread.h>
#endif

namespace smash::vm {

// Callback type: receives faulting address, returns true if handled.
using FaultCallback = bool(*)(uintptr_t fault_addr, void* context);

// Read-vs-write classification of the in-flight fault, for the decompress
// callback (soft-dirty ROI). signalHandler sets it on the faulting thread just
// before calling the callback, so a thread_local is race-free across concurrent
// faults. 1 = write, 0 = read, -1 = unknown (Mach path / non-x86). NOTE: we
// rely on REG_ERR/ucontext_t already being visible via <csignal> — do NOT add
// <ucontext.h>, which conflicts with the signal includes on glibc and corrupts
// the handler's ucontext view (caused a teardown SIGSEGV/SIGBUS).
// Accessor over a FUNCTION-LOCAL thread_local. A namespace-scope inline
// thread_local in this widely-included, LD_PRELOAD'd header broke the static
// TLS block (teardown SIGSEGV); a single .cpp definition fails to link in the
// targets that don't pull in smash_heap.cpp (compress-only, tests). A
// function-local static thread_local is header-safe (one lazily-initialized
// instance per thread, no static-TLS-block slot) and links everywhere.
//
// TLS model is PLATFORM-SPLIT and the split is load-bearing:
//   Linux: initial-exec — resolved as a fixed tpidr offset (no __tls_get_addr).
//     This is read/written from inside the SIGSEGV/SIGBUS handler, where the
//     default local-dynamic model is fatal: __tls_get_addr lazily allocates the
//     dynamic TLS block via the allocator (back into smash) and is not
//     async-signal-safe, so it faults, re-enters the handler, and recurses
//     until the stack overflows. initial-exec sidesteps it entirely.
//   macOS: DEFAULT model (no attribute). Apple's linker does NOT support the
//     initial-exec (static-offset) TLS model for a dylib loaded via
//     DYLD_INSERT_LIBRARIES — the attribute on a function-local static there
//     miscompiles into a TLS-descriptor sequence that traps with SIGILL inside
//     the fault handler under a live compressor (caught by macOS CI; Linux is
//     fine). macOS uses the signal-handler path too, but the write-bit
//     classification below is __x86_64__-only, so on Apple silicon faultWasWrite
//     is only ever -1 and the default-model lazy init is not exercised the way
//     Linux's recursive case is.
#if defined(__linux__)
inline int& faultWasWrite() {
    static thread_local __attribute__((tls_model("initial-exec"))) int v = -1;
    return v;
}
#else
inline int& faultWasWrite() {
    static thread_local int v = -1;
    return v;
}
#endif

// Signal of the fault being handled (Linux): in userfaultfd mode SIGBUS is a
// MISSING page (smash dropped it, or never populated), SIGSEGV a protection
// fault (a PROT_NONE/PROT_READ page from the classic path). handleFault
// dispatches on it. 0 = unknown (macOS / not in a fault).
#if defined(__linux__)
inline __thread __attribute__((tls_model("initial-exec"))) int g_fault_sig = 0;
inline int faultSignal() { return g_fault_sig; }
#else
inline int faultSignal() { return 0; }
#endif

// Per-thread recursion depth for the SIGSEGV/SIGBUS handler (bounded re-entry
// guard — see signalHandler). LINUX ONLY.
//
// The unbounded recursion this guards against is a Linux mechanism: a fault
// inside libc's __dtoa/__Balloc malloc path re-enters the SA_NODEFER handler
// on a fresh address each time, and the default-model TLS access itself
// recurses via __tls_get_addr → stack overflow. On macOS we must NOT touch
// any TLS from inside the signal handler at all: the first per-thread access
// routes through the dyld thread-local resolver, whose lazy init traps SIGILL
// in a signal context (the same reason faultWasWrite is kept untouched on
// macOS; adding this guard there made bench_sqlite die -4 in CI). macOS
// therefore keeps master's guard-free handler, which is stable in CI.
//
// initial-exec so the static-offset access needs no __tls_get_addr; __thread
// (C-style) has no lazy-init guard.
#if defined(__linux__)
inline __thread __attribute__((tls_model("initial-exec"))) int g_fault_depth = 0;
inline int& faultDepth() { return g_fault_depth; }
#endif

#if defined(__APPLE__) || defined(__linux__)

#if defined(__linux__) && defined(__aarch64__)
// Walk the arm64 signal frame's context records (struct _aarch64_ctx
// {u32 magic; u32 size;}, terminated by magic 0) for the ESR record
// (ESR_MAGIC, followed by the u64 ESR_EL1 value). Constants are spelled out
// rather than taken from <asm/sigcontext.h>, which clashes with glibc's
// <signal.h>. Returns 1 write, 0 read, -1 unknown. Async-signal-safe.
inline int aarch64FaultIsWrite(const unsigned char* p, size_t len) {
    constexpr uint32_t kEsrMagic = 0x45535201;
    size_t off = 0;
    while (off + 8 <= len) {
        uint32_t magic, size;
        __builtin_memcpy(&magic, p + off, 4);
        __builtin_memcpy(&size, p + off + 4, 4);
        if (magic == 0 || size < 8 || off + size > len) return -1;
        if (magic == kEsrMagic && size >= 16) {
            uint64_t esr;
            __builtin_memcpy(&esr, p + off + 8, 8);
            const uint64_t ec = esr >> 26;
            if (ec != 0x24 && ec != 0x25) return -1;
            return ((esr >> 6) & 1) ? 1 : 0;
        }
        off += size;
    }
    return -1;
}
#endif

// ── Per-thread alternate signal stacks (SMASH_TRACK_EXTERNAL) ───────────────
// With external tracking, a plain RW mmap can be a coroutine / fiber stack
// (makecontext, boost.context, folly fibers: no MAP_STACK). While the
// coroutine is suspended its stack is cold and gets compressed; on resume the
// first touch faults with the stack pointer INSIDE the inaccessible range, so
// the kernel cannot push a signal frame there and kills the process
// (external_patterns_test "ucstack"). Running the handler on an alternate
// stack fixes that: smash gives every thread it sees (threadInit) a 64 KiB
// sigaltstack mapped with rawMmap (never tracked) and installs its handler
// with SA_ONSTACK. Threads without an alternate stack are unaffected by
// SA_ONSTACK; a thread whose app installs its own keeps the app's (see the
// sigaltstack interposers, which also untrack it).
// SMASH_ALTSTACK=0 disables; default on iff SMASH_TRACK_EXTERNAL=1.
inline bool altStackWanted() {
    static const bool on = [] {
        const char* te = std::getenv("SMASH_TRACK_EXTERNAL");
        const char* as = std::getenv("SMASH_ALTSTACK");
        if (as) return as[0] == '1';
        return te && te[0] == '1';
    }();
    return on;
}
inline constexpr size_t kAltStackSize = 64 * 1024;
#if defined(__linux__)
inline __thread __attribute__((tls_model("initial-exec"))) char* g_altstack_mem = nullptr;
inline int rawSigaltstack(const stack_t* ss, stack_t* old) {
    return static_cast<int>(syscall(SYS_sigaltstack, ss, old));
}
#else
inline thread_local char* g_altstack_mem = nullptr;
inline int rawSigaltstack(const stack_t* ss, stack_t* old) { return sigaltstack(ss, old); }
#endif
// Give the calling thread an alternate signal stack unless it has one.
inline void installThreadAltStack() {
    if (!altStackWanted() || g_altstack_mem) return;
    stack_t cur{};
    if (rawSigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) return;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t total = kAltStackSize + page;  // + low guard page
    void* m = rawMmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);
    if (m == MAP_FAILED) return;
    (void)rawMprotect(m, page, PROT_NONE);
    stack_t ss{};
    ss.ss_sp = static_cast<char*>(m) + page;
    ss.ss_size = kAltStackSize;
    ss.ss_flags = 0;
    if (rawSigaltstack(&ss, nullptr) != 0) { rawMunmap(m, total); return; }
    g_altstack_mem = static_cast<char*>(m);
}
// Thread exit: disable the alternate stack if it is still ours, unmap it.
inline void releaseThreadAltStack() {
    char* m = g_altstack_mem;
    if (!m) return;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    stack_t cur{};
    if (rawSigaltstack(nullptr, &cur) == 0 && cur.ss_sp == m + page) {
        if (cur.ss_flags & SS_ONSTACK) return;  // running on it (can't happen at exit)
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        (void)rawSigaltstack(&off, nullptr);
    }
    g_altstack_mem = nullptr;
    rawMunmap(m, kAltStackSize + page);
}

// Signal-based fault handler for macOS and Linux.
// Uses SIGSEGV/SIGBUS to detect accesses to protected pages.
class FaultHandler {
    FaultCallback callback_ = nullptr;
    void* callback_ctx_ = nullptr;
    struct sigaction old_sigsegv_{};
    struct sigaction old_sigbus_{};
    std::atomic<bool> running_{false};

#ifdef __APPLE__
    // Mach exception ports mode (SMASH_USE_MACH_EXCEPTIONS=1).
    bool mach_mode_ = false;
    mach_port_t mach_exc_port_ = MACH_PORT_NULL;
    pthread_t mach_handler_thread_ = 0;
#endif

    static FaultHandler* instance_;

    // Set when we've decided this signal terminates the process.
    // ensureInstalled() checks it before re-arming our handler so the
    // compressor's periodic re-install doesn't race with the
    // sigaction(SIG_DFL) → raise() chain we use to propagate
    // user-space-synthesized SIGSEGV/SIGBUS to the original
    // disposition.
    static inline std::atomic<bool> shutting_down_{false};

    // Signal-handler env flags, cached at install time. getenv() is NOT
    // async-signal-safe: calling it from signalHandler() can fault inside
    // libc's environ walk, and with SA_NODEFER that re-enters signalHandler,
    // which calls getenv() again → unbounded recursion → stack-overflow
    // SIGSEGV. (Observed as redis-server under LD_PRELOAD dumping core with a
    // getenv → signalHandler → __restore_rt → getenv … backtrace.) Read these
    // once in start()/ensureInstalled() and consult the cached bools only.
    static inline bool sigtrace_enabled_ = false;
    static inline bool dump_crash_bt_enabled_ = false;

    // Detect signals that were synthesized by user-space (raise(),
    // pthread_kill(), kill(), tgkill()) rather than from a real memory
    // fault. For these, info->si_code is non-positive (SI_USER=0,
    // SI_TKILL=-6, SI_QUEUE=-1, etc.) and info->si_addr is not a real
    // faulting address. POSIX guarantees positive si_code values are
    // kernel-synchronous fault sources (SEGV_MAPERR=1, SEGV_ACCERR=2,
    // BUS_ADRALN, etc.).
    //
    // For user-synthesized SIGSEGV/SIGBUS the correct chain action is
    // "act as if smash hadn't installed a handler at all": uninstall
    // ourselves and re-raise so the original disposition (often
    // SIG_DFL → terminate, sometimes a Python C-level handler) takes
    // over. Without this carve-out, a user-space raise(SIGSEGV)
    // fed through smash's handler chains to whatever old_sigsegv_
    // was — which on Linux+Python is SIG_DFL — and the chain logic
    // works *most* of the time, but races with ensureInstalled() can
    // re-arm our handler in the µs-window between signal(SIG_DFL)
    // and raise(), producing an infinite loop.
    //
    // WalrusDriver.py:518 calls signal.raise_signal(-rc) on backend
    // SIGSEGV exits, which is the production trigger for this path.
    static bool isKernelFault(const siginfo_t* info) {
        return info->si_code > 0;
    }

    // Bounded re-entrancy guard. The SIGSEGV/SIGBUS handlers are installed
    // with SA_NODEFER (deliberate: a legitimate decompress can itself fault
    // once, e.g. prefetching an adjacent compressed page, and that nested
    // fault must be handled). But if callback_ (handleFault) faults on a
    // DIFFERENT address every time — e.g. when the originating fault came
    // from inside libc's __dtoa/__Balloc malloc path while formatting a
    // float — the per-page reentrancy guard never matches and the handler
    // recurses without bound until it overflows the signal stack
    // ("Could not determine thread index for stack guard region").
    // Cap the depth: past kMaxFaultDepth nested kernel faults on one thread
    // we stop trying to handle and let the fault chain to the previous
    // disposition, producing a clean crash with a real backtrace instead of
    // a stack-overflow. 2 allows one legitimate nested decompress fault.
    static constexpr int kMaxFaultDepth = 2;

    static void signalHandler(int sig, siginfo_t* info, void* ucontext) {
        int saved_errno = errno;
        // Bounded re-entry guard is LINUX ONLY — see faultDepth() decl for why
        // macOS must not touch TLS in the handler. On macOS depthOk is always
        // true (master's guard-free behavior); the Linux path caps recursion.
#if defined(__linux__)
        int& depth = faultDepth();
        const bool depthOk = depth < kMaxFaultDepth;
#else
        const bool depthOk = true;
#endif
        if (isKernelFault(info) && instance_ && instance_->callback_ && depthOk) {
#if defined(__linux__)
            ++depth;
            struct DepthGuard {
                int& d;
                ~DepthGuard() { --d; }
            } depth_guard{depth};
#endif
            uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
#if defined(__linux__)
            const int prev_sig = g_fault_sig;
            g_fault_sig = sig;
            struct SigRestore { int v; ~SigRestore() { g_fault_sig = v; } } sig_restore{prev_sig};
#endif
            // Classify read vs write for the soft-dirty ROI re-dirty signal.
            // x86-64: bit 1 of the page-fault error code (gregs[REG_ERR]) is the
            // write bit. ucontext_t/REG_ERR come from <csignal> (already
            // included); we deliberately do NOT include <ucontext.h>.
            //
            // LINUX ONLY: touch faultWasWrite() here. On macOS the value is
            // never set meaningfully (the write-bit read below is Linux-x86
            // only) and the lone reader (compressor_thread.h) already treats
            // the unset/-1 case as "write". More importantly, the macOS
            // function-local thread_local carries a lazy-init guard that, when
            // first run INSIDE this signal handler under a live compressor,
            // traps with SIGILL (observed deterministically in macOS CI:
            // bench_sqlite dies in the fault/decompress phase). Not touching
            // the TLS in the handler on macOS avoids that guard entirely; the
            // compressor thread inits it lazily (safely, not in a handler) on
            // its first read.
#if defined(__linux__)
            faultWasWrite() = -1;
#if defined(__x86_64__) && defined(REG_ERR)
            if (ucontext) {
                auto* uc = static_cast<const ucontext_t*>(ucontext);
                faultWasWrite() =
                    (uc->uc_mcontext.gregs[REG_ERR] & 0x2) ? 1 : 0;
            }
#elif defined(__aarch64__)
            // arm64: the kernel appends an esr_context record to the signal
            // frame's __reserved area for data aborts. For a data abort
            // (ESR.EC 0x24 lower EL / 0x25 same EL), ESR.WnR (bit 6) is the
            // write bit. Without this every arm64 fault read as "write".
            if (ucontext) {
                auto* uc = static_cast<const ucontext_t*>(ucontext);
                faultWasWrite() = aarch64FaultIsWrite(uc->uc_mcontext.__reserved,
                                                      sizeof(uc->uc_mcontext.__reserved));
            }
#endif
#else
            (void)ucontext;
#endif
            if (instance_->callback_(addr, instance_->callback_ctx_)) {
                errno = saved_errno;
                return;  // Fault handled, resume execution
            }
        }
        // Not our fault — chain to previous handler.
        // KNOWN ISSUE on macOS with SpiderMonkey/V8/wasm: those engines
        // install their own SIGBUS handler in a way that, combined with
        // ensureInstalled() re-claiming, can recurse smash → engine →
        // smash. The clean fix is task-level Mach exception ports
        // instead of POSIX signal handlers. Tracked in FIREFOX_PLAN.md.
        struct sigaction* old = (sig == SIGSEGV)
            ? &instance_->old_sigsegv_ : &instance_->old_sigbus_;

        // SMASH_SIGTRACE=1 — async-signal-safe one-line trace per chain.
        // Flag cached at install time; getenv() here would not be
        // async-signal-safe (see sigtrace_enabled_ declaration).
        if (sigtrace_enabled_) {
            char buf[160];
            const char* kind = "?";
            if (old->sa_flags & SA_SIGINFO) kind = "SA_SIGINFO";
            else if (old->sa_handler == SIG_DFL) kind = "SIG_DFL";
            else if (old->sa_handler == SIG_IGN) kind = "SIG_IGN";
            else kind = "OTHER";
            int n = smash::safe_snprintf(buf, sizeof(buf),
                "[smash sig] sig=%d si_code=%d si_addr=%p chain_kind=%s flags=0x%x\n",
                sig, info->si_code, info->si_addr, kind,
                (unsigned)old->sa_flags);
            if (n > 0) (void)!write(2, buf, (size_t)n);
        }

        // For non-kernel-synchronous faults (raise/kill/etc.) that we
        // didn't handle, we MUST uninstall ourselves before returning;
        // otherwise the kernel resumes the faulting instruction (the
        // raise() syscall return), the caller's next instruction
        // executes, and the user's intent (terminate the process) is
        // silently dropped. By restoring the old sigaction and calling
        // raise() ourselves, we let the original disposition fire.
        if (!isKernelFault(info)) {
            shutting_down_.store(true, std::memory_order_release);
            sigaction(sig, old, nullptr);
            raise(sig);
            errno = saved_errno;
            return;
        }

        // SMASH_DUMP_CRASH_BT=1 — dump a backtrace before terminating.
        // Useful for diagnosing real segfaults that smash didn't handle
        // (e.g. compiler crashes in app code under full smash mode).
        // Only fires for kernel-fault SIGSEGV/SIGBUS that chain to
        // SIG_DFL — i.e. the process is about to die anyway.
        // Flag cached at install time (getenv() is not async-signal-safe).
        bool want_bt = dump_crash_bt_enabled_ &&
                       isKernelFault(info) &&
                       !(old->sa_flags & SA_SIGINFO) &&
                       old->sa_handler == SIG_DFL;
        if (want_bt) {
            // Portable thread id: SYS_gettid is Linux-only; macOS uses
            // pthread_threadid_np. Diagnostic-only.
#if defined(__linux__)
            long tid = syscall(SYS_gettid);
#elif defined(__APPLE__)
            uint64_t tid64 = 0; pthread_threadid_np(nullptr, &tid64);
            long tid = static_cast<long>(tid64);
#else
            long tid = 0;
#endif
            char hdr[160];
            int n = smash::safe_snprintf(hdr, sizeof(hdr),
                "[smash crash] pid=%d tid=%ld sig=%d si_code=%d si_addr=%p\n",
                (int)getpid(), tid,
                sig, info->si_code, info->si_addr);
            if (n > 0) (void)!write(2, hdr, (size_t)n);
            void* frames[64];
            int nframes = backtrace(frames, 64);
            backtrace_symbols_fd(frames, nframes, 2);
        }

        if (old->sa_flags & SA_SIGINFO) {
            old->sa_sigaction(sig, info, ucontext);
        } else if (old->sa_handler == SIG_DFL) {
            shutting_down_.store(true, std::memory_order_release);
            sigaction(sig, old, nullptr);
            raise(sig);
        } else if (old->sa_handler == SIG_IGN) {
            // Synchronous fault + caller asked to ignore = the
            // faulting instruction would re-execute forever. Force
            // SIG_DFL termination instead.
            shutting_down_.store(true, std::memory_order_release);
            struct sigaction dfl{};
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            sigaction(sig, &dfl, nullptr);
            raise(sig);
        } else {
            old->sa_handler(sig);
        }
    }

#ifdef __APPLE__
    // Layout copied from /usr/include/mach/mach_exc.defs (the
    // exception_raise routine the kernel uses to deliver
    // EXC_BAD_ACCESS to a task-level exception port). Same shape
    // SpiderMonkey uses in WasmSignalHandlers.cpp:605.
#pragma pack(push, 4)
    struct ExceptionRaiseRequest {
        mach_msg_header_t Head;
        mach_msg_body_t msgh_body;
        mach_msg_port_descriptor_t thread;
        mach_msg_port_descriptor_t task;
        NDR_record_t NDR;
        exception_type_t exception;
        mach_msg_type_number_t codeCnt;
        int64_t code[2];
    };
    struct ExceptionRaiseRequestWithTrailer {
        ExceptionRaiseRequest body;
        mach_msg_trailer_t trailer;
    };
    struct ExceptionRaiseReply {
        mach_msg_header_t Head;
        NDR_record_t NDR;
        kern_return_t RetCode;
    };
#pragma pack(pop)

    static void* machHandlerThreadEntry(void* arg) {
        auto* self = static_cast<FaultHandler*>(arg);
        // Standard reply id is request.msgh_id + 100 per mach_exc.defs.
        for (;;) {
            ExceptionRaiseRequestWithTrailer req;
            kern_return_t kr = mach_msg(&req.body.Head, MACH_RCV_MSG, 0,
                                        sizeof(req), self->mach_exc_port_,
                                        MACH_MSG_TIMEOUT_NONE,
                                        MACH_PORT_NULL);
            if (kr != KERN_SUCCESS) continue;

            // For EXC_BAD_ACCESS with MACH_EXCEPTION_CODES:
            //   code[0] = subcode (KERN_INVALID_ADDRESS, KERN_PROTECTION_FAILURE)
            //   code[1] = faulting address
            bool handled = false;
            if (req.body.exception == EXC_BAD_ACCESS &&
                req.body.codeCnt >= 2 && self->callback_) {
                uintptr_t addr =
                    static_cast<uintptr_t>(req.body.code[1]);
                handled = self->callback_(addr, self->callback_ctx_);
            }
            // Optional one-line stderr trace per exception, for tuning.
            // SMASH_MACH_TRACE=1 to enable. Async-signal-safe ish (not
            // strictly, but this thread isn't inside a signal handler).
            static const bool trace = []{
                const char* v = std::getenv("SMASH_MACH_TRACE");
                return v && v[0] == '1';
            }();
            if (trace) {
                char buf[128];
                int n = smash::safe_snprintf(buf, sizeof(buf),
                    "[smash mach] exc=%d code=[%llx,%llx] handled=%d\n",
                    req.body.exception,
                    (unsigned long long)req.body.code[0],
                    (unsigned long long)(req.body.codeCnt >= 2 ? req.body.code[1] : 0),
                    handled);
                if (n > 0) write(2, buf, (size_t)n);
            }

            ExceptionRaiseReply reply;
            reply.Head.msgh_bits = MACH_MSGH_BITS(
                MACH_MSGH_BITS_REMOTE(req.body.Head.msgh_bits), 0);
            reply.Head.msgh_size = sizeof(reply);
            reply.Head.msgh_remote_port = req.body.Head.msgh_remote_port;
            reply.Head.msgh_local_port = MACH_PORT_NULL;
            reply.Head.msgh_id = req.body.Head.msgh_id + 100;
            reply.NDR = NDR_record;
            // KERN_SUCCESS  → resume faulting thread (we fixed the page)
            // KERN_FAILURE  → kernel falls back to host (= POSIX signal),
            //                 so any installed signal handler still gets
            //                 a chance.
            reply.RetCode = handled ? KERN_SUCCESS : KERN_FAILURE;
            mach_msg(&reply.Head, MACH_SEND_MSG, sizeof(reply), 0,
                     MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                     MACH_PORT_NULL);
        }
        return nullptr;
    }

    bool startMach() {
        kern_return_t kr;
        kr = mach_port_allocate(mach_task_self(),
                                MACH_PORT_RIGHT_RECEIVE,
                                &mach_exc_port_);
        if (kr != KERN_SUCCESS) return false;

        kr = mach_port_insert_right(mach_task_self(), mach_exc_port_,
                                    mach_exc_port_,
                                    MACH_MSG_TYPE_MAKE_SEND);
        if (kr != KERN_SUCCESS) {
            mach_port_destroy(mach_task_self(), mach_exc_port_);
            mach_exc_port_ = MACH_PORT_NULL;
            return false;
        }

        // Spawn the listener BEFORE registering the port; the kernel
        // could deliver an exception immediately afterwards.
        if (pthread_create(&mach_handler_thread_, nullptr,
                           machHandlerThreadEntry, this) != 0) {
            mach_port_destroy(mach_task_self(), mach_exc_port_);
            mach_exc_port_ = MACH_PORT_NULL;
            return false;
        }

        kr = task_set_exception_ports(
            mach_task_self(),
            EXC_MASK_BAD_ACCESS,
            mach_exc_port_,
            EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
            THREAD_STATE_NONE);
        if (kr != KERN_SUCCESS) {
            // Listener thread is still spinning on mach_msg from a port
            // we haven't registered — it'll just block forever. Leave
            // it; we're doomed anyway since fault interception is what
            // the whole library exists to do.
            return false;
        }
        mach_mode_ = true;
        return true;
    }
#endif // __APPLE__

public:
    bool start(FaultCallback cb, void* ctx) {
        callback_ = cb;
        callback_ctx_ = ctx;
        instance_ = this;

        // Cache signal-handler env flags ONCE here, before the handler can
        // ever run. signalHandler() must not call getenv() (not
        // async-signal-safe; recurses under SA_NODEFER — see flag decls).
        const char* st = std::getenv("SMASH_SIGTRACE");
        sigtrace_enabled_ = st && st[0] == '1';
        const char* bt = std::getenv("SMASH_DUMP_CRASH_BT");
        dump_crash_bt_enabled_ = bt && bt[0] == '1';

#ifdef __APPLE__
        const char* mach_env = std::getenv("SMASH_USE_MACH_EXCEPTIONS");
        if (mach_env && mach_env[0] == '1') {
            if (startMach()) {
                running_.store(true);
                return true;
            }
            // Fall through to signals if Mach setup failed.
        }
#endif

        struct sigaction sa{};
        sa.sa_sigaction = signalHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | (altStackWanted() ? SA_ONSTACK : 0);
        sigemptyset(&sa.sa_mask);

        sigaction(SIGSEGV, &sa, &old_sigsegv_);
        sigaction(SIGBUS, &sa, &old_sigbus_);
        running_.store(true);
        return true;
    }

    void stop() {
        if (!running_.load()) return;
#ifdef __APPLE__
        if (mach_mode_) {
            // Releasing the task exception port and the listener thread
            // cleanly is fiddly; for our use case stop() only fires at
            // process teardown so the kernel reaps both. Just mark down.
            running_.store(false);
            instance_ = nullptr;
            return;
        }
#endif
        sigaction(SIGSEGV, &old_sigsegv_, nullptr);
        sigaction(SIGBUS, &old_sigbus_, nullptr);
        running_.store(false);
        instance_ = nullptr;
    }

    // Idempotent, thread-safe entry point for "make sure the signal
    // handlers are still ours." Safe to call from any thread at any
    // time; no-op if start() hasn't been called yet.
    static void ensureInstalledIfRunning() {
        if (instance_) instance_->ensureInstalled();
    }

    // Check if our signal handlers are still installed; reinstall if
    // overwritten. Called periodically from CompressorThread::tick().
    // We deliberately reinstall **only SIGSEGV**, not SIGBUS — unless
    // deferred-reclaim mode is active. macOS delivers SIGBUS for
    // KERN_PROTECTION_FAILURE on anonymous pages. SpiderMonkey installs
    // its own SIGBUS handler with proper chaining, so re-installing SIGBUS
    // would invert that chain. But for MongoDB (deferred-reclaim target),
    // MongoDB's SIGBUS handler does NOT chain — it aborts. So we must
    // reclaim SIGBUS to handle page faults from PROT_NONE pages.
    void ensureInstalled() {
        if (!running_.load(std::memory_order_relaxed)) return;
        // Don't fight the chained termination path. signalHandler sets
        // this when it's restored SIG_DFL (or another disposition) and
        // is on its way out via raise(); reinstalling our handler here
        // would race the kernel's pending delivery and loop.
        if (shutting_down_.load(std::memory_order_acquire)) return;
#ifdef __APPLE__
        if (mach_mode_) return;
#endif

        struct sigaction current{};
        sigaction(SIGSEGV, nullptr, &current);
        bool need_reinstall = (current.sa_sigaction != signalHandler);

#ifdef __APPLE__
        const bool want_bus = isDeferredReclaimMode();
#else
        // userfaultfd mode delivers smash's faults as SIGBUS; an app that
        // installs its own SIGBUS handler after us (Redis does, at startup)
        // must not keep it.
        const bool want_bus = g_uffd_fd >= 0;
#endif
        bool bus_reinstall = false;
        if (want_bus) {
            sigaction(SIGBUS, nullptr, &current);
            bus_reinstall = (current.sa_sigaction != signalHandler);
        }

        if (!need_reinstall && !bus_reinstall) return;

        struct sigaction sa{};
        sa.sa_sigaction = signalHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | (altStackWanted() ? SA_ONSTACK : 0);
        sigemptyset(&sa.sa_mask);
        if (need_reinstall) sigaction(SIGSEGV, &sa, &old_sigsegv_);
        if (bus_reinstall) sigaction(SIGBUS, &sa, &old_sigbus_);
    }

    ~FaultHandler() { stop(); }
};

inline FaultHandler* FaultHandler::instance_ = nullptr;

#else

// Windows / other: stub
class FaultHandler {
public:
    bool start(FaultCallback, void*) { return false; }
    void stop() {}
};

#endif

} // namespace smash::vm
