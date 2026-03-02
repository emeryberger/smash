// smash/src/vm/page_state.h - Per-page state machine
//
// Tracks the lifecycle of every page in the VmRegion:
//   EMPTY → ACTIVE → ACTIVE_MONITORING → COMPRESSING → COMPRESSED → ACTIVE
#pragma once

#include "smash/config.h"
#include "../core/bootstrap_alloc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace smash {

enum class PageState : uint8_t {
    EMPTY             = 0,  // Virtual reservation only, not committed
    ACTIVE            = 1,  // Committed, PROT_READ|PROT_WRITE
    COMPRESSED        = 2,  // Data compressed, page is PROT_NONE
    COMPRESSING       = 3,  // Compressor thread is working on this page
    ACTIVE_MONITORING = 4,  // PROT_READ for write-fault access tracking
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
// Fine-grained: one atomic_flag per page.
class PageLockTable {
    std::atomic_flag* locks_;
    size_t num_pages_;

public:
    void init(size_t num_pages) {
        num_pages_ = num_pages;
        // atomic_flag is 1 byte; bootstrap returns zeroed memory (= clear state)
        locks_ = static_cast<std::atomic_flag*>(
            BootstrapAlloc::instance().allocateZeroed(
                num_pages * sizeof(std::atomic_flag),
                alignof(std::atomic_flag)));
    }

    void lock(size_t page_idx) {
        while (locks_[page_idx].test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
        }
    }

    // Non-blocking lock attempt. Returns true if lock was acquired.
    bool tryLock(size_t page_idx) {
        return !locks_[page_idx].test_and_set(std::memory_order_acquire);
    }

    void unlock(size_t page_idx) {
        locks_[page_idx].clear(std::memory_order_release);
    }
};

} // namespace smash
