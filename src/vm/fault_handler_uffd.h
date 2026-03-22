// smash/src/vm/fault_handler_uffd.h - Linux userfaultfd-based page fault handling
//
// Uses userfaultfd(2) for page fault interception on Linux.
// Advantages over signal-based handling:
// - Dedicated handler thread (no async-signal-safe restrictions)
// - No signal handler conflicts with other libraries
// - Can batch-handle multiple faults
// - More precise read/write fault distinction
//
// Requires Linux 4.3+ (4.11+ for UFFDIO_WRITEPROTECT)
#pragma once

#ifdef __linux__

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>
#include <unistd.h>

namespace smash::vm {

// Callback for userfaultfd: receives fault address and buffer to fill.
// Must decompress/prepare the page data into page_buf (4096 bytes).
// Returns true if handled (page_buf filled), false if not our page.
using FaultCallbackUffd = bool(*)(uintptr_t fault_addr, void* page_buf, void* context);

class FaultHandlerUffd {
    int uffd_ = -1;
    FaultCallbackUffd callback_ = nullptr;
    void* callback_ctx_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread handler_thread_;

    // Registered memory regions
    struct Region {
        void* start;
        size_t len;
    };
    std::vector<Region> regions_;

    // Page buffer for decompression (one per handler thread)
    void* page_buf_ = nullptr;
    static constexpr size_t kPageSize = 4096;

    void handlerLoop() {
        // Set thread name for debugging
        pthread_setname_np(pthread_self(), "smash-uffd");

        struct pollfd pfd{};
        pfd.fd = uffd_;
        pfd.events = POLLIN;

        while (running_.load(std::memory_order_relaxed)) {
            int ret = poll(&pfd, 1, 100);  // 100ms timeout for shutdown check
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;  // Error
            }
            if (ret == 0) continue;  // Timeout

            if (pfd.revents & POLLIN) {
                handleFaultEvent();
            }
            if (pfd.revents & (POLLERR | POLLHUP)) {
                break;  // fd closed or error
            }
        }
    }

    void handleFaultEvent() {
        struct uffd_msg msg{};
        ssize_t n = read(uffd_, &msg, sizeof(msg));
        if (n != sizeof(msg)) {
            if (errno == EAGAIN) return;  // No event ready
            return;  // Error
        }

        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            // Could be UFFD_EVENT_FORK, UFFD_EVENT_REMAP, etc.
            // For now, ignore non-pagefault events
            return;
        }

        uintptr_t fault_addr = msg.arg.pagefault.address;
        uintptr_t page_addr = fault_addr & ~(kPageSize - 1);

        // Call the callback to decompress into our buffer
        bool handled = false;
        if (callback_) {
            handled = callback_(fault_addr, page_buf_, callback_ctx_);
        }

        if (handled) {
            // Use UFFDIO_COPY to install the page
            struct uffdio_copy copy{};
            copy.dst = page_addr;
            copy.src = reinterpret_cast<unsigned long>(page_buf_);
            copy.len = kPageSize;
            copy.mode = 0;  // Wake the faulting thread

            if (ioctl(uffd_, UFFDIO_COPY, &copy) < 0) {
                // EEXIST means page was already mapped (race with another handler)
                // ENOENT means the region was unregistered
                // Both are recoverable
                if (errno != EEXIST && errno != ENOENT) {
                    // Real error - try to wake the thread anyway with zeroed page
                    memset(page_buf_, 0, kPageSize);
                    copy.mode = 0;
                    ioctl(uffd_, UFFDIO_COPY, &copy);
                }
            }
        } else {
            // Not our page - this shouldn't happen if regions are registered correctly.
            // Wake the thread with a zero page to avoid deadlock.
            // The thread will likely crash, but that's better than hanging.
            memset(page_buf_, 0, kPageSize);
            struct uffdio_copy copy{};
            copy.dst = page_addr;
            copy.src = reinterpret_cast<unsigned long>(page_buf_);
            copy.len = kPageSize;
            copy.mode = 0;
            ioctl(uffd_, UFFDIO_COPY, &copy);
        }
    }

public:
    FaultHandlerUffd() = default;

    ~FaultHandlerUffd() {
        stop();
        if (page_buf_) {
            munmap(page_buf_, kPageSize);
            page_buf_ = nullptr;
        }
    }

    // Initialize userfaultfd. Call before registerRegion().
    bool init() {
        if (uffd_ >= 0) return true;  // Already initialized

        // Create userfaultfd
        uffd_ = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (uffd_ < 0) {
            return false;  // Kernel doesn't support userfaultfd or seccomp blocked it
        }

        // Check API version and enable features
        struct uffdio_api api{};
        api.api = UFFD_API;
        api.features = 0;  // Request no special features for now

        if (ioctl(uffd_, UFFDIO_API, &api) < 0) {
            close(uffd_);
            uffd_ = -1;
            return false;
        }

        // Allocate page buffer for decompression
        page_buf_ = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page_buf_ == MAP_FAILED) {
            close(uffd_);
            uffd_ = -1;
            page_buf_ = nullptr;
            return false;
        }

        return true;
    }

    // Register a memory region for fault handling.
    // The region must be page-aligned.
    bool registerRegion(void* start, size_t len) {
        if (uffd_ < 0) return false;

        struct uffdio_register reg{};
        reg.range.start = reinterpret_cast<unsigned long>(start);
        reg.range.len = len;
        reg.mode = UFFDIO_REGISTER_MODE_MISSING;  // Handle missing pages

        if (ioctl(uffd_, UFFDIO_REGISTER, &reg) < 0) {
            return false;
        }

        regions_.push_back({start, len});
        return true;
    }

    // Register a single page for fault handling (for per-page registration).
    // More efficient than registerRegion for individual pages.
    bool registerPage(void* page_addr) {
        if (uffd_ < 0) return false;

        struct uffdio_register reg{};
        reg.range.start = reinterpret_cast<unsigned long>(page_addr);
        reg.range.len = kPageSize;
        reg.mode = UFFDIO_REGISTER_MODE_MISSING;

        // EEXIST is OK - page might already be registered
        if (ioctl(uffd_, UFFDIO_REGISTER, &reg) < 0 && errno != EEXIST) {
            return false;
        }
        return true;
    }

    // Unregister a single page.
    bool unregisterPage(void* page_addr) {
        if (uffd_ < 0) return false;

        struct uffdio_range range{};
        range.start = reinterpret_cast<unsigned long>(page_addr);
        range.len = kPageSize;

        // ENOENT is OK - page might not be registered
        if (ioctl(uffd_, UFFDIO_UNREGISTER, &range) < 0 && errno != ENOENT) {
            return false;
        }
        return true;
    }

    // Unregister a memory region.
    bool unregisterRegion(void* start, size_t len) {
        if (uffd_ < 0) return false;

        struct uffdio_range range{};
        range.start = reinterpret_cast<unsigned long>(start);
        range.len = len;

        if (ioctl(uffd_, UFFDIO_UNREGISTER, &range) < 0) {
            return false;
        }

        // Remove from our list
        for (auto it = regions_.begin(); it != regions_.end(); ++it) {
            if (it->start == start && it->len == len) {
                regions_.erase(it);
                break;
            }
        }
        return true;
    }

    // Start the handler thread.
    bool start(FaultCallbackUffd cb, void* ctx) {
        if (uffd_ < 0) {
            if (!init()) return false;
        }

        callback_ = cb;
        callback_ctx_ = ctx;
        running_.store(true);

        handler_thread_ = std::thread(&FaultHandlerUffd::handlerLoop, this);
        return true;
    }

    void stop() {
        if (!running_.load()) return;

        running_.store(false);

        if (handler_thread_.joinable()) {
            handler_thread_.join();
        }

        // Unregister all regions
        for (const auto& r : regions_) {
            struct uffdio_range range{};
            range.start = reinterpret_cast<unsigned long>(r.start);
            range.len = r.len;
            ioctl(uffd_, UFFDIO_UNREGISTER, &range);
        }
        regions_.clear();

        if (uffd_ >= 0) {
            close(uffd_);
            uffd_ = -1;
        }
    }

    // No-op for userfaultfd (no signal handler to reinstall)
    void ensureInstalled() {}

    // Check if userfaultfd is available on this system
    static bool isAvailable() {
        int fd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (fd < 0) return false;
        close(fd);
        return true;
    }

    int fd() const { return uffd_; }
};

} // namespace smash::vm

#endif // __linux__
