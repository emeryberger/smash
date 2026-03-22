// smash/src/vm/fault_handler_signal.h - Signal-based page fault interception
//
// Uses SIGSEGV/SIGBUS to detect accesses to protected pages.
// Works on both macOS and Linux.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <csignal>

namespace smash::vm {

// Callback type: receives faulting address, returns true if handled.
using FaultCallback = bool(*)(uintptr_t fault_addr, void* context);

class FaultHandlerSignal {
    FaultCallback callback_ = nullptr;
    void* callback_ctx_ = nullptr;
    struct sigaction old_sigsegv_{};
    struct sigaction old_sigbus_{};
    std::atomic<bool> running_{false};

    static FaultHandlerSignal* instance_;

    static void signalHandler(int sig, siginfo_t* info, void* ucontext) {
        int saved_errno = errno;
        if (instance_ && instance_->callback_) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
            if (instance_->callback_(addr, instance_->callback_ctx_)) {
                errno = saved_errno;
                return;  // Fault handled, resume execution
            }
        }
        // Not our fault — chain to previous handler
        struct sigaction* old = (sig == SIGSEGV)
            ? &instance_->old_sigsegv_ : &instance_->old_sigbus_;
        if (old->sa_flags & SA_SIGINFO) {
            old->sa_sigaction(sig, info, ucontext);
        } else if (old->sa_handler == SIG_DFL) {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

public:
    bool start(FaultCallback cb, void* ctx) {
        callback_ = cb;
        callback_ctx_ = ctx;
        instance_ = this;

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
        sigaction(SIGSEGV, &old_sigsegv_, nullptr);
        sigaction(SIGBUS, &old_sigbus_, nullptr);
        running_.store(false);
        instance_ = nullptr;
    }

    // Check if our signal handler is still installed; reinstall if overwritten.
    // Called periodically from CompressorThread::tick().
    void ensureInstalled() {
        if (!running_.load(std::memory_order_relaxed)) return;

        struct sigaction current{};
        sigaction(SIGSEGV, nullptr, &current);

        if (current.sa_sigaction == signalHandler) return;  // Still ours

        // Someone overwrote our handler — reinstall, chaining to them
        struct sigaction sa{};
        sa.sa_sigaction = signalHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);

        sigaction(SIGSEGV, &sa, &old_sigsegv_);
        sigaction(SIGBUS, &sa, &old_sigbus_);
    }

    ~FaultHandlerSignal() { stop(); }
};

inline FaultHandlerSignal* FaultHandlerSignal::instance_ = nullptr;

} // namespace smash::vm
