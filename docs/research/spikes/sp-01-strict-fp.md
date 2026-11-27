# SP-01: strict floating point and a cross-platform hash (VCH-077)

Validates [ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md): canonical C++ gives the same bits
on macOS arm64 (Apple Clang) and Windows x64 (MSVC).

## What was built

- `cmake/VeachCompiler.cmake`: `-ffp-contract=off -fno-fast-math` (Clang) and `/fp:precise` (MSVC), global, before
  every dependency.
- `tests/determinism_tests.cpp`: 4,096 rounds of Horner polynomials, dot products and a recurrence over runtime
  inputs, every intermediate hashed; the test checks one committed hash.
- `tools/scripts/check_repo.py`: fails when the flags go missing or fast math / FP contraction appears in a build file.

## Result

| Build | Hash |
|---|---|
| macOS arm64, Apple Clang, strict flags, Debug (pre-push gate) and Debug + ASan/UBSan (CI) | `0xd22b4dba96c3f2d5` |
| Windows x64, MSVC, `/fp:precise`, Debug (CI run 46070286109) | `0xd22b4dba96c3f2d5` |
| Same source, Apple Clang without the flags, `-O0` and `-O2` | `0x09763f25784f94bd` |

The hashes match across platforms, and removing the flag on the Mac changes it: **the decision holds**. All goldens
passed unchanged with the new flags (the GPU keeps fast math; CPU-side float changes stayed within tolerance).

## Follow-ups

- A Release-build run on both platforms (CI builds Debug only): part of SP-02's benchmark and stage-hash tests.
- ADR 0043's remaining fix-list items: the `core.fp_nondeterministic` startup self-test and the canonical-file lint
  for banned calls, when the first canonical code (SP-04, SP-05) lands.
