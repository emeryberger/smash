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
#include <cstdlib>
#include "smash/config.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/exception_types.h>
#include <mach/task.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#endif

namespace smash::vm {

// Callback type: receives faulting address, returns true if handled.
using FaultCallback = bool(*)(uintptr_t fault_addr, void* context);

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

    static void signalHandler(int sig, siginfo_t* info, void* ucontext) {
        int saved_errno = errno;
        if (instance_ && instance_->callback_) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
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
        if (old->sa_flags & SA_SIGINFO) {
            old->sa_sigaction(sig, info, ucontext);
        } else if (old->sa_handler == SIG_DFL) {
            signal(sig, SIG_DFL);
            raise(sig);
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
                int n = snprintf(buf, sizeof(buf),
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
