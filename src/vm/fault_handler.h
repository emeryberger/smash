// smash/src/vm/fault_handler.h - Platform-specific page fault interception
//
// Two implementations available on Linux:
// 1. Signal-based (SIGSEGV/SIGBUS) - default, works everywhere
// 2. userfaultfd - optional, set SMASH_USE_USERFAULTFD=ON in CMake
//
// macOS: Signal-based only (compatible with ObjC runtime's restartable ranges)
// Windows: VEH (stub for now)
//
// When userfaultfd is enabled, BOTH handlers run:
// - userfaultfd catches missing pages (COMPRESSED/EMPTY states)
// - Signal handler catches permission violations (COMPRESSING/ACTIVE_MONITORING)
//
// To use userfaultfd on Linux, build with -DSMASH_USE_USERFAULTFD=ON
// Requires Linux 4.3+ kernel and CAP_SYS_PTRACE or /proc/sys/vm/unprivileged_userfaultfd=1
#pragma once

#include "smash/config.h"

// Callback types for fault handlers
namespace smash::vm {

// Signal-based callback: receives faulting address, returns true if handled.
// Handler writes directly to the page and restores permissions.
using FaultCallback = bool(*)(uintptr_t fault_addr, void* context);

// Userfaultfd callback: receives faulting address and buffer to fill.
// Must fill page_buf with 4096 bytes of page data.
// Returns true if handled (page_buf filled), false if not our page.
using FaultCallbackUffd = bool(*)(uintptr_t fault_addr, void* page_buf, void* context);

} // namespace smash::vm

// Always include signal-based handler (needed for all platforms)
#include "fault_handler_signal.h"

// Include userfaultfd handler on Linux when enabled
#if defined(__linux__) && defined(SMASH_USE_USERFAULTFD)
    #include "fault_handler_uffd.h"
    namespace smash::vm {
        inline constexpr bool kUsingUserfaultfd = true;
    }
#else
    namespace smash::vm {
        inline constexpr bool kUsingUserfaultfd = false;
    }
#endif

// Type alias for the primary fault handler
namespace smash::vm {
#if defined(__APPLE__) || defined(__linux__)
    using FaultHandler = FaultHandlerSignal;
#else
    // Windows / other: stub
    class FaultHandler {
    public:
        bool start(FaultCallback, void*) { return false; }
        void stop() {}
        void ensureInstalled() {}
    };
#endif
}
