// smash/src/util/relaxed.h - Racy-by-design cells as relaxed atomics
#pragma once

#include <cstddef>
#include <type_traits>

namespace smash {

// Relaxed<T>: a T-sized, T-aligned cell whose every load and store is a
// relaxed atomic (__atomic builtins on the plain storage, so the layout of the
// per-page metadata arrays is unchanged and they stay zero-initializable from
// BootstrapAlloc).
//
// For per-page hints (cold counters, refault marks, tier tags, ...) that the
// compressor's phases read and update without the page lock while app threads
// (free -> releaseCompressedPages, malloc -> Span cold reset) and the fault
// handler write them. A stale or lost value only delays or hastens a
// compression decision; correctness rests on the PageState CAS and the
// per-page lock. As plain bytes those accesses were C++ data races (undefined
// behaviour, and ThreadSanitizer reports); as relaxed atomics they are defined
// and still compile to ordinary loads and stores on x86-64 and arm64.
//
// The increment/decrement operators are a relaxed load followed by a relaxed
// store, NOT an atomic read-modify-write: the plain code they replace already
// tolerated a lost update, and a locked RMW on these paths would cost cycles.
template <typename T>
struct Relaxed {
    static_assert(std::is_integral_v<T>, "Relaxed<T> holds an integer");
    T raw;

    T load() const { return __atomic_load_n(&raw, __ATOMIC_RELAXED); }
    void store(T v) { __atomic_store_n(&raw, v, __ATOMIC_RELAXED); }

    operator T() const { return load(); }
    Relaxed& operator=(T v) { store(v); return *this; }
    Relaxed& operator++() { store(static_cast<T>(load() + 1)); return *this; }
    Relaxed& operator--() { store(static_cast<T>(load() - 1)); return *this; }
    T operator++(int) { T v = load(); store(static_cast<T>(v + 1)); return v; }
    T operator--(int) { T v = load(); store(static_cast<T>(v - 1)); return v; }
    Relaxed& operator+=(T d) { store(static_cast<T>(load() + d)); return *this; }
    Relaxed& operator-=(T d) { store(static_cast<T>(load() - d)); return *this; }
    Relaxed& operator|=(T m) { store(static_cast<T>(load() | m)); return *this; }
    Relaxed& operator&=(T m) { store(static_cast<T>(load() & m)); return *this; }
};

static_assert(sizeof(Relaxed<unsigned char>) == 1 && alignof(Relaxed<unsigned char>) == 1);
static_assert(sizeof(Relaxed<unsigned>) == sizeof(unsigned));
static_assert(std::is_trivial_v<Relaxed<unsigned char>>);

// Relaxed fill of n cells (replaces memset on a Relaxed array).
template <typename T>
inline void relaxedFill(Relaxed<T>* p, size_t n, T v) {
    for (size_t i = 0; i < n; ++i) p[i].store(v);
}

} // namespace smash
