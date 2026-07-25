# CLOUDNEW_SMASH_PLAN.md — re-measure RocksDB on cloudnew at the new pin

**Goal.** The RocksDB pin moved from 9.8.4 → **11.1.2** (current release). Re-run
the paper's RocksDB experiments on **cloudnew** (EPYC 9R14 — the machine the
paper's full datasets came from) at the new pin, and update *both* families of
RocksDB numbers in the paper.

Written 2026-07-25. Everything below except the cloudnew run itself is already
done and committed to the working tree.

---

## 0. Status — what is already done

| Item | State |
|---|---|
| `DB::Open` port (`#if ROCKSDB_MAJOR >= 10`) in `bench/bench_rocksdb.cpp`, `bench/bench_heap_compress.cpp` | **done**, builds+runs on 9.8.4 and 11.1.2, macOS/Clang and Linux/GCC |
| `SMASH_ROCKSDB_VERSION` pin → `11.1.2` (`bench/CMakeLists.txt`) | **done** |
| `SMASH_ROCKSDB_ROOT=<prefix>` escape hatch + static-archive lz4/zstd link fix | **done** |
| Version A/B on EC2 m5.2xlarge (9.8.4 vs 11.1.2) | **done** — see §5 |
| Re-measure on cloudnew + update paper macros | **THIS PLAN** |

Two facts that shape the plan:

* `DB::Open`'s `DB**` overload was removed in **10.0.1** (not 11) — 9.8.4 is the
  last version that compiles the old call shape, so any move forward at all
  required the port.
* On EC2 the version made **no measurable difference** (82.6 % vs 82.4 % cool-phase
  RSS reduction; gap smaller than run-to-run spread). Expect cloudnew to confirm,
  not overturn, this. If cloudnew shows a large version gap, that is a *finding*,
  not a routine result — investigate before publishing.

---

## 1. Unknowns to resolve first

1. **cloudnew access.** It does not resolve in DNS from Emery's laptop, there is
   no `~/.ssh/config`, and it is not in the smash-bench AWS account
   (`~/amazon-aws-credentials.sh`, account 994919950049 — 118 instances, all
   `m5`/`c5`, none named cloudnew). Need host/IP + SSH user + key, or the
   account/VPN/jump-host route. Referenced in
   `agents/dhat-walrus-split-findings.md` and `smash-paper/paper/figures/plot_all.py:436`
   ("cloudnew EPYC 9R14").
2. **The cool-phase solo harness is not in the smash repo.** `rss_fresh_macros.tex`
   says it was "Generated 2026-07-23 from `smash_work/tmp/coolphase_results.json`".
   No script in `bench/` or `scripts/` references `smaps_rollup`, `Private_Dirty`,
   `coolphase`, or `solo_avgrss`. Look for `smash_work/` **on cloudnew** — that is
   the most likely home. If it cannot be found, §4b must be reconstructed from the
   methodology in the `rss_fresh_macros.tex` header (see §4b).

---

## 2. Connect and sync

```bash
CLOUDNEW=<user>@<host>            # fill in from §1
cd ~/git/smash
git ls-files > /tmp/smash_files.txt
rsync -az -e "ssh -i <key>" --files-from=/tmp/smash_files.txt ./ $CLOUDNEW:~/smash-rdb11/
```

**Use `--files-from=$(git ls-files)`.** A plain `rsync ./` drags in the untracked
local `model/` directory (4.0 GB) and can fill the box — it took the EC2 box to
100 % disk mid-build. Tracked-files-only is 4.5 MB.

Check free disk before starting (`df -h ~`): the RocksDB build tree needs a few GB.

---

## 3. Build RocksDB 11.1.2

Flags mirror the `rocksdb_build` ExternalProject in `bench/CMakeLists.txt` so the
prefix is equivalent to what `-DSMASH_BUILD_BENCH_DEPS=ON` would produce.

```bash
git clone --depth 1 --branch v11.1.2 https://github.com/facebook/rocksdb.git ~/rdb/src
cmake -S ~/rdb/src -B ~/rdb/src/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_TESTS=OFF -DWITH_BENCHMARK_TOOLS=OFF -DWITH_TOOLS=OFF -DWITH_GFLAGS=OFF \
  -DWITH_LZ4=ON -DWITH_ZSTD=ON -DROCKSDB_BUILD_SHARED=OFF \
  -DCMAKE_INSTALL_PREFIX=$HOME/rdb/11.1.2 -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build ~/rdb/src/build -j$(nproc) && cmake --install ~/rdb/src/build
rm -rf ~/rdb/src        # reclaim disk
```

Needs `liblz4-dev libzstd-dev` (`sudo apt-get install -y liblz4-dev libzstd-dev`).
~4 min on 8 vCPU; faster on the EPYC. Expect a ~38 MB `librocksdb.a`.

**Optional A/B arm:** repeat with `--branch v9.8.4` → `$HOME/rdb/9.8.4` if you want
to reproduce the version comparison on EPYC as well as EC2. Not required for the
paper update.

---

## 4. Build smash and run BOTH experiments

```bash
cmake -S ~/smash-rdb11 -B ~/build-rdb11 \
  -DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=OFF -DSMASH_BUILD_BENCH_ALLOCATORS=OFF \
  -DSMASH_ROCKSDB_ROOT=$HOME/rdb/11.1.2
cmake --build ~/build-rdb11 -j$(nproc) \
      --target smash smash_compress_only bench_rocksdb
```

Expect `-- RocksDB 11.1.2 found (matches pin)`. A *warning* about being newer than
the pin means the pin edit did not sync — re-check `bench/CMakeLists.txt:581`.

**`smash_compress_only` is required** — the compress-only experiment aborts
immediately without `libsmash_compress_only.so`. This cost a wasted launch on EC2.
`run_paper_experiments.py` also rebuilds `smash_noopt` and `bench_sqlite` itself.

### 4a. Compress-only experiment → feeds `\coRocksdb*`

```bash
cd ~/build-rdb11
python3 ~/smash-rdb11/bench/run_paper_experiments.py \
  --apps rocksdb --compress-only-only --runs 3 \
  --build-dir ~/build-rdb11 --output-dir ~/rdb/results-11.1.2
```

~8.5 min. Produces `compress_only_results.json` with arms
`rocksdb_{baseline,jemalloc,mimalloc,compress_only,full_smash}`, plus a
`paper_tables.txt` LaTeX fragment.

Do **not** use `--quick` — that switches `bench_rocksdb` to a 200 k-key workload;
the paper's numbers are the no-args defaults (500 k keys, 512 B values, cool 10 s,
serve 20 s).

### 4b. Cool-phase heap-footprint solo runs → feeds `\rssRocksdb*`

These are the **headline** numbers (abstract, intro, evaluation Table 1) and come
from a *different* harness than 4a. Per the `rss_fresh_macros.tex` header:

> Cool-phase HEAP FOOTPRINT (Anonymous+Private_Dirty via smaps_rollup), solo,
> N=3 medians, AUC/duration (MiB). Generated 2026-07-23 from
> `smash_work/tmp/coolphase_results.json`. Metric = the anonymous, non-reclaimable
> memory an allocator governs (excludes reclaimable clean file pages).
> Overall = whole-run avg; Cool = idle-window avg.

Steps:
1. Find the generator on cloudnew: `ls ~/smash_work/`, then
   `grep -rl "smaps_rollup\|coolphase" ~/smash_work/ ~/ 2>/dev/null`.
2. Re-run it for **rocksdb only**, 4 allocator arms (glibc, jemalloc, mimalloc,
   smash), solo (one allocator per fresh process), N=3, same idle-window
   definition. Keep every other knob identical to the 2026-07-23 run.
3. If the harness is gone, reconstruct: run `bench_rocksdb` under each allocator,
   sample `/proc/<pid>/smaps_rollup` (sum `Anonymous` + `Private_Dirty`) once per
   second, report AUC/duration for the whole run ("Overall") and for the idle
   window after the fill phase ("Cool"). Reduction = `1 − smash/glibc`.
   **Flag clearly in the commit message that the harness was reconstructed** —
   the numbers are then not strictly comparable to the 2026-07-23 run.

---

## 5. Reference values (EC2 m5.2xlarge, 2026-07-25, medians of 3)

Sanity-check cloudnew output against these. Absolute RSS should be close (the
workload is dataset-bound, ~276 MB baseline); reduction % may differ by hardware.

| arm | RocksDB 9.8.4 | RocksDB 11.1.2 |
|---|---|---|
| baseline cool RSS | 274.1 MB | 276.0 MB |
| compress-only | 276.8 MB | 274.9 MB |
| full smash cool RSS | 50.2 MB | 50.8 MB |
| full smash reduction | 82.6 % (83.0/82.5/82.6) | 82.4 % (82.4/82.7/82.4) |
| peak RSS | 289.2 MB | 289.3 MB |

Current paper values for comparison: `\coRocksdbFullSmashRssLin{66}`,
`\coRocksdbFullSmashRssRedLin{79.7}`, `\coRocksdbFullSmashReductionLin{76}`,
`\coRocksdbCompOnlyRssLin{299}` (+8 % overhead), `\rssRocksdbSmashCool{69}`,
`\rssRocksdbCoolRed{74}`.

**Known anomaly to watch:** the paper's compress-only arm shows 298.8 MB
(+8 % over baseline), but both EC2 runs showed ~275 MB — *no* overhead — on both
RocksDB versions. So `\coRocksdbCompOnlyOverheadLin{8}` may no longer hold, and
this is unrelated to the RocksDB version. If cloudnew reproduces ~275 MB, that
macro and the sentence citing it in `evaluation.tex` need revisiting.

---

## 6. Update the paper (`~/git/smash-paper`, branch `main`)

Two macro families, two different update mechanisms. **Work on a branch.**

### 6a. `\coRocksdb*` — generated, do NOT hand-edit
`paper/results_macros.tex` is written wholesale by `paper/gen_macros.py` from
`paper_results/{linux,macos}/compress_only_results.json`.

```bash
cd ~/git/smash-paper
# Merge ONLY the rocksdb_* keys — the file also holds sqlite/redis/memcached arms
python3 - <<'EOF'
import json, pathlib
tgt = pathlib.Path("paper_results/linux/compress_only_results.json")
old = json.loads(tgt.read_text())
new = json.loads(pathlib.Path("/path/to/results-11.1.2/compress_only_results.json").read_text())
for k, v in new.items():
    if k.startswith("rocksdb_"):
        old[k] = v
tgt.write_text(json.dumps(old, indent=2))
EOF
cd paper && python3 gen_macros.py
git diff --stat paper/results_macros.tex     # expect ONLY coRocksdb*/related to move
```

**Never copy the whole JSON over** — the run only contains `rocksdb_*` arms and
would wipe every other app's results.

### 6b. `\rssRocksdb*` — hand-maintained
Edit `paper/rss_fresh_macros.tex` directly: `\rssRocksdb{Glibc,Jemalloc,Mimalloc,Smash}{Overall,Cool}`,
`\rssRocksdbOverallRed`, `\rssRocksdbCoolRed`. Update the header comment block
(date, source JSON path, smash/alloc8 commit hashes) — it is the only provenance
record for these numbers.

### 6c. Version strings
* `paper/evaluation.tex:41` — "RocksDB~9.8.4" → the version actually measured.
* `paper_results/linux/coolphase_heap_footprint_results.json` `app_versions`
  field — contains `rocksdb v9.8.4`.
* Grep for other stale mentions: `grep -rn "9\.8\.4" ~/git/smash-paper`.

### 6d. Rebuild and check prose
```bash
cd ~/git/smash-paper/paper && latexmk -pdf paper.tex   # or the repo's usual build
```
Numbers appear in prose as well as tables — check `evaluation.tex` (RocksDB
paragraph ~:197, cool-phase list ~:153, ~:348) and `introduction.tex:85` still
read correctly, and that the RSS-reduction *range* macros
(`\rssCoolRedMin`/`\rssCoolRedMax`) still bracket the new value.

---

## 7. Gotchas (all hit for real on 2026-07-25)

* `rsync` without `--files-from=$(git ls-files)` → 4 GB `model/` dir → disk 100 %.
* Forgetting `--target smash_compress_only` → compress-only run aborts instantly.
* `LD_PRELOAD=… timeout … ./bench` hangs `timeout` itself — use
  `timeout … env LD_PRELOAD=…` (see memory `project_preload_timeout_deadlock`).
* Reverting to the old pin for a paper-reproduction run is
  `-DSMASH_ROCKSDB_VERSION=9.8.4` — it retargets the from-source `GIT_TAG` *and*
  the system-install acceptance check together.
* The system-RocksDB path accepts pin-or-newer; **newer emits a `message(WARNING)`**
  about provenance. Exact-match prints a plain STATUS. If you see the warning
  during a paper run, stop and check which RocksDB you actually linked.
* A static `librocksdb.a` needs `lz4_static`/`libzstd_static` linked (handled
  automatically by `bench/CMakeLists.txt` when `ROCKSDB_LIB` ends in `.a`; the
  Homebrew *dylib* carries them internally, which is why macOS never hit this).

---

## 8. Definition of done

- [ ] cloudnew builds RocksDB 11.1.2 and smash with `matches pin` in the configure log
- [ ] 4a run complete, 3 runs, medians recorded
- [ ] 4b run complete (or reconstruction explicitly flagged), 3 runs, 4 allocators
- [ ] `results_macros.tex` regenerated; diff touches only RocksDB macros
- [ ] `rss_fresh_macros.tex` RocksDB block + provenance header updated
- [ ] version strings updated (§6c); `grep -rn "9\.8\.4"` clean in smash-paper
- [ ] paper rebuilds; prose ranges still consistent
- [ ] compress-only overhead anomaly (§5) either reproduced or explained
