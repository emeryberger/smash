# macOS arm64 / arm64e: interposition, benches, and the release

Apple Silicon has two ABIs: **arm64** (ordinary apps) and **arm64e** (pointer-authentication / PAC — Apple's own system binaries, and opt-in third-party builds). A dylib can only be `DYLD_INSERT_LIBRARIES`'d into a process whose ABI its Mach-O contains. This forces three separate architecture decisions in this repo, which are easy to conflate:

| thing | arch | why |
|---|---|---|
| `libsmash.dylib` (built + shipped) | **fat** arm64 + arm64e | must interpose into *both* kinds of process |
| benchmark binaries (`bench/`) | **arm64 only** | they are the app *under test*, and must load arm64-only preloads |
| release macOS assets | **both** (arm64 dylib + fat zip) | ship the drop-in *and* the arm64e-capable slice |

## libsmash must be fat

An arm64-only dylib fails to load into an arm64e process:

```
dyld: … incompatible architecture (have 'arm64', need 'arm64e')
```

Verified directly: a fat libsmash interposes into an arm64e app (compress + fault-decompress work, ~76% RSS reduction on `bench_rss`); an arm64-only libsmash aborts that same app at load. So the root `CMakeLists.txt` builds `arm64;arm64e` on Apple Silicon, and that is load-bearing — do not "simplify" it to arm64.

## Benchmarks must be arm64-only (NOT fat)

The benches are the *application under test*, and a fat executable runs its **arm64e** slice on Apple Silicon. An arm64e process then cannot load any **arm64-only** dylib — and everything preloaded into a bench is arm64-only:

- the **released** `libsmash-macos-arm64.dylib` (see the release section — the direct download is arm64-only),
- the comparison allocators (mimalloc / jemalloc / tcmalloc / Hoard) built by `allocators/`.

Making the benches fat therefore **broke every macOS allocator comparison** (they SIGKILL/abort at startup) and made it impossible to test the shipped arm64 libsmash against them. Fixed by setting `CMAKE_OSX_ARCHITECTURES=arm64` at **directory scope** in `bench/CMakeLists.txt` (commit `1189914`), which covers every bench target and flows into the ExternalProject deps so their static libs match the slice. Real apps are arm64, so an arm64 bench is also the honest model of one.

> Regression guard, learned the hard way: verify preloading by **exit code and program output**, not by grepping stderr. A dyld failure message *contains the dylib's name*, so `grep -c mimalloc` on a failed run returns non-zero and reads as success. The real check is "did the process run and print its result."

## The release ships arm64 + a fat zip

`release.yml` builds one fat dylib and emits two macOS assets:

- `libsmash-macos-arm64.dylib` — arm64 only, thinned (`lipo -thin arm64`) from the fat build so its arm64 slice is identical; the drop-in for ordinary arm64 apps.
- `libsmash-macos-fat.zip` — the fat arm64 + arm64e dylib, for arm64e apps.

Both are **re-signed ad-hoc** (`codesign --force --sign -`) after `lipo`/copy: `lipo` strips the signature, and **arm64e mandates a valid code signature to load**. The fat one is delivered **in a zip** because a raw release-download binary is not a reliable vehicle for the arm64e slice's signature; the zip keeps it intact.

## The exec-vs-load trap (why CI verifies on macos-26)

Two independent gates matter for the arm64e slice, and they were repeatedly confused:

1. **Can this machine *exec* a third-party arm64e binary at all?** — an OS-version property.
2. **Does the fat dylib *load* into such a process?** — the thing we actually want to verify.

**macOS 14/13/15 answer "no" to (1):** hosted runners on those images lack the `-arm64e_preview_abi` boot-arg, so *any* third-party arm64e process is **SIGKILLed (exit 137)** before `main`, no matter how it is signed. **macOS 26 (Tahoe) answers "yes"** with no boot-arg and no SIP change.

Consequences:
- An `exit 137` from an arm64e probe on macOS ≤ 15 means "this OS can't exec third-party arm64e," **not** "the signature/dylib is broken." Do not chase a signing bug for it.
- A fat dylib appearing to work on a dev machine may just mean that machine is on **macOS 26** (or has SIP disabled) — it is **not** evidence the *shipped* artifact loads for stock users. (This bit twice: a local success on macOS 26.5 was first misattributed to SIP being disabled, then a `macos-14` CI probe's SIGKILL was misread as "arm64e injection is fundamentally blocked." Both were the OS-version gate.)

So `release.yml` splits the macOS verification:
- **`build-macos` (`macos-14`)** — static checks only: arm64e slice present + `codesign -v`. Never execs arm64e.
- **`verify-macos-arm64e` (`macos-26`)** — downloads the shipped zip, unzips, builds a real arm64e probe, and preloads the dylib into it, asserting `arm64e probe output: [ok]`. It is **capability-gated**: it runs the bare probe first inside an `if`, so a runner that can't exec arm64e *skips* with a `::warning::` instead of false-failing. `publish` depends on this job, so the arm64e slice is proven end-to-end on every release.

The CI probe on macos-26 is the source of truth for "does the shipped arm64e artifact load," precisely because local dev machines give misleading answers.
