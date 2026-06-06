// smash/src/vm/page_state.h - Per-page state machine
//
// Tracks the lifecycle of every page in the VmRegion:
//   EMPTY → ACTIVE → ACTIVE_MONITORING → COMPRESSING → COMPRESSED → ACTIVE
//   Deferred-reclaim mode adds: COMPRESSING → COMPRESSED_SHADOW → COMPRESSED
#pragma once

#include "smash/config.h"
#include "../core/bootstrap_alloc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

namespace smash {

enum class PageState : uint8_t {
    EMPTY             = 0,  // Virtual reservation only, not committed
    ACTIVE            = 1,  // Committed, PROT_READ|PROT_WRITE
    COMPRESSED        = 2,  // Data compressed, page is PROT_NONE
    COMPRESSING       = 3,  // Compressor thread is working on this page
    ACTIVE_MONITORING = 4,  // PROT_READ for write-fault access tracking
    COMPRESSED_SHADOW = 5,  // Compressed copy stored; page still PROT_RW (deferred reclaim)
};

class PageStateTable {
    std::atomic<uint8_t>* states_;
    size_t num_pages_;

public:
    void init(size_t num_pages) {
        num_pages_ = num_pages;
        states_ = bootstrapArray<std::atomic<uint8_t>>(num_pages);
    }

    PageState get(size_t page_idx) const {
        return static_cast<PageState>(states_[page_idx].load(std::memory_order_acquire));
    }

    void set(size_t page_idx, PageState state) {
        states_[page_idx].store(static_cast<uint8_t>(state), std::memory_order_release);
    }

    // CAS transition. Returns true on success.
    bool transition(size_t page_idx, PageState expected, PageState desired) {
        auto exp = static_cast<uint8_t>(expected);
        return states_[page_idx].compare_exchange_strong(
            exp, static_cast<uint8_t>(desired),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    size_t numPages() const { return num_pages_; }
};

// Per-page spinlock table for compression synchronization.
// Fine-grained: one atomic_flag per page, plus an owner-TID slot so the
// fault handler can detect when the current thread already holds the
// lock (and bail out instead of self-deadlocking on the spinlock).
//
// Portability: pthread_t is available on both Linux and macOS, compares
// with pthread_equal. The owner slot is a plain (non-atomic) pthread_t —
// only the lock owner reads/writes it, so no atomicity needed beyond the
// release fence implied by atomic_flag.clear().
class PageLockTable {
    std::atomic_flag* locks_;
    pthread_t* owners_;
    size_t num_pages_;

public:
    void init(size_t num_pages) {
        num_pages_ = num_pages;
        // atomic_flag is 1 byte; bootstrap returns zeroed memory (= clear state)
        locks_ = static_cast<std::atomic_flag*>(
            BootstrapAlloc::instance().allocateZeroed(
                num_pages * sizeof(std::atomic_flag),
                alignof(std::atomic_flag)));
        owners_ = static_cast<pthread_t*>(
            BootstrapAlloc::instance().allocateZeroed(
                num_pages * sizeof(pthread_t),
                alignof(pthread_t)));
    }

    void lock(size_t page_idx) {
        while (locks_[page_idx].test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
        }
        owners_[page_idx] = pthread_self();
    }

    // Non-blocking lock attempt. Returns true if lock was acquired.
    bool tryLock(size_t page_idx) {
        if (locks_[page_idx].test_and_set(std::memory_order_acquire)) return false;
        owners_[page_idx] = pthread_self();
        return true;
    }

    void unlock(size_t page_idx) {
        // Clear owner before releasing the flag — otherwise another thread
        // can grab the lock, write a new owner, and then we'd clobber it.
        owners_[page_idx] = pthread_t{};
        locks_[page_idx].clear(std::memory_order_release);
    }

    // Used by the fault handler to detect self-recursion. compressPage
    // holds the lock while doing memcpy after PROT_READ; if the memcpy
    // SIGSEGVs (TLB inconsistency or smash state-machine bug), the
    // signal is delivered to the same thread, and handleFault must NOT
    // call lock() again — that would self-deadlock on the non-recursive
    // spinlock. Returns true if THIS thread is the current lock owner.
    bool heldByThisThread(size_t page_idx) const {
        // Read order: check the flag is set, then read owner. If flag is
        // clear, no one holds the lock; owner read is racy but irrelevant.
        // If flag is set and owner equals pthread_self(), it's us.
        // (We assume pthread_t reads are atomic enough on the architectures
        // we care about — pointer-sized on Linux glibc, opaque struct on
        // some platforms but in practice 8-byte aligned. The test_and_set
        // earlier means the lock holder already wrote owners_[page_idx];
        // we're just reading the value back.)
        return pthread_equal(owners_[page_idx], pthread_self()) != 0;
    }
};

} // namespace smash
