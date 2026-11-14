// smash/src/util/spinlock.h - Lightweight spinlock (no malloc dependency)
#pragma once

#include <atomic>
#include <sched.h>
#include <cstdint>
#include "thread_safety.h"

namespace smash {

// CPU relax hint for the spin-wait body.
static inline void spinRelax() {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    asm volatile("yield");
#endif
}

// Test-and-test-and-set spinlock with bounded exponential backoff.
//
// The naive test-and-set loop (flag_.test_and_set every iteration) issues a
// bus-locked read-for-ownership on EVERY spin, so N contending threads generate
// an O(N) cache-line ping-pong storm on the single lock word. On a 192-core
// EPYC this collapsed the neuron-cc walrus backend's slab-refill path: profiling
// showed 99.6% of allocateBatch/drain self-time on the `test %cl,%cl` of the
// TAS spin, IPC halved (0.26 vs 0.49) at equal cycles — pure coherence traffic.
//
// TTAS fixes the mechanism: spin on a plain relaxed LOAD (the line stays Shared,
// generating no coherence traffic while the lock is held), and only issue the
// atomic exchange when the lock APPEARS free. Exponential backoff further thins
// the herd of exchanges the instant the lock is released.
class SMASH_CAPABILITY("mutex") Spinlock {
    std::atomic<uint8_t> state_{0};   // 0 = free, 1 = held
    // ~1K pause/yield instructions: tens of microseconds, far longer than
    // any smash critical section, so an uncontended-but-busy holder never
    // triggers it; only a descheduled holder does.
    static constexpr unsigned kSpinsBeforeYield = 1024;

public:
    // The uncontended acquire is always inlined; the contended loop lives
    // out of line. Left to the inliner, lock() as a whole is inlined or not
    // depending on unrelated code growth elsewhere in the LTO unit, and when
    // it isn't, every lock on the malloc/free path pays a call (larson and
    // threadtest at 8 threads lost 23-39% that way after an unrelated change).
    [[gnu::always_inline]] void lock() SMASH_ACQUIRE() {
        if (tryLockRelaxed()) [[likely]] return;
        lockSlow();
    }

    [[gnu::always_inline]] void unlock() SMASH_RELEASE() {
        state_.store(0, std::memory_order_release);
    }

    [[gnu::always_inline]] bool tryLock() SMASH_TRY_ACQUIRE(true) {
        return tryLockRelaxed();
    }

private:
    [[gnu::noinline]] void lockSlow() SMASH_NO_TS_ANALYSIS {
        unsigned backoff = 1;
        for (;;) {
            // Read-spin: no atomic RMW, line stays Shared, zero coherence
            // traffic while another core holds the lock. If the holder is
            // not running (preempted on an oversubscribed machine), spinning
            // only burns the time slice it needs: yield after a bounded spin.
            unsigned spins = 0;
            while (state_.load(std::memory_order_relaxed) != 0) {
                spinRelax();
                if (++spins == kSpinsBeforeYield) { sched_yield(); spins = 0; }
            }
            // Lock looks free — attempt the one atomic exchange.
            if (tryLockRelaxed()) return;
            // Lost the race; back off to spread out the retry storm.
            for (unsigned i = 0; i < backoff; ++i) spinRelax();
            if (backoff < 1024) backoff <<= 1;
        }
    }

    // Single atomic acquire attempt. Acquire ordering on success.
    [[gnu::always_inline]] bool tryLockRelaxed() {
        uint8_t expected = 0;
        return state_.compare_exchange_strong(
            expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
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
