# Q12 raw notes: numeric determinism across platforms and CPU/GPU

Researched 2026-11-27. Repo inspection was done on the dev lead's Mac (Apple clang 21.0.0, clang-2100.1.1.101,
arm64-apple-darwin25.3.0; Apple metal 32023.883). The throwaway probes are in the session scratchpad, not the repo.
The web search budget ran out after 12 searches, so SLEEF and Rapier details come from a single fetch of each
project's own page or from a search snippet. Anything not fetched from a primary source is marked unverified.

## What our build actually does today (repo inspection and local probes)

### Takeaway
Veach sets no floating-point flags anywhere. As a result, Apple Clang on arm64 fuses `a*b + c` into `fmadd`, and MSVC
on x64 does not. The Metal shaders compile with fast math on. Luau's `math.*` calls the platform libm through a
fastcall path that bypasses the `math` table. Today, cross-platform bit equality of anything float-based is luck.

### Cited findings
- `cmake/VeachCompiler.cmake` sets the C++/C standard, ccache, sanitizers and, on MSVC only, `/utf-8 /Zc:preprocessor
  /Zc:__cplusplus`. There is no `-ffp-contract`, `-ffast-math`, `-fno-fast-math`, `/fp:` or `/arch:` anywhere in
  `cmake/`, `CMakeLists.txt`, `CMakePresets.json` or the engine CMake files (grep). Repo inspection.
- Apple Clang 21 defaults: the hidden help says `-ffp-contract` "Default is ... 'on' otherwise" (fuse within one
  statement). Probe: `float f(a,b,c){return a*b+c;}` compiles to `fmadd s0, s0, s1, s2` at **-O0 and -O2**; a
  two-statement version (`t = a*b; return t + c;`) stays `fmul` + `fadd`; `-ffp-contract=off` gives `fmul` + `fadd`
  for both. Local probe (`clang++ -S`).
- MSVC: "starting with Visual Studio 2022, the /fp:precise behavior defaults to fp_contract(off)"; `/fp:contract`
  (new in VS2022) turns contraction back on; `fma`/`fmaf` intrinsics still produce FMA instructions; contractions may
  not appear in debug builds — [MSVC devblog, Dec 2021](https://devblogs.microsoft.com/cppblog/the-fpcontract-flag-and-changes-to-fp-modes-in-vs2022/),
  [/fp docs](https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior).
- Inference: with our defaults, one C++ expression of the form `a*b + c` rounds once on the Mac and twice on Windows.
  This covers glm (header-only, compiled in our TUs; glm SIMD intrinsics are not forced, since only
  `GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_FORCE_XYZW_ONLY` are defined in `engine/core/CMakeLists.txt`)
  and every dependency, Luau included.
- Shaders: `cmake/VeachShaders.cmake` runs `xcrun metal -std=metal3.1 -mmacosx-version-min=14.0 -c` with no math
  flags; `VEACH_SHADER_FLAGS` = `-matrix-layout-column-major -warnings-as-errors all -warnings-disable 41017`
  (+ `-DVEACH_VALIDATE_BINDLESS=1` in Debug). DXIL: `slangc -target dxil -profile sm_6_6` with the same flags.
  Repo inspection.
- Metal default is fast math. Probe kernel (`a*b + c + sin(a) + exp(b) + a/b + rsqrt(c)`) compiled to LLVM IR with the
  same flags as our build: `fmul fast`, `fadd fast`, `fdiv fast`, `@air.fast_sin.f32`, `@air.fast_exp.f32`,
  `@air.fast_rsqrt.f32`. With `-fno-fast-math` (or `-fmetal-math-mode=safe -fmetal-math-fp32-functions=precise`):
  `@air.sin.f32`, `@air.exp.f32`, no `fast` flags, but `@llvm.fmuladd.f32` (fusion permitted). With
  `-fno-fast-math -ffp-contract=off`: plain `fmul` + `fadd`. `-fmetal-math-mode=relaxed` gives
  `reassoc nsz arcp contract afn`. Note that `-fmetal-math-mode=safe` alone still uses `air.fast_sin`/`fast_exp`.
  Local probe. The Apple doc page for `MTLCompileOptions.mathMode` did not render in fetch, so the option semantics
  are from the compiler's own output, not Apple's prose (unverified wording).
- D3D (DXIL) float32 rules: add, subtract and multiply within 0.5 ULP, round to nearest even; divide may be
  `x*(1/y)`; sqrt and rcp 1 ULP; fused ops (mad, dp3) "must produce results that are no less accurate than the worst
  possible serial ordering", so mad may or may not be fused; denorms "MUST be flushed to sign-preserved zero"; no
  ULP bound for exp/log/sin/cos in the section read —
  [D3D11.3 functional spec §3.1](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
  (Assumed to carry over to D3D12/SM 6.6. That is unverified, but D3D12 references the D3D11.3 rules.)
- Denormals on the CPU: a probe on macOS arm64 shows `FLT_MIN * 0.5f` stays subnormal (FTZ off by default). Local
  probe. The x64 MSVC default MXCSR (FTZ/DAZ off) is general knowledge, not verified here.
- Luau (0.748, `.cpm-cache/luau/.../VM/src`): `math_sin` calls C `sin`; `luai_numpow` is `pow`, so the `^` operator
  uses libm; `math.random` is PCG32 with VM-global state (`g->rngstate`); `math.noise` is float32 Perlin, with
  `perlin_lerp = a + t*(b - a)` and `perlin_grad = g0*x + g1*y + g2*z`, exactly the contractible shape. Repo
  inspection of Luau source.
- Luau fastcalls: `lbuiltins.cpp` has `luauF_sin`, `luauF_exp` and `luauF_pow`, which call C `sin`/`exp`/`pow`
  directly when the environment is "safe" (`cl->env->safeenv` checks in `lvmexecute.cpp`). Replacing `math.sin` in
  the global table is therefore not enough in a sandboxed (safe) env. `Luau::CompileOptions::disabledBuiltins` ("null-
  terminated array of library functions that should not be compiled into a built-in fastcall") is the switch. Our
  `engine/script/src/script_asset.cpp` compiles with `optimizationLevel = 1` (constant folding of builtins is gated
  at level ≥ 2 in `Compiler.cpp`). Repo inspection.
- Luau is linked as `Luau.VM Luau.Compiler` (`engine/script/CMakeLists.txt`); no `Luau.CodeGen`, so no native-code
  lowering choices today. Repo inspection.
- `veach::Rng` (`engine/core/include/veach/core/rng.hpp`): PCG32, integer-only, constexpr; `next_float` is
  `(u32 >> 8) * 2^-24` (exact); `range` uses modulo (exact). It is bit-exact on any platform, but it is a
  *stateful stream*: results depend on call order. Repo inspection.
- Replay hash (`engine/runtime/src/replay.cpp`): FNV-1a over `nlohmann::json::dump()` of each Scriptable component,
  types and rows sorted. nlohmann formats doubles with its own Grisu2 `to_chars`
  (`detail/conversions/to_chars.hpp`), not the CRT, so equal bits give equal text on both platforms. Repo
  inspection. The spec scopes replay to "repeat simulation" on a run; nothing tests a Mac recording on Windows.
- CI: Windows runs the `debug` preset (MSVC, `/Od`), macOS runs `asan` and `leaks`; the pre-push gate runs on the
  Mac. MCP/replay tests are CTest entries running `tests/mcp/test_mcp_lab.py` (whether they run on Windows CI was
  not checked). Repo inspection.
- There are 18 `std::unordered_map/set` uses in `engine/` (not audited one by one). The only `std::thread` is the MCP
  stdin reader. Flecs threading is not on yet (ADR 0018: "Flecs threading must later be hooked into our own job
  system"). Repo inspection.
- `std::from_chars` is already used for config parsing (`engine/runtime/src/config.cpp`). Repo inspection.

### Inferences
- The cheapest fix with the biggest effect is one global CMake line on Clang (`-ffp-contract=off`, plus explicit
  `-fno-fast-math`) and an explicit `/fp:precise` on MSVC. It must be global (`add_compile_options` before the
  dependencies are added), because Luau and any physics library are dependencies.
- Luau's `math.noise` very likely returns different bits on Mac and Windows today, because Apple Clang contracts
  `a + t*(b-a)` (FMA-shaped) and MSVC does not. This is an inference from the source plus the probe. It is not
  observed on Windows.
- Shader fast math is fine for visuals and fatal for anything canonical. Turning it off would not buy cross-vendor
  equality anyway (D3D allows mad either way and gives no transcendental bounds).

### Gaps
- No Windows machine: MSVC codegen was not inspected (ADR 0032). The CI probe is spike S1.
- Apple's prose for `MTLMathMode` defaults was not retrieved; the compiler IR is the evidence.
- Not measured: how much `-ffp-contract=off` costs in our own hot loops.

## Sources of nondeterminism (literature)

### Takeaway
IEEE 754 makes +, −, ×, ÷ and √ correctly rounded and therefore reproducible. Everything else, including
transcendentals, contraction, reassociation, estimate instructions, FPU mode changes, parallel order and container
order, is where real projects lost determinism.

### Cited findings
- "the IEEE standard does not guarantee that the same program will deliver identical results on all conforming
  systems"; add/sub/mul/div/sqrt are correctly rounded; sin/cos/tan "are not defined by the IEEE standard"; FMA:
  "if you need determinism across architectures you will have to avoid fmadd"; FPU settings are per-thread and can be
  changed by rogue code (a printer driver); flush-to-zero; reciprocal/rsqrt estimate instructions vary by vendor;
  printing and parsing are not guaranteed identical; advice: assert FPU settings "once per frame", avoid fmadd and
  estimates, test cross-platform early; "You can’t solve this problem just by using epsilons" —
  [Bruce Dawson, Floating-Point Determinism (2013)](https://randomascii.wordpress.com/2013/07/16/floating-point-determinism/).
- Fiedler: the answer is "yes, if…"; arbitrary C/C++ float code across compilers or architectures is "incredibly
  naive" to expect identical; practitioners: MotoGP replays not shareable Xbox↔PC or debug↔release, and a compiler
  update broke old replays (Shawn Hargreaves); Battlezone 2 wrapped transcendentals to make AMD/Intel agree (Ken
  Miller); Gas Powered Games asserted FPU control words every tick (Elijah); physics middleware with CPU-speed-
  dependent iteration counts —
  [Glenn Fiedler, Floating Point Determinism (2010)](https://gafferongames.com/post/floating_point_determinism/).
- Box2D v3: "FMA must be disabled for cross-platform determinism"; `atan2f` "gives different answers on different
  platforms"; custom `sin`/`cos`/`atan2` approximations through `b2MakeRot()`; `sqrtf` kept (correctly rounded);
  `-ffp-contract=off` on Clang/GCC; tested x64 and ARM, MSVC/GCC/Clang, Apple M2 vs AMD Ryzen produce the same
  result; no noticeable cost; multithreaded determinism by per-worker bit arrays merged with OR —
  [Erin Catto, Determinism (2024)](https://box2d.org/posts/2024/08/determinism/).
- Jolt: default determinism needs the same API call order and "The same binary code"; `CROSS_PLATFORM_DETERMINISTIC`
  CMake option makes the library "approximately 8% slower"; requires precise FP mode (`-ffp-model=precise`,
  `/fp:precise`), `-ffp-contract=off`, consistent rounding/DAZ/FTZ on all threads; use Jolt's `Sin`/`Cos`,
  `QuickSort` not `std::sort`, its own heap and `Hash` not `std::hash`; same defines everywhere (no mixing
  `JPH_DOUBLE_PRECISION`); validated on MSVC x64 AVX2, macOS clang ARM64 NEON, Linux x64/ARM/RISC-V, WASM; broadphase
  queries and listener callbacks are not deterministic in order; "less tested than other features"; double
  precision is 5–10% slower — [Jolt Physics docs](https://jrouwe.github.io/JoltPhysics/index.html).
- Rapier: default only locally deterministic; `enhanced-determinism` feature for cross-platform, incompatible with
  `simd8`, needs IEEE 754-2008 targets, trig via nalgebra traits not primitives —
  [Rapier determinism guide](https://www.rapier.rs/docs/user_guides/rust/determinism) (search snippet, page not
  fetched: unverified detail).
- Unity Burst `FloatMode.Deterministic`: long "on hold", listed as supported in Burst 1.8.26 (2025-11-21); 64-bit
  only; no bitwise NaN guarantee; vectorization intentionally disabled because LLVM may use non-deterministic
  instructions when vectorizing, "notably on Arm/Neon" —
  [Unity manual](https://docs.unity3d.com/Manual/burst/float-precision-determinism.html),
  [Burst changelog](https://docs.unity3d.com/Packages/com.unity.burst@1.8/changelog/CHANGELOG.html),
  [issue tracker](https://issuetracker.unity3d.com/issues/sleef-calls-are-not-automatically-vectorized-when-floatmode-dot-deterministic-is-used)
  (from search summaries; pages not fetched in full).
- WG21 P3375R3 (Guy Davidson, 2025-05-12): "C++ does not support reproducible programming"; it proposes only
  `cr_add/sub/mul/div/sqrt`; transcendentals: "It is assumed that the user would provide their own implementations";
  games are a named motivation ("This can only be achieved with bitwise identical results") —
  [P3375R3](https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3375r3.html).
- Jolt's advice to avoid `std::sort` (above) implies unstable sorts differ between standard libraries for equal keys.
  libc++ (Apple) vs MSVC STL make this a live risk for us (inference).

### Inferences
- Our two CPU targets are both SSE2/NEON-class IEEE machines with no x87; the remaining risks are contraction, libm,
  sort/hash/container order, parallel order, and third-party FP-mode changes.

### Gaps
- No primary source quantified how often Apple libm and the MSVC CRT disagree on `sin`/`exp`/`pow` for float and
  double (it is widely reported to be last-ulp differences; unverified count).

## Remedies and precedents

### Takeaway
Two families. Fixed point everywhere (Photon Quantum, classic RTS) buys simple guarantees and pays in range,
ergonomics and GPU fit. Strict IEEE plus owned transcendentals (Box2D v3, Jolt, Rapier, Factorio, Burst
Deterministic) keeps floats and pays a few percent. Correct rounding (CORE-MATH, LLVM-libc) is a newer third
option: it makes any conforming implementation agree by definition.

### Cited findings
- Photon Quantum: the FP struct "completely replaces all usages of floats and doubles to ensure cross-platform
  determinism"; Q48.16 in one `long`; trig/sqrt via approximations and small lookup tables; "a good balance between
  precision and performance with a bias towards the latter" —
  [Quantum 3 fixed point](https://doc.photonengine.com/quantum/v3/manual/quantum-ecs/fixed-point),
  [FP API](https://doc-api.photonengine.com/en/quantum/current/struct_photon_1_1_deterministic_1_1_f_p.html).
- Glenn Fiedler's independent Q48.16 library — [mas-bandwidth/fixed](https://github.com/mas-bandwidth/fixed) (search
  snippet only).
- Factorio: kovarex wrote his own trig "because the results of sin/cos/asin/acos/atan etc. are different on
  different platforms"; a 2020 AMA says they ship their own trig and exponential functions on all platforms —
  [forum (FFF #35 thread)](https://forums.factorio.com/viewtopic.php?p=28956),
  [FFF #340 Deep desyncs thread](https://forums.factorio.com/viewtopic.php?p=487846) (secondhand: forum and AMA, not
  an FFF post; unverified detail).
- SLEEF: "These functions return deterministic values across all architectures"; 1-ULP and faster few-ULP variants;
  x86 SSE2/AVX2/AVX512F, AArch64 Advanced SIMD; Boost Software License 1.0 — [sleef.org](https://sleef.org/).
- CORE-MATH: correctly rounded libm, MIT license (to ease integration); the authors claim correctly rounded `tanf` is
  "up to 3 times faster than GNU libc" (author claim, 2022); a musl maintainer argued CR double costs ~2x code —
  [libc-alpha post](https://inbox.sourceware.org/libc-alpha/mwfspsxm9r.fsf@tomate.loria.fr),
  [musl list](https://openwall.com/lists/musl/2022/01/04/2),
  [Zimmermann FPBench 2023 slides](https://members.loria.fr/PZimmermann/talks/core-math-fpbench2023.pdf),
  [repo mirror](https://github.com/mockingbirdnest/core-math). Whether CORE-MATH is merged into glibc: not found.
- LLVM-libc in MSVC (2026-09-16): "MSVC is using LLVM-libc for compile-time evaluation and runtime execution of math
  functions when `/Zc:cmath` is enabled"; correct rounding makes bits "mathematically unique" across x86-64, ARM64,
  Windows, Linux and library updates; fast path "very similar to fast, non-correctly rounded math libraries" for
  "over 99.99% of all possible inputs"; no MSVC version or benchmarks named —
  [MSVC devblog](https://devblogs.microsoft.com/cppblog/bringing-correctly-rounded-math-to-production-with-llvm-libc/).
- Noise-based RNG: Squirrel Eiserloh, GDC 2017 "Math for Game Programmers: Noise-Based RNG", a stateless hash of
  (position, seed), order-independent; SquirrelNoise5 later fixed a weakness found by Peter Schmidt-Nielsen —
  community sources: [gist](https://gist.github.com/kevinmoran/0198d8e9de0da7057abe8b8b34d50f86),
  [squirrel-noise-5](https://npmjs.com/package/squirrel-noise-5) (GDC Vault not fetched: unverified detail).
- streflop: cited by Fiedler as a library addressing FPU/libm/compiler differences —
  [Fiedler](https://gafferongames.com/post/floating_point_determinism/). Not evaluated further (old, x87-era).

### Inferences
- A correctly rounded dmath is the most future-proof spec: when "the result equals the correctly rounded value", the
  implementation (CORE-MATH, LLVM-libc, a later MSVC or Apple CR libm, even a GPU port) can change without changing
  a single bit, so generator versions survive math-library swaps. Explicit `std::fma` is itself correctly rounded, so
  CR libraries that use it internally stay deterministic even though implicit contraction does not.
- Fixed point fits int64 positions (Q1) and money-like counters. It cannot hold astrophysical ranges (stellar masses
  ~1e30 kg, luminosities ~1e26 W) in Q48.16, so it cannot be the universal answer for Veach's pipeline.

### Gaps
- CR double-precision cost on our hot paths (terrain density with pow/exp) is unmeasured; spike.
- Which CORE-MATH double functions are complete in late 2026: not checked.

## Contract, tiers and enforcement (design notes)

### Takeaway
Three tiers: canonical (bit-exact across platforms and build types, CPU only), replay (bit-exact on one platform and
binary) and visual (tolerance-checked). Data may only flow from canonical to replay to visual. The meeting point for
the cross-platform check is a committed stage-hash file that both the Mac gate and Windows CI must reproduce.

### Cited findings
- Precedents for hash-based checking: Jolt's "Check Determinism" re-runs a step and compares state
  ([Jolt docs](https://jrouwe.github.io/JoltPhysics/index.html)); Box2D's cross-thread-count and cross-CPU comparisons
  ([Box2D](https://box2d.org/posts/2024/08/determinism/)); Veach replay's per-frame FNV-1a (repo).
- ADR 0032/0033: Windows is verified only on hosted CI on WARP; real DX12 GPUs are unverifiable (Blocked). Repo.

### Inferences
- Because no real Windows GPU can be tested, GPU results can never be canonical: we could not prove bit equality on
  the hardware players have. That settles "GPU for visuals only" on policy grounds, before performance.
- Stateless keyed hashing (seed, stage id, body id, index) is required for lazy, any-order generation. `veach::Rng`
  stays for per-entity streams in simulation (stateful is fine there because fixed-step order is deterministic).

### Gaps
- Real cost numbers for every tier: spikes S1–S4 in the deliverable.

## M1-class vs M3 Pro-class

### Takeaway
Determinism is identical on both (same arm64 FP semantics, same binary). What differs is the CPU budget for
canonical generation and the spread of visual-tier GPU results, which our goldens only check on the M5 Max.

### Cited findings
- None primary. Apple core counts (M1: 4 performance + 4 efficiency cores; M3 Pro: 11 or 12 cores, 5–6 performance)
  come from Apple's product specs (training knowledge, not fetched: unverified).

### Inferences
- Canonical CPU generation (collider-grade density near the player, body parameters) costs proportionally more on
  M1; the visual-tier GPU path is where M1 vs M3 Pro differ most, and that tier is free to use fast math.

### Gaps
- Whether M1's and M5's GPUs produce the same bits for the same fast-math shader: unknown, and irrelevant under the
  contract (visual tier).
