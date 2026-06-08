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
// instance per thread, no static-TLS-block slot) and links everywhere. Default
// tls_model (local-dynamic) is fine here: this is read once per fault in the
// handler, not on the malloc hot path, so it needn't be initial-exec.
inline int& faultWasWrite() {
    // initial-exec: resolved as a fixed tpidr offset (no __tls_get_addr).
    // This is read/written from inside the SIGSEGV/SIGBUS handler, where
    // local-dynamic access is fatal — __tls_get_addr lazily allocates the
    // dynamic TLS block via the allocator (back into smash) and is not
    // async-signal-safe, so it faults and re-enters the handler, recursing
    // until the stack overflows. initial-exec sidesteps it entirely. The
    // attribute is legal on a function-local static and keeps this header-safe
    // (no .cpp definition needed, links in compress-only/test targets).
    static thread_local __attribute__((tls_model("initial-exec"))) int v = -1;
    return v;
}

#if defined(__APPLE__) || defined(__linux__)

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

    static void signalHandler(int sig, siginfo_t* info, void* ucontext) {
        int saved_errno = errno;
        if (isKernelFault(info) && instance_ && instance_->callback_) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
            // Classify read vs write for the soft-dirty ROI re-dirty signal.
            // x86-64: bit 1 of the page-fault error code (gregs[REG_ERR]) is the
            // write bit. ucontext_t/REG_ERR come from <csignal> (already
            // included); we deliberately do NOT include <ucontext.h>.
            faultWasWrite() = -1;
#if defined(__linux__) && defined(__x86_64__) && defined(REG_ERR)
            if (ucontext) {
                auto* uc = static_cast<const ucontext_t*>(ucontext);
                faultWasWrite() =
                    (uc->uc_mcontext.gregs[REG_ERR] & 0x2) ? 1 : 0;
            }
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
        const char* trace_env = std::getenv("SMASH_SIGTRACE");
        if (trace_env && trace_env[0] == '1') {
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
        const char* bt_env = std::getenv("SMASH_DUMP_CRASH_BT");
        bool want_bt = bt_env && bt_env[0] == '1' &&
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
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
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
        if (!need_reinstall && isDeferredReclaimMode()) {
            sigaction(SIGBUS, nullptr, &current);
            need_reinstall = (current.sa_sigaction != signalHandler);
        }
#endif

        if (!need_reinstall) return;

        struct sigaction sa{};
        sa.sa_sigaction = signalHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &old_sigsegv_);
#ifdef __APPLE__
        if (isDeferredReclaimMode())
            sigaction(SIGBUS, &sa, &old_sigbus_);
#endif
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
