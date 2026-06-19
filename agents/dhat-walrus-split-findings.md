# DHAT object-split findings on neuron-cc walrus backend (PRELIMINARY)

> **⚠️ PRELIMINARY — small HLO.** These findings are from a DHAT run on the *tiny*
> `model_tiny_graph.hlo` (2.5 KB). The allocation *volumes* are therefore small and not
> representative of a real compile; the *patterns and owning classes* are the takeaway. A
> full-HLO DHAT run (`test7_fsdp…hlo`, 9.3 MB) was attempted to get at-scale numbers but is
> impractically slow under Valgrind (>6 h, one 28 GB walrus subgraph still instrumenting). Treat
> the reclaim-byte figures below as illustrative, not load-bearing; the structural finding (which
> class, which field-offset is cold, why) is what's actionable.

## What this is

Using Valgrind **DHAT** (not smash — DHAT is an external profiler) to find *object-splitting*
opportunities in the Neuron compiler: allocation sites whose per-byte access histogram shows a
hot region + a cold region within the same object, attributed to the owning C++ class. This is
the Chilihimbi "cache-conscious structure splitting" signal (PLDI'99), measured rather than
guessed. Method/context: see [agents/analysis-mode-design.md](analysis-mode-design.md) (the
on-hold in-smash equivalent) and [agents/neuron-cc-investigation.md](neuron-cc-investigation.md).

How to reproduce (on cloudnew, image `kaena-fixed:local` = valgrind + patched islpy):
```
# in the container, from the KaenaCompiler workspace:
DHAT_HISTOGRAM_MEMORY=4096 valgrind --tool=dhat --trace-children=yes --num-callers=30 \
  --dhat-out-file=OUT/dhat.%p.json \
  python3 -m neuronxcc.driver.CommandDriver compile --framework XLA <hlo> --target trn2 -o x.neff
```
Then identify the `walrus_driver` output file (its `cmd` ends `.../bin/walrus_driver`) and parse
it. `walrus_driver` / `libwalrus.so` are built `RelWithDebInfo` (NOT stripped) so DHAT resolves
full app backtraces; only the prebuilt `hlo2penguin` frontend is stripped (`???` frames — ignore
its file). DHAT `acc` histogram encoding: pairs `(−run_bytes, access_count)` summing to object
size. Parser used: `/tmp/dhat_owner.py` (dumps full backtrace per candidate).

## Finding #1 (dominant): `bir::QuasiAffineExprIterator`'s `std::stack` is ~88% cold

**Owning class / member:**
`neuronxcc/walrus/ir/include/bir/IR/QuasiAffineExprIterator.h:64`
```cpp
std::stack<pelican::Expr::Ref> stack;   // std::stack defaults to std::deque backing
```
(`pelican::Expr::Ref` = `pelican::RefPtr<pelican::Expr>` — matches DHAT's
`std::_Deque_base<pelican::RefPtr<pelican::Expr>>`.)

**Measured (tiny HLO):** 64-byte allocations, **~88% of bytes never accessed**; the top sites had
~480 blocks each (≈0.03 MB each — small because the HLO is tiny). Access heat:
`........########........` — only the middle/back ~8 bytes touched.

**Why cold:** `std::stack` defaults to `std::deque`, whose constructor *eagerly* allocates a
64-byte map-of-node-pointers (+a chunk). This iterator's DFS (`traverseChildren`,
`QuasiAffineExprIterator.h:22`) pushes only a handful of `Expr::Ref`s, so the 64-byte deque map
block is almost entirely unused.

**Why hot (high frequency):** a fresh `QuasiAffineExprIterator` is constructed on every
`bir::QuasiAffineExpr::hasOpaqueAffineOp()` (`QuasiAffineExpr.cpp:391-392`), called per-argument
by `bir::SymbolicAccessPattern::isDynamicOffsetAP()` (`SymbolicAccessPattern.cpp:149`) inside the
**`Unroll` pass's per-instruction loop** (`neuronxcc::backend::Unroll::genPhyAP → unroll_arg →
unrollInst`, `unroll.cpp`). So this is an **allocation-churn** hot spot (very high construct/destroy
rate of short-lived iterators), not a steady-RSS one. The aggregate cost at real scale is what the
(too-slow) full-HLO run would quantify.

**Proposed fix (one line, container swap):** give `std::stack` a vector backing so it does not
allocate the deque map block:
```cpp
// QuasiAffineExprIterator.h:64
std::stack<pelican::Expr::Ref, llvm::SmallVector<pelican::Expr::Ref, 8>> stack;  // inline buffer → 0 heap for shallow trees
// or, without the llvm dependency:
std::stack<pelican::Expr::Ref, std::vector<pelican::Expr::Ref>> stack;
```
`std::stack` needs `back/push_back/pop_back/empty/size` from its container (vector & SmallVector
both provide). `equal()` (`:60`) does `stack == other.stack`; `std::stack::operator==` works for
any equality-comparable backing container, so it compiles unchanged. `SmallVector<…,8>` is
preferred: shallow traversal depth ⇒ zero heap allocation in the common case vs. the deque's
mandatory map+chunk every construction.

**Note:** the header itself (lines ~70+) documents a visitor-based walk "significantly more
efficient than the iterator above by avoiding multiple atomic inc_ref/dec_ref per node" — i.e. the
codebase already knows this iterator is a hot-path inefficiency. The container swap is a cheap
complement to (or stopgap for) migrating callers to that visitor.

## Other candidates seen (tiny HLO, lower volume — verify at scale)

- `boost::log::v2s_mt_posix::attribute_value_set` (~840 B): hot prefix + cold tail
  (`########################.#..............`) — a structure-split candidate, but boost-owned
  (not walrus source), so the lever is "log less / construct fewer records," not a struct edit.
- `icu_67::DateFormatSymbols::assignArray` (~136 B): bimodal access — incidental (ICU date
  formatting), low value.
- `llvm::DenseMap<bir::Instruction*, bir::Instruction*>`, `<bir::BasicBlock*, unsigned>`,
  `<bir::BasicBlock const*, unsigned long>` (~1024 B): **fully accessed, NOT split candidates** —
  listed only to show the method correctly distinguishes hot-dense maps from cold-sparse deques.

## Status / next step

- The OOB fix (PR #38, merged) and these DHAT findings are **independent** — DHAT never loads
  libsmash.
- To turn the QuasiAffineExprIterator finding into a confident, quantified recommendation, get
  at-scale numbers from either: (a) the full-HLO DHAT run when it finishes, or (b) a faster
  `walrus_driver`-standalone replay on a real-but-bounded subgraph (skips the slow Tensorizer
  frontend). The structural fix is already clear regardless of the magnitude.
