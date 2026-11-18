// smash/src/vm/page_idle.h - idle-page bitmap helpers (Linux, header-only,
// allocation-free; shared by the compressor, smash-idled and smash-estimate).
//
// /sys/kernel/mm/page_idle/bitmap tracks a large folio (THP, mTHP) by its
// HEAD PFN only: the kernel ignores writes to a tail PFN's bit and a tail
// PFN always reads "not idle" (mm/page_idle.c: page_idle_get_folio() refuses
// PageTail). pagemap reports each 4 KiB page's own PFN, so a THP-backed page
// armed by that PFN was never armed and read back as accessed — every page
// of a THP looked hot. Arm and test the head instead; one access anywhere in
// the folio then marks all of its pages accessed, as the kernel tracks it.
#pragma once

#include <cstdint>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace smash::vm {

// /proc/kpageflags bits (include/uapi/linux/kernel-page-flags.h).
constexpr uint64_t kKpfCompoundHead = 1ULL << 15;
constexpr uint64_t kKpfCompoundTail = 1ULL << 16;
constexpr uint64_t kKpfZeroPage = 1ULL << 24;
// Largest folio walked back over (1 GiB of 4 KiB pages); anon THP is 2 MiB.
constexpr uint64_t kMaxFolioPages = 1ULL << 18;

#if defined(__linux__)
// Head PFN of the folio holding `pfn`, given an open /proc/kpageflags fd
// (root). Returns `pfn` itself for a base page, or when the flags cannot be
// read. A tail's head is the nearest lower PFN flagged COMPOUND_HEAD.
inline uint64_t folioHeadPfn(int kpageflags_fd, uint64_t pfn) {
    uint64_t f = 0;
    if (kpageflags_fd < 0 || pfn == 0 ||
        pread(kpageflags_fd, &f, sizeof(f), static_cast<off_t>(pfn * 8)) != static_cast<ssize_t>(sizeof(f)) ||
        !(f & kKpfCompoundTail))
        return pfn;
    uint64_t buf[64];
    uint64_t p = pfn;
    while (p > 0 && pfn - p < kMaxFolioPages) {
        const uint64_t n = p < 64 ? p : 64;
        const uint64_t lo = p - n;
        if (pread(kpageflags_fd, buf, n * 8, static_cast<off_t>(lo * 8)) != static_cast<ssize_t>(n * 8)) break;
        for (uint64_t k = n; k-- > 0;) {
            if (buf[k] & kKpfCompoundHead) return lo + k;
            if (!(buf[k] & kKpfCompoundTail)) return pfn;
        }
        p = lo;
    }
    return pfn;
}
#endif

}  // namespace smash::vm
