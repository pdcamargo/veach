# 0067: veach::dmath on LLVM libc's header-only math, one no-FMA path

Date: 2026-11-27

## Status

Accepted. Implements rule 3 of [0043](0043-determinism-tiers-and-strict-floating-point.md), with one exception (double
atan and atan2), and extends the dependency set of [0017](0017-dependency-set.md). Validated by SP-02 (VCH-078,
[findings](../research/spikes/sp-02-dmath.md)).

## Context

Canonical code (seeds, orbits, terrain density at collider resolution, edits, generator hooks) must give the same bits
on macOS arm64 and Windows x64 ([0043](0043-determinism-tiers-and-strict-floating-point.md)). SP-01 fixed the compiler
flags; transcendentals still came from each platform's libm. ADR 0043 asked for correctly rounded (CR) functions,
because any two CR implementations agree bit for bit, so the backing library can change without changing results.

SP-02 compared CORE-MATH, LLVM libc (release 23.1.6 and a main commit) and our own polynomials on 10^7 inputs per
function and on 7.7 million known hardest inputs. CORE-MATH needs 128-bit integers MSVC lacks in eight functions, and
its atan2 calls `exit()` on some inputs. LLVM 23.1.6's pow is wrong for large exponents. Our own polynomials are at most
12 % faster for exp, log and pow and are never CR. LLVM libc's main branch builds with MSVC (LLVM's own `APFloat.cpp`
uses these headers) but picks FMA code on arm64 and non-FMA code on MSVC, and the two differ: the non-FMA path gets sin
and tan wrong for |x| < 2^-960.

## Decision

- **`veach::dmath` is backed by LLVM libc's header-only math** (`libc/shared/math`) at a pinned main commit
  (`e6091a8158c863643d4fbe44b70fe62dd0ef3eba`). The API is `engine/core/include/veach/core/dmath.hpp`:
  `sin cos tan asin acos atan atan2 exp exp2 log log2 log10 pow cbrt hypot sinh cosh tanh`, double and float, plus a
  `version` constant that canonical stage hashes include.
- **One code path everywhere: no FMA.** The headers are compiled with FMA off on every platform, so the Mac runs the
  code MSVC x64 compiles. Equal bits then rest only on IEEE `+ − × ÷ √` under the SP-01 flags.
- **Three wrapper rules** in `engine/core/src/dmath.cpp` correct the pinned code: sin and tan return x exactly for tiny
  x, atan2 keeps the sign of a zero result, and atan is computed as atan2(x, 1).
- **Exception to 0043 rule 3: double atan and atan2 are faithfully rounded** (within one ulp of the true value), not CR.
  They are deterministic and part of dmath `version` 1. Correctly rounded versions (VCH-104) must land before the
  determinism contract is frozen by the first saved universe; until then a change only bumps `version`.
- **Fetching:** `veach_fetch_archive_subset` in `cmake/VeachDependencies.cmake` downloads the pinned GitHub archive,
  checks its SHA256, keeps only the libc headers it needs (26 MB) in the dependency cache, and deletes the archive. CPM
  would unpack the whole 1.7 GB monorepo. This follows [0002](0002-language-and-build-system.md)'s pinning rule (exact
  version, archive URL, SHA256; no git clones).
- **Hashing NaNs:** stage and test hashes treat every NaN as one value, since NaN payloads are not portable.
- **Tests:** `tests/determinism_tests.cpp` checks committed hashes per function on a fast input set (every build) and
  on 10^7 inputs (labeled `full`); Windows CI compares them on MSVC. `tests/bench/dmath_bench.cpp` measures ns/call.

## Consequences

- Canonical code has transcendentals that give the same bits on both platforms, at 2.5 to 13 ns per call on the M5 Max
  (sin 7.4, exp 3.2, log 2.5, atan2 8.5, pow 12.9 ns). Turning FMA off costs up to 2× against the FMA path; Apple's
  libm is faster for sin, cos and pow, slower for exp and atan2.
- The pin is a main commit until LLVM 24.1 ships CR pow; moving the pin reruns the CORE-MATH comparison, and CR
  functions must keep their hashes (VCH-105).
- A first configure downloads a 330 MB archive once per dependency-cache miss; CI caches the extracted headers.
- `tools/scripts/wincheck.sh` sees the LLVM headers but takes their Clang branches; only CI proves the MSVC branches.

## Alternatives considered

- **CORE-MATH:** CR everywhere and often fastest, but needs a fork for MSVC and fixing its atan2.
- **LLVM 23.1.6:** a release, but wrong pow for large exponents and no double sinh, cosh, tanh.
- **Our own polynomials:** never CR, so every change is a determinism version bump, for at most 12 % speed.
- **Committing the 26 MB header subset to the repo:** no download, but vendored third-party code outside the pinning
  rule, and history grows with every re-pin.
- **Per-platform libm with FMA:** fastest, but the FMA and no-FMA paths give different bits.
