// smash/src/util/thread_safety.h - Clang Thread Safety Analysis annotations
//
// Capability/locking attributes understood by clang's -Wthread-safety static
// analysis (https://clang.llvm.org/docs/ThreadSafetyAnalysis.html). They are
// no-ops under any other compiler (and under clang without -Wthread-safety),
// so annotating costs nothing at runtime and lets clang prove — at compile
// time — that shared state is only touched while the right lock is held.
#pragma once

#if defined(__clang__) && (!defined(SWIG))
#define SMASH_TS_ATTR(x) __attribute__((x))
#else
#define SMASH_TS_ATTR(x)  // no-op on non-clang
#endif

#define SMASH_CAPABILITY(x)        SMASH_TS_ATTR(capability(x))
#define SMASH_SCOPED_CAPABILITY    SMASH_TS_ATTR(scoped_lockable)
#define SMASH_GUARDED_BY(x)        SMASH_TS_ATTR(guarded_by(x))
#define SMASH_PT_GUARDED_BY(x)     SMASH_TS_ATTR(pt_guarded_by(x))
#define SMASH_ACQUIRE(...)         SMASH_TS_ATTR(acquire_capability(__VA_ARGS__))
#define SMASH_TRY_ACQUIRE(...)     SMASH_TS_ATTR(try_acquire_capability(__VA_ARGS__))
#define SMASH_RELEASE(...)         SMASH_TS_ATTR(release_capability(__VA_ARGS__))
#define SMASH_REQUIRES(...)        SMASH_TS_ATTR(requires_capability(__VA_ARGS__))
#define SMASH_EXCLUDES(...)        SMASH_TS_ATTR(locks_excluded(__VA_ARGS__))
#define SMASH_NO_TS_ANALYSIS       SMASH_TS_ATTR(no_thread_safety_analysis)
#define SMASH_ASSERT_HELD(x)       SMASH_TS_ATTR(assert_capability(x))
