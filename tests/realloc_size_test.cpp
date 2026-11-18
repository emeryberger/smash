// Realloc / usable-size contract under interposition, in BOTH malloc modes.
//
// Regression guard for the large-only hang in bench_sqlite on macOS: SQLite's
// default allocator (mem1.c) on Apple allocates through
// malloc_default_zone() and sizes blocks with zone->size(zone, p) — a DIRECT
// call to the default zone's size callback, not malloc_size(). Once alloc8
// promotes its zone to default, that callback answered 0 for pointers it does
// not own, i.e. every small block in large-only mode (those are passed through
// to the system allocator). growOpArray() then computed nOpAlloc = 0 / 24 = 0
// and looped forever growing the VDBE op array (sqlite3VdbeAddOp3 ->
// growOp3 -> growOpArray -> sqlite3Realloc). This program replays that
// pattern with a bounded iteration count, plus the portable contract
// (malloc_usable_size / malloc_size >= requested) across realloc transitions
// that straddle the large-only threshold in both directions.
//
// Run under DYLD_INSERT_LIBRARIES / LD_PRELOAD with SMASH_LARGE_ONLY=0 and 1.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#if defined(__APPLE__)
#include <malloc/malloc.h>
static size_t usable(void* p) { return malloc_size(p); }
#else
#include <malloc.h>
static size_t usable(void* p) { return malloc_usable_size(p); }
#endif

static int g_failures = 0;
#define CHECK(cond, ...)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
      std::fprintf(stderr, __VA_ARGS__);                                   \
      std::fprintf(stderr, "\n");                                          \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

static void fill(void* p, size_t n, unsigned seed) {
  auto* b = static_cast<unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) b[i] = static_cast<unsigned char>((i * 131u + seed) & 0xff);
}
static bool verify(const void* p, size_t n, unsigned seed) {
  auto* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i)
    if (b[i] != static_cast<unsigned char>((i * 131u + seed) & 0xff)) return false;
  return true;
}

#if defined(__APPLE__)
// SQLite mem1.c on Apple: _sqliteZone_ = malloc_default_zone();
// SQLITE_MALLOCSIZE(p) = _sqliteZone_->size(_sqliteZone_, p).
static size_t zone_size(malloc_zone_t* z, void* p) { return z->size(z, p); }

static void sqlite_zone_pattern() {
  malloc_zone_t* zone = malloc_default_zone();
  CHECK(zone && zone->size, "malloc_default_zone() has no size callback");
  if (!zone || !zone->size) return;

  // Every size SQLite asks for during CREATE TABLE is small (<= 16 KiB), so
  // in large-only mode these are all system-passthrough pointers.
  const size_t sizes[] = {1, 24, 100, 1000, 4096, 16384, 16385, 100000, 2u << 20};
  for (size_t n : sizes) {
    void* p = malloc_zone_malloc(zone, n);
    CHECK(p, "malloc_zone_malloc(%zu) failed", n);
    if (!p) continue;
    size_t zs = zone_size(zone, p);
    CHECK(zs >= n, "zone->size(malloc_zone_malloc(%zu)) = %zu (< requested)", n, zs);
    size_t ms = usable(p);
    CHECK(ms >= n, "malloc_size(malloc_zone_malloc(%zu)) = %zu", n, ms);
    malloc_zone_free(zone, p);

    void* q = std::malloc(n);
    zs = zone_size(zone, q);
    CHECK(zs >= n, "zone->size(malloc(%zu)) = %zu (< requested)", n, zs);
    std::free(q);
  }

  // growOpArray(): nNew = nOpAlloc ? 2*nOpAlloc : 1024/sizeof(Op);
  // p = realloc(p, nNew*sizeof(Op)); nOpAlloc = zone->size(p)/sizeof(Op).
  // With a zero size answer nOpAlloc never grows and SQLite spins forever;
  // here we cap the iterations instead.
  const size_t kOp = 24;
  size_t nOpAlloc = 0, nOp = 0;
  void* aOp = nullptr;
  int grows = 0;
  while (nOp < 4096 && grows < 64) {
    if (nOpAlloc <= nOp) {
      size_t nNew = nOpAlloc ? 2 * nOpAlloc : 1024 / kOp;
      void* pNew = malloc_zone_realloc(zone, aOp, nNew * kOp);
      CHECK(pNew, "malloc_zone_realloc(%zu) failed", nNew * kOp);
      if (!pNew) break;
      aOp = pNew;
      nOpAlloc = zone_size(zone, aOp) / kOp;
      ++grows;
      continue;
    }
    ++nOp;
  }
  CHECK(nOp == 4096, "SQLite growOpArray pattern stalled: nOp=%zu nOpAlloc=%zu after %d grows",
        nOp, nOpAlloc, grows);
  malloc_zone_free(zone, aOp);
}
#endif

// Portable: realloc across the large-only threshold (16 KiB small/large
// boundary, 1 MiB VmRegion threshold) in both directions preserves contents
// and reports a usable size >= requested at every step.
static void realloc_transitions() {
  const size_t steps[] = {16, 4096, 16384, 16385, 65536, (1u << 20) + 1, 4u << 20,
                          (1u << 20) + 1, 65536, 16385, 16384, 100, 16};
  void* p = nullptr;
  size_t live = 0;
  unsigned seed = 7;
  for (size_t n : steps) {
    void* q = std::realloc(p, n);
    CHECK(q, "realloc(%zu) failed", n);
    if (!q) return;
    size_t keep = live < n ? live : n;
    CHECK(verify(q, keep, seed), "realloc to %zu lost contents (kept %zu)", n, keep);
    size_t us = usable(q);
    CHECK(us >= n, "usable size after realloc(%zu) = %zu", n, us);
    ++seed;
    fill(q, n, seed);
    p = q;
    live = n;
  }
  std::free(p);
}

int main() {
#if defined(__APPLE__)
  sqlite_zone_pattern();
#endif
  realloc_transitions();
  if (g_failures) {
    std::fprintf(stderr, "realloc_size_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("realloc_size_test: OK\n");
  return 0;
}
