# 0043: Determinism tiers, strict floating point and veach::dmath

Date: 2026-11-27

## Status

Accepted.

## Context

"Everything is derived from a seed" holds only if the same seed gives the same bits on every machine that must agree:
an Apple arm64 Mac (Apple Clang, Apple libm, Metal) and a Windows x64 PC (MSVC, MSVC CRT, DX12). Multiplayer and saves
regenerate the universe instead of storing it ([0055](0055-universe-identity-and-versioning.md),
[0056](0056-server-authoritative-multiplayer.md)). IEEE 754 makes only `+ − × ÷ √` correctly rounded; the rest is implementation-defined.
Research: [12-determinism](../research/12-determinism.md). Findings (repository and local probes):

- **No floating-point flags anywhere** in `cmake/VeachCompiler.cmake`, the presets or the engine CMake files.
- **Apple Clang fuses `a*b + c`, even at -O0.** Apple Clang 21 defaults to `-ffp-contract=on`; `return a*b + c;` is one
  `fmadd` on arm64. MSVC's `/fp:precise` does not contract (since VS2022). The same expression rounds once on the Mac
  and twice on Windows, in our code, glm and every dependency.
- **Metal compiles with fast math:** the IR shows `fmul fast`, `air.fast_sin`. D3D allows `mad` fused or not.
- **Luau calls the platform libm.** `math.sin`, `math.exp` and `^` go to libm, and the fastcall builtins (`luauF_sin`
  and friends) call C `sin` directly, **bypassing the `math` table**: replacing `math.sin` is silently ignored unless
  the compiler's `disabledBuiltins` lists it. `math.noise` is float32 Perlin of the shape Apple Clang fuses.
- **Untested:** no float result is compared between the Mac gate and Windows CI; 18 `std::unordered_*` uses in
  `engine/` are unaudited. Nothing canonical exists yet.

## Decision

**Three tiers.** Every system, procgen stage and data product is in exactly one. Data flows only downward
(canonical → replay → visual); a canonical stage that reads a visual product fails at registration.

| Tier | Guarantee | Runs on | Members |
|---|---|---|---|
| **Canonical** | Bit-identical on macOS arm64 + Apple Clang and Windows x64 + MSVC, Debug and Release, same source and manifest | CPU only | seeds, addresses, stage records, orbits from ticks, terrain density at collider and edit resolution, gameplay placement, edit application, Luau generator hooks, aggregate simulation state |
| **Replay** | Bit-identical for the same binary, OS and architecture | CPU only | fixed-step simulation, Luau systems, physics (Jolt is also built cross-platform deterministic, [0058](0058-physics-jolt.md)) |
| **Visual** | Within FLIP tolerance ([0031](0031-image-comparison-and-goldens.md)) | GPU and CPU | all rendering, GPU meshing and fields, audio |

**Rules for canonical C++** (directories listed in `tools/scripts/check_repo.py`, starting with `space/gen/**`, and
any file with a `// veach: canonical` header):

1. Built with `-ffp-contract=off -fno-fast-math` (Clang) and an explicit `/fp:precise` (MSVC), never `/fp:fast` or
   `/fp:contract`. Explicit `std::fma` is allowed.
2. Only `+ − × ÷ √`, comparisons, `floor ceil trunc round fmod fma`, conversions, and **`veach::dmath`** for the rest.
   `std::` and `glm::` transcendentals are banned.
3. **`veach::dmath`** (`engine/core/include/veach/core/dmath.hpp`): `sin cos tan asin acos atan atan2 exp exp2 log log2
   log10 pow cbrt hypot sinh cosh tanh` in float and double, **specified as correctly rounded**, from a vendored CR
   library (CORE-MATH or LLVM-libc's math, picked by VCH-078). If CR `pow`/`exp` is too slow in the density hot path, a
   fixed polynomial in our source is allowed for that function, versioned like a generator.
4. Order is data: no `rsqrt`/`rcp` estimates or variable-order SIMD reductions, no iteration over `std::unordered_*`
   where order reaches output, no `std::hash` in outputs, `std::sort` only with a total order, parallel jobs reduce in
   index order.
5. Randomness is counter-based ([0044](0044-counter-based-hash-randomness.md)); decimal inputs use `std::from_chars`.
6. The FP environment (round-to-nearest, FTZ/DAZ off) is asserted at init and, in debug builds, once per fixed step on
   every simulation thread.

**GPU is visual-only.** Shaders keep fast math, passed explicitly in `cmake/VeachShaders.cmake`. An integer-only kernel
may be canonical only if the stage-hash test proves it bit-equal on Metal and WARP (unverified on real DX12 hardware).

**The fix list:**

- Add the flags in `cmake/VeachCompiler.cmake` globally, before dependencies (glm, Luau, Jolt inherit them; this also
  fixes `math.noise`).
- `check_repo.py` fails when the flags are missing, when `-ffast-math`, `-ffp-contract=fast` or `/fp:fast` appear, and
  on banned calls in canonical files.
- A startup self-test at `init core` compares FMA-sensitive expressions and dmath values with constants; a mismatch is
  the new error `core.fp_nondeterministic` (hint: the missing flag) and a nonzero exit.
- Luau generators run in the sandbox of [0052](0052-procgen-runtime.md): `disabledBuiltins` closes the fastcall bypass,
  `math` is rebound to dmath.
- Stage-hash goldens `tests/golden/determinism/<stage>.json` compared by the macOS gate and Windows CI; CR test vectors
  in Debug and Release; one canonical-only recording replayed on both; the `std::unordered_*` uses audited.

**Where it lives:** tiers, flags, dmath, self-test and lint are engine (`engine/core`, `cmake/`, `tools/`); the toolbox
declares which of its stages are canonical ([0042](0042-space-toolbox-layering.md)).

## Consequences

- A seed names one universe on both platforms. Contraction off costs a few percent (Jolt's full rule set: about 8 %).
- dmath and the hash are part of the universe: changing either bumps the determinism contract version
  ([0055](0055-universe-identity-and-versioning.md)). Because dmath is specified as CR, its implementation can be swapped
  freely. The Luau `math` rebinding is permanent once mods use it.

**Validated by** VCH-077 (one committed hash reproduced on the macOS gate and Windows CI; removing `-ffp-contract=off`
on the Mac changes it) and VCH-078 (CR candidates bit-equal over 10^7 inputs on both platforms; ns/call in Tracy).
**Reopen if** VCH-077 can't reach equal hashes with these flags, multiplayer becomes lockstep, or Apple libm and the
MSVC CRT both ship correctly rounded math by default (dmath would forward to `std::`).

## Alternatives considered

- **Same-binary determinism only** (Jolt's default): Mac and Windows could not share a universe.
- **Fixed point for everything authoritative** (Photon Quantum's Q48.16): can't hold a stellar mass.
- **Own non-CR transcendentals** (Box2D, Factorio): any implementation change changes the universe.
- **Canonical data on the GPU:** unverifiable on Windows hardware ([0032](0032-windows-verified-in-hosted-ci.md)).
