// smash/src/util/spinlock.h - Lightweight spinlock (no malloc dependency)
#pragma once

#include <atomic>
#include "thread_safety.h"

namespace smash {

class SMASH_CAPABILITY("mutex") Spinlock {
    std::atomic_flag flag_{};

public:
    void lock() SMASH_ACQUIRE() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
            asm volatile("yield");
#endif
        }
    }

    void unlock() SMASH_RELEASE() {
        flag_.clear(std::memory_order_release);
    }

    bool tryLock() SMASH_TRY_ACQUIRE(true) {
        return !flag_.test_and_set(std::memory_order_acquire);
    }
};

class SMASH_SCOPED_CAPABILITY LockGuard {
    Spinlock& lock_;
public:
    explicit LockGuard(Spinlock& lock) SMASH_ACQUIRE(lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() SMASH_RELEASE() { lock_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

} // namespace smash
