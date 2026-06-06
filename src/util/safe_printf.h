// smash/src/util/safe_printf.h - Allocation-free printf wrappers
//
// glibc's snprintf/fprintf/sprintf can call malloc internally (see e.g.
// glibc commit history around %g/%a, large field widths, locale data). On
// the malloc fast path, in fault handlers, or in the compressor thread,
// that recursion can deadlock or — under pathological inputs — overflow
// the signal stack. mpaland/printf (vendored as github.com/emeryberger/printf
// via FetchContent) is a self-contained, allocation-free reimplementation.
//
// Use these `safe_*` wrappers anywhere in smash that formats diagnostic
// strings. They expand to the printf library's `*_` functions and never
// touch the heap. Keep using `::write(2, ...)` for the actual fd output —
// stdio is allowed to allocate too.
//
// Note on linkage: printf.h itself does
//   #define snprintf snprintf_
//   #define vsnprintf vsnprintf_
//   #define printf printf_
//   ...
// which would silently shadow the libc names everywhere it's #included.
// We don't want that — Smash's own use of stdio (banner, stats, etc.)
// shares the same TU as malloc-path code in some cases. So we undef the
// macros immediately after the header and expose explicit `safe_*` names
// for the call sites that actually need malloc-free formatting.
#pragma once

#include "printf.h"

// printf.h leaves these as macros redirecting to *_ variants; remove the
// redirection so libc's printf/snprintf/etc. behave normally elsewhere.
#ifdef printf
#undef printf
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef vprintf
#undef vprintf
#endif

namespace smash {

// Allocation-free formatters. Same signatures as the C library equivalents.
inline int safe_snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = ::vsnprintf_(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

inline int safe_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    return ::vsnprintf_(buf, n, fmt, ap);
}

}  // namespace smash
