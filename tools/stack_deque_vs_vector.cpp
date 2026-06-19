// stack_deque_vs_vector.cpp
//
// Demonstrates the heap-space difference between std::stack backed by std::deque
// (the default) vs std::vector / a SmallVector-style inline buffer, for the
// access pattern of bir::QuasiAffineExprIterator: construct a short-lived stack,
// push a small number of pointer-sized elements (a shallow expr-tree DFS), then
// destroy it. This is done MANY times (the Unroll pass constructs one such
// iterator per instruction-argument), so per-construction heap cost dominates.
//
// Build (libstdc++, matches walrus):  g++ -O2 -std=c++17 stack_deque_vs_vector.cpp -o s && ./s
//
// We instrument global operator new/delete to count bytes + allocation calls.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <stack>
#include <vector>
#include <deque>

// ---- heap instrumentation -------------------------------------------------
static std::size_t g_bytes = 0;     // total bytes requested this measurement
static std::size_t g_live  = 0;     // currently-live bytes
static std::size_t g_peak  = 0;     // peak live bytes
static std::size_t g_calls = 0;     // number of operator new calls
static bool        g_on    = false; // only count inside the measured region

// Track sizes so delete can decrement live bytes. Tiny fixed map (we never have
// many live allocations at once here).
struct Rec { void* p; std::size_t n; };
static Rec  g_recs[4096];
static int  g_nrecs = 0;

void* operator new(std::size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (g_on) {
    g_bytes += n; g_calls++; g_live += n;
    if (g_live > g_peak) g_peak = n + (g_live - n);
    if (g_live > g_peak) g_peak = g_live;
    if (g_nrecs < 4096) g_recs[g_nrecs++] = {p, n};
  }
  return p;
}
void operator delete(void* p) noexcept {
  if (g_on && p) {
    for (int i = 0; i < g_nrecs; ++i)
      if (g_recs[i].p == p) { g_live -= g_recs[i].n; g_recs[i] = g_recs[g_nrecs-1]; g_nrecs--; break; }
  }
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

static void reset() { g_bytes = g_live = g_peak = g_calls = 0; g_nrecs = 0; }

// ---- the modeled element: pelican::Expr::Ref is a refcounted pointer (8 B) --
using ExprRef = void*;   // stand-in: one pointer, like RefPtr<Expr>

// A minimal SmallVector-style backing: inline buffer of N, heap only if exceeded.
// std::stack needs back()/push_back()/pop_back()/empty()/size() + operator==.
template <class T, std::size_t N>
struct SmallVec {
  // typedefs required by std::stack's container interface
  using value_type      = T;
  using reference       = T&;
  using const_reference = const T&;
  using size_type       = std::size_t;
  alignas(T) unsigned char buf_[sizeof(T) * N];
  T*           heap_ = nullptr;
  std::size_t  cap_  = N;
  std::size_t  sz_   = 0;
  T* data() { return heap_ ? heap_ : reinterpret_cast<T*>(buf_); }
  void push_back(const T& v) {
    if (sz_ == cap_) {
      std::size_t nc = cap_ * 2;
      T* nh = static_cast<T*>(::operator new(nc * sizeof(T)));
      for (std::size_t i = 0; i < sz_; ++i) nh[i] = data()[i];
      if (heap_) ::operator delete(heap_);
      heap_ = nh; cap_ = nc;
    }
    data()[sz_++] = v;
  }
  void pop_back() { --sz_; }
  T&   back()     { return data()[sz_ - 1]; }
  bool empty() const { return sz_ == 0; }
  std::size_t size() const { return sz_; }
  ~SmallVec() { if (heap_) ::operator delete(heap_); }
  bool operator==(const SmallVec& o) const {
    if (sz_ != o.sz_) return false;
    for (std::size_t i = 0; i < sz_; ++i) if (const_cast<SmallVec*>(this)->data()[i] != const_cast<SmallVec&>(o).data()[i]) return false;
    return true;
  }
};

// One iterator lifetime: push `depth` refs (the DFS), then pop them all.
template <class Stack>
void one_iterator(int depth) {
  Stack s;
  for (int i = 0; i < depth; ++i) s.push(reinterpret_cast<ExprRef>(0x1000 + i));
  // dereference / equal would touch top; increment pops:
  while (!s.empty()) s.pop();
}

template <class Stack>
void measure(const char* label, int depth, int iters) {
  reset();
  g_on = true;
  for (int k = 0; k < iters; ++k) one_iterator<Stack>(depth);
  g_on = false;
  std::printf("%-34s depth=%d  iters=%d  | heap calls=%zu  total bytes=%zu  per-iter bytes=%.1f  per-iter allocs=%.2f\n",
              label, depth, iters, g_calls, g_bytes,
              (double)g_bytes / iters, (double)g_calls / iters);
}

int main() {
  std::printf("sizeof(ExprRef)=%zu, _GLIBCXX_DEQUE_BUF_SIZE-ish deque chunk applies on libstdc++\n\n",
              sizeof(ExprRef));
  const int ITERS = 100000;
  for (int depth : {1, 2, 3, 5}) {
    measure<std::stack<ExprRef>>                            ("std::stack<ExprRef> (DEQUE, default)", depth, ITERS);
    measure<std::stack<ExprRef, std::vector<ExprRef>>>      ("std::stack<ExprRef, std::vector>     ", depth, ITERS);
    measure<std::stack<ExprRef, SmallVec<ExprRef, 8>>>      ("std::stack<ExprRef, SmallVec<.,8>>   ", depth, ITERS);
    std::printf("\n");
  }
  return 0;
}
