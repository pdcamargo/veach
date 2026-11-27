# SP-02: dmath bake-off (VCH-078)

Picks the implementation behind `veach::dmath`, the correctly rounded (CR) math that canonical code must use
([ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md) rule 3). A CR function returns the
representable value nearest the exact result, so any two CR implementations agree bit for bit.

## Decision

**LLVM libc's header-only math (`libc/shared/math`), pinned to commit `e6091a8` on main (2026-11-27), compiled without
fused multiply-add (FMA) on every platform.** Three small wrapper rules fix the cases where the pinned code isn't CR. The
result:

- **Correctly rounded:** sin, cos, tan, asin, acos, exp, exp2, log, log2, log10, pow, cbrt, hypot, sinh, cosh, tanh in
  double, and all 18 functions in float.
- **Not correctly rounded: double atan and atan2.** They are faithfully rounded, deterministic, and part of dmath
  `version` 1. No candidate gives CR atan2 that builds with MSVC today. This is an exception to ADR 0043 rule 3, recorded in
  [ADR 0067](../../adr/0067-dmath-on-llvm-libc.md).
- **Bit-equal across platforms by construction.** The Mac runs the same code path MSVC x64 compiles, so equal bits rest
  only on IEEE `+ − × ÷ √` under the SP-01 flags, not on the CR claim.

## Candidates

| | CORE-MATH | LLVM libc 23.1.6 (latest release) | LLVM libc main `e6091a8` | Own polynomials (+ − × ÷ √ only) |
|---|---|---|---|---|
| License | MIT | Apache-2.0 WITH LLVM-exception | same | ours |
| Form | C, one `.c` per function plus `dint.h`/`qint.h`/`tint.h` | C++ headers | C++ headers | — |
| MSVC | **No without a fork**: `unsigned __int128` in sin, cos, tan, atan2, log, log10, pow, hypot (MSVC has no 128-bit integer); GCC builtins everywhere (`__builtin_fma`, `__builtin_expect`, ...); x86 inline asm under GCC guards | Yes: `LIBC_COMPILER_IS_MSVC` paths, `UInt<128>` fallback; LLVM's own `APFloat.cpp` includes `shared/math.h` and LLVM builds with MSVC | Yes, same | Yes |
| CR (double) | Yes, but atan2 at `b464d7d` calls `exit()` on 26 of its own worst cases (e.g. `atan2(0x1.8000000000003p-51, 0x1.0000000000003p+0)`) | **No** for pow, atan, atan2; no double sinh/cosh/tanh | Yes except atan, atan2 | No |
| pow | CR | **Wrong**: e.g. `pow(0x1.27490e3bf5ce2p+158, -0x1.da04ff576145ep+921)` returns `-0x1.91aee60d9aa24p-815` instead of `+0`; for \|y\| ≥ 2^45 it returns wrong values and signs (18 % of our inputs) | CR (PR #222827, after the 23.x branch) | — |
| Pinning | No releases; commit archive 197 MB | Release tarball 179 MB (whole monorepo) | Commit archive 330 MB | — |
| Kept | — | — | 2,828 files (26 MB), compiled into one TU (`engine/core/src/dmath.cpp`) | — |

"Own polynomials" was measured as LLVM's fast pass with the accurate pass compiled out (`LIBC_MATH_SKIP_ACCURATE_PASS`,
no FMA). That is exactly a fixed polynomial in basic operations. Against CR on the same no-FMA path, it is no faster for
exp (3.1 ns either way) and at most 12 % faster for log and pow (pow 10.9 vs 12.3 ns), the functions rule 3's fallback
was meant for. It is about 2× faster only for tan, asin and acos, and it differs from CR on up to 35 % of inputs
(asin). **Rule 3's polynomial fallback is not needed.**

## The finding that shaped the design: FMA and no-FMA paths differ

LLVM libc compiles different code when the target has FMA (`__ARM_FEATURE_FMA`, every arm64 Mac) and when it doesn't
(MSVC x64, which never defines `__FMA__`). Both paths are meant to be CR, but on main and 23.1.6 the no-FMA path gets
**sin and tan wrong for |x| < 2^-960**: it rounds `x - x·2^-54` twice, and when `x·2^-54` is subnormal the sum can land
on an exact tie. That is 261 and 254 per 10^6 of our inputs. Shipped as-is, the Mac and Windows would disagree on
those inputs.

So `dmath.cpp` includes `cpu_features.h` first and `#undef`s the four `LIBC_TARGET_CPU_HAS_FMA*` macros. Every platform
then runs the no-FMA code, an `#error` guards it, and three exact rules patch what is left:

- **sin, tan:** return x for |x| < 2^-26 (sin) and 2^-27 (tan), where x itself is the CR result.
- **atan2:** a zero result takes the sign of y. LLVM returns `+0` for `atan2(-tiny, +huge)`, and that was all 14,074
  of its random-input misses.
- **atan:** computed as `atan2(x, 1)`. LLVM's own atan is 1 ulp; atan2(x, 1) has no misses on 10^7 random inputs and
  slightly fewer on worst cases (8,635 vs 8,985 of 55,770).

## Verification

All of this ran on an Apple M5 Max (Apple Clang 21). "x86-64" means the same source built `-arch x86_64` and run under
Rosetta 2: SSE2, no FMA, the feature set MSVC x64 compiles for. The reference is CORE-MATH `b464d7d` (binary64 and
binary32).

| Check | Result |
|---|---|
| 10^7 inputs per function (edges, typical range, any finite, raw bits), arm64 vs CORE-MATH | **0 mismatches** in all 36 functions |
| CORE-MATH's worst-case lists (hardest known inputs), 16 CR double functions, arm64 | **0 / 7,636,841** |
| Same, atan | 8,635 / 55,770 (faithful, see above) |
| Same, atan2 | 5,741 / 29,115, every miss one ulp (CORE-MATH itself called `exit()` on 26 more) |
| arm64 vs x86-64 hashes: 10^7 inputs (36 functions) and all worst cases (18 functions) | **equal** |
| Debug (`-O0`) vs Release (`-O3`), Apple Clang | equal (unit test and `veach-bench-dmath`) |
| Windows x64, MSVC, Debug | **pending CI**: `[adr0043]` unit tests (10^5 inputs per function, 252 hard cases) and the `full`-labeled ctest over 10^7 |

LLVM libc 23.1.6 and main, by path, vs CORE-MATH over 10^6 inputs (misses per function, 0 omitted):

| | FMA path (Mac as shipped) | no-FMA path (MSVC as shipped) |
|---|---|---|
| 23.1.6 | atan 7,932, atan2 14,074, pow 181,201 | sin 261, tan 254, atan 313, atan2 14,074, pow 181,252 |
| main `e6091a8` | atan 7,932, atan2 14,074 | sin 261, tan 254, atan 313, atan2 14,074 |
| dmath (main, no FMA, wrapper rules) | — | none |

### Hashes (FNV-1a over result bits, NaN canonicalized; inputs from `tests/support/dmath_vectors.hpp`)

| Function | 10^5, double | 10^5, float | 10^7, double | 10^7, float |
|---|---|---|---|---|
| sin | `bb84e4600234a8f7` | `9e2d62720369c318` | `bc8c78a3dd2abf00` | `4166eb69819224b6` |
| cos | `268ceb6086e40d8a` | `b1d4e6400616c543` | `7ca706dfa90f8880` | `c526b87df16bc42d` |
| tan | `11b20f7aa42dcf13` | `c33ce235ecb65a32` | `2d94b75dd05e0ccd` | `ed437ab4a46d8730` |
| asin | `88026bb91c82f4a4` | `bd4a5442f866d647` | `63445983dc1428b4` | `4af712fdbd7e7a6a` |
| acos | `bedd14cce496bf38` | `a449238ec3a5ae2b` | `4cd600ac087f9df6` | `f45f0866435c67ca` |
| atan | `595ff7ee6461f5ea` | `ba4de38383df9d35` | `9af445bd9e883859` | `7b0bd074d78e53bb` |
| atan2 | `96df6d2c68fdebca` | `8feb5f1f6e95a0c2` | `d1e73afc6dd2fb7e` | `2a785cf27e8f5f14` |
| exp | `bcb3ae89af47a0d5` | `34bb52d2db6d0c4b` | `cd9b14832ccdd7f2` | `341dbee74ff7399e` |
| exp2 | `1f78b48a8024f433` | `a5153d2c39c33f6b` | `2fc256ab204d6223` | `9467f1486b091784` |
| log | `0de4b1fd1d118646` | `1918570a521479e4` | `1e25cd6d4501c672` | `50f84bed7cc10a71` |
| log2 | `fc7a714c141b70d8` | `0feaedd0de77a108` | `b86772a8088bb4fd` | `ab7a9c5a617f261e` |
| log10 | `4b1843a34d85e84d` | `dc89fc4e2b08ecdf` | `214af709f6394293` | `077ef44f3c4c3fe4` |
| pow | `6d19454c45c1ae44` | `a09e6803863900fd` | `68a167a9976fd85b` | `bb23deaade335892` |
| cbrt | `da2e815c2b7ea5c8` | `9a31b3cccc7ad4ed` | `1e120f3dd371de88` | `5d59782218d765c8` |
| hypot | `f8931d24e0c31650` | `c126e4a854fa894f` | `e9c1ed97fea01569` | `d52a2b6a9c30f38b` |
| sinh | `76d8ca01a2397216` | `5750397128d80523` | `8ac4c647e6805538` | `a4e441c4fe9f0399` |
| cosh | `f9b6c33a07ac6ed3` | `7f00c58d9b2e40c9` | `0e8179a41898b054` | `143845d5a2a02675` |
| tanh | `7713168546d2b0d2` | `386c71d7589f8dfa` | `40b0c9aeac7e8a97` | `2e1aef44bd4f3938` |

NaN results hash as one value. NaN sign and payload are not portable (x86's default NaN is negative, Arm's
positive, even for `0.0 / 0.0`), so stage hashes must canonicalize NaN too.

## ns per call (Apple M5 Max, macOS arm64)

Throughput over 10^7 uniform inputs in each function's typical range (sin/cos/tan in [-8, 8], log in (0, 100], pow
x in (0, 4] and y in [-30, 30], ...), each call through a function pointer, best of 3. A second run was within 5 %. dmath and libm come from
`build/release/tests/veach-bench-dmath` (`release` preset, `-O3`). The other columns come from the spike harness
(`-O2`, same strict flags).

| Function | **dmath** (shipped: main, no FMA) | Apple libm | LLVM main, FMA path | CORE-MATH | Fixed polynomial (LLVM fast pass, no FMA) | dmath float | libm float |
|---|---|---|---|---|---|---|---|
| sin | 7.4 | 5.7 | 3.5 | 3.2 | 6.1 | 2.5 | 5.3 |
| cos | 7.3 | 5.7 | 3.6 | 3.3 | 6.1 | 2.8 | 5.6 |
| tan | 10.0 | 3.1 | 4.3 | 13.0 | 4.8 | 2.7 | 5.3 |
| asin | 8.8 | 5.7 | 5.5 | 5.2 | 4.3 | 5.2 | 4.7 |
| acos | 9.4 | 7.1 | 6.2 | 5.2 | 4.5 | 5.7 | 6.2 |
| atan | 8.6 | 6.4 | 3.3 | 3.5 | 5.4 | 4.2 | 2.2 |
| atan2 | 8.5 | 18.2 | 4.3 | 4.6 | 7.9 | 3.0 | 7.5 |
| exp | 3.2 | 4.8 | 2.6 | 2.7 | 3.1 | 1.9 | 1.2 |
| exp2 | 2.9 | 1.7 | 2.4 | 2.2 | 3.6 | 2.4 | 1.2 |
| log | 2.5 | 2.0 | 1.9 | 2.8 | 2.1 | 2.4 | 1.7 |
| log2 | 3.2 | 2.0 | 2.1 | 2.8 | 2.7 | 1.7 | 1.7 |
| log10 | 3.3 | 2.0 | 2.3 | 3.1 | 2.8 | 2.6 | 1.7 |
| pow | 12.9 | 7.6 | 5.7 | 10.9 | 10.9 | 4.2 | 2.4 |
| cbrt | 6.8 | 1.7 | 3.6 | 5.9 | 6.3 | 2.2 | 2.1 |
| hypot | 3.8 | 1.2 | 1.9 | 6.7 | 3.3 | 2.0 | 1.0 |
| sinh | 4.2 | 2.7 | 3.6 | 6.3 | 2.6 | 2.0 | 2.7 |
| cosh | 3.8 | 2.6 | 3.3 | 6.0 | 2.4 | 2.3 | 2.8 |
| tanh | 5.0 | 4.0 | 3.6 | 5.1 | 3.2 | 2.2 | 2.7 |

The single no-FMA path costs up to 2× on the Mac (sin 3.5 → 7.4 ns, pow 5.7 → 12.9 ns). Everything stays under 13 ns.
At 10 M density samples per core per second (VCH-082's target), a sample affords 100 ns in all, so a handful of pow or
exp calls fit but are not free.

## Why this one

- CORE-MATH is the best CR library, and it is our reference. On MSVC it is a fork: eight of the needed functions do
  128-bit integer arithmetic, and the code calls `exit()` when its rounding test fails.
- LLVM libc builds with MSVC because LLVM itself does, it is header-only, and its main branch is CR for everything we
  need except atan and atan2.
- A single code path makes cross-platform equality independent of any one CR bug. The tiny-sin bug above is exactly
  the kind of defect that path-dependent equality would have shipped.

## What would reopen it

- **Windows CI disagrees** with any committed hash: MSVC compiled a branch Clang doesn't take. Bisect with the unit
  test, then patch or pin around it.
- **dmath shows up in a Tracy profile of a canonical hot path** (terrain density, SP-06/SP-08). Options are the FMA path
  on both platforms (MSVC needs `/arch:AVX2` plus a patch for `__builtin_fma`, which sets a minimum CPU: a product
  decision), or float dmath where the precision allows.
- **CR atan/atan2 becomes available** that builds with MSVC (upstream LLVM or a CORE-MATH port): swap it in and bump
  `dmath::version`.
- Apple libm and the MSVC CRT both ship CR math by default (ADR 0043's reopen condition).

## Follow-ups

- **ADR:** written as [0067](../../adr/0067-dmath-on-llvm-libc.md).
- **Backlog VCH-104: CR atan and atan2**, before the determinism contract is frozen by the first saved universe. Options:
  - CORE-MATH atan (no `__int128`, needs a builtin shim on MSVC) plus atan2 ported to a portable 128-bit type.
  - Wait for upstream.
  - Our own Ziv-style atan2 (fast double-double pass, rounding test, a correctly rounded slow pass) using LLVM's
    double-double helpers.
- **Backlog VCH-105: report upstream to LLVM** The no-FMA sin/tan tiny-argument bug and the atan2 signed-zero bug, with the
  inputs above.
- **Backlog VCH-105: move the pin to LLVM 24.1** when it ships, the first release with CR pow. Rerun the CORE-MATH comparison.
  CR functions must keep their hashes; atan/atan2 may change, which is a `version` bump.
- **ADR 0043 fix list:** the `core.fp_nondeterministic` startup self-test can now compare a few dmath values. SP-07
  rebinds Luau's `math` to dmath.
- **Release on Windows** is still unverified: CI builds Debug only. The Mac shows Debug equal to Release.
- **CI time:** the `full`-labeled 10^7 ctest takes 60 s in a Mac Debug build. CI runs it on Windows and skips it
  under ASan and leak checks.
- `tools/scripts/wincheck.sh` now sees the LLVM headers but takes their Clang branches (clang-cl defines `__clang__`).
  Only CI proves the MSVC branches.

## How it was measured

- **Scratch harness, not committed.** It linked these four, with the same strict flags, arm64 and `-arch x86_64`:
  - CORE-MATH `b464d7d` binary64 and binary32 sources.
  - LLVM libc 23.1.6 (`llvm-project-23.1.6.src.tar.xz`, SHA256 `d3b86ab2…`).
  - LLVM main `e6091a8` (SHA256 `fe900189…`), in FMA, no-FMA and fast-pass variants.
  - The repo's `engine/core/src/dmath.cpp`.
- It compared them on the shared input set and on CORE-MATH's `.wc` worst-case files.
- **To reproduce the dmath side:** `veach-bench-dmath` (timings and 10^7 hashes, exit 1 on mismatch) and
  `veach-tests -tc="*adr0043*"`.

## Decisions (dev lead)

Engineering decisions, recorded in [ADR 0067](../../adr/0067-dmath-on-llvm-libc.md):

1. **LLVM libc at a pinned main commit, one no-FMA path on every platform** (option A). It is the only candidate that
   builds with MSVC and gives the same bits on arm64 and x86-64 today. CORE-MATH needs a fork for MSVC; LLVM 23.1.6's
   pow is wrong for large exponents; our own polynomials are never correctly rounded and save at most 12 %.
2. **Double atan and atan2 stay faithfully rounded (within one ulp, same bits everywhere) for now.** Their bits become
   a commitment only when the first universe is saved, so correctly rounded versions block freezing the determinism
   contract (VCH-104), not SP-04, which is a throwaway spike.
3. **Download the pinned archive and keep only the headers** (26 MB in the dependency cache CI already caches) rather
   than committing them: it follows ADR 0002's pinning rule (exact version, archive URL, SHA256).
