# 12: Numeric determinism across platforms and between CPU and GPU

The determinism contract the engine and the space toolbox sign: what must be bit-identical, on which machines, and
how we enforce and test it. Seeded regeneration, multiplayer and record/replay all rest on it. The question was split
out of Q3 by the [scope gap analysis](00-scope.md#questions-added-or-split) because Q2, Q5, Q6, Q9 and record/replay
each depend on it, and it must be settled before Q5 and Q6 commit to "regenerate instead of sync". Raw notes, probes
and every source: [notes/12-determinism-notes.md](notes/12-determinism-notes.md).

## 1. The problem in Veach's terms

"Everything in the universe is derived from a seed" is only true if the same seed gives the same bits on every
machine that has to agree. Two Veach clients, one on an Apple arm64 Mac (Apple Clang, libc++, Apple libm, Metal)
and one on Windows x64 (MSVC, MSVC STL and CRT, DX12), must agree on everything that decides **what exists where**:
which systems and bodies exist and their physical parameters, where a resource node or a tree with a collider sits,
the terrain surface a player stands on and digs into, and where a moon is at tick t. If they disagree by one bit in a
mass, the habitable-zone test can flip; if the density field differs by a millimeter, an edit delta (Q11) lands on a
different surface, and a server and client disagree about collision.

IEEE 754 helps less than people assume. It makes `+ − × ÷ √` correctly rounded, so those five are reproducible
([Dawson](https://randomascii.wordpress.com/2013/07/16/floating-point-determinism/)). Everything else is
implementation-defined: `sin`, `exp` and `pow` from different libms, fused multiply-add (FMA) "contraction" (the
compiler turning `a*b + c` into one instruction that rounds once instead of twice), reassociation under fast math,
reciprocal-estimate instructions, denormal flushing, the order of a parallel reduction, the iteration order of a hash
map and the tie order of `std::sort`.

### Where Veach stands today (repo inspection and local probes)

- **No floating-point flags are set anywhere.** `cmake/VeachCompiler.cmake`, the presets and the engine CMake files
  contain no `-ffp-contract`, `-ffast-math`, `/fp:` or `/arch:`.
- **Apple Clang fuses, MSVC doesn't.** Apple Clang 21 defaults to `-ffp-contract=on`: `return a*b + c;` compiles to
  one `fmadd` on arm64, **even at -O0** (probe). MSVC since VS2022 defaults `/fp:precise` to "contract off"
  ([MSVC blog](https://devblogs.microsoft.com/cppblog/the-fpcontract-flag-and-changes-to-fp-modes-in-vs2022/)), and
  plain x64 has no FMA instruction anyway. So today the same expression rounds once on the Mac and twice on Windows,
  in our code, in glm (header-only, so it compiles with our flags) and in every dependency.
- **Luau is not deterministic across platforms as shipped.** Luau numbers are doubles. `math.sin`, `math.exp` and
  the `^` operator call the platform libm. They also take a "fastcall" path (`luauF_sin` and friends) that calls C
  `sin` directly in a sandboxed environment, **bypassing the `math` table**, so swapping `math.sin` for our own
  function is silently ignored unless the compiler's `disabledBuiltins` option lists it. `math.noise` is float32
  Perlin noise built from `a + t*(b - a)` expressions, exactly the shape Apple Clang fuses. It very likely returns
  different bits on Mac and Windows today (an inference from the source and the probe; not observed on Windows).
  `math.random` is PCG32 with one VM-wide state, so its results depend on which script called it first.
- **Metal shaders compile with fast math.** `xcrun metal` runs with no math flags. In LLVM IR, our probe kernel shows
  `fmul fast` and calls to `air.fast_sin` and `air.fast_exp`. `-fno-fast-math` gives precise `air.sin`, but still
  allows fusion (`llvm.fmuladd`); only `-ffp-contract=off` on top removes it. On the DX12 side the D3D rules allow
  `mad` to be fused or not, require float32 denormals to be flushed, and set no bound on transcendentals
  ([D3D11.3 spec §3.1](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)). WARP,
  hardware DX12 and Metal can all differ legally.
- **What is already right.** `veach::Rng` is integer PCG32 and bit-exact everywhere, but it is a stateful stream, so
  results depend on call order. glm runs its scalar code paths (no `GLM_FORCE_INTRINSICS`). The replay hash formats
  floats with nlohmann's own Grisu2, not the CRT, so equal bits hash equally on both platforms. Simulation is
  single-threaded today. CPU denormals are kept (FTZ off) on macOS arm64 (probe).
- **What is untested.** No test compares any float result between the Mac gate and Windows CI. Windows CI runs the
  `debug` preset, and nothing compares Debug against Release. There are 18 `std::unordered_*` uses in `engine/`,
  which have not been audited for simulation-visible iteration order.

The good news is that nothing canonical exists yet: the reset left no procgen. This is the cheapest moment to set
the contract.

## 2. Options, with precedents

**A. Same-binary determinism only.** Accept that results match only for one binary on one OS and architecture, and
sync everything else over the network. Jolt's default contract is exactly this: "The same binary code is used to run
the simulation" ([Jolt](https://jrouwe.github.io/JoltPhysics/index.html)). MotoGP replays could not move between
Xbox and PC, or between debug and release ([Fiedler](https://gafferongames.com/post/floating_point_determinism/)).
It is cheap, but it kills "regenerate from seed instead of sync" for Mac↔Windows multiplayer, and a seed would no
longer name one universe.

**B. Fixed point for everything authoritative.** Photon Quantum's `FP` (Q48.16 in an int64, trig and square root
through lookup tables) "completely replaces all usages of floats and doubles to ensure cross-platform determinism"
([Photon](https://doc.photonengine.com/quantum/v3/manual/quantum-ecs/fixed-point)); classic RTS lockstep used the
same idea. The guarantee is airtight and independent of the compiler. But Q48.16 cannot hold a stellar mass
(~1e30 kg) or a luminosity, every formula in the physical pipeline (Q3) becomes a range-management exercise, glm and
Jolt don't speak it, the GPU can't share the code, and Luau's numbers are doubles.

**C. Strict IEEE plus owned transcendentals.** Floats and doubles stay, contraction and fast math go off, and the
engine owns every non-basic function. Precedents:
- Box2D v3: `-ffp-contract=off`, custom `sin`/`cos`/`atan2`, `sqrtf` kept, M2 and Ryzen produce identical results,
  "no noticeable performance impact" ([Catto](https://box2d.org/posts/2024/08/determinism/)).
- Jolt `CROSS_PLATFORM_DETERMINISTIC`: precise mode, contraction off, its own `Sin`/`Cos`/`QuickSort`/`Hash`, about
  8% slower, validated on MSVC x64 and Apple Clang ARM64 among others ([Jolt](https://jrouwe.github.io/JoltPhysics/index.html)).
- Rapier `enhanced-determinism`, trig through nalgebra rather than primitives
  ([Rapier](https://www.rapier.rs/docs/user_guides/rust/determinism), unverified detail).
- Factorio: its own trig and exponential functions "because the results of sin/cos/asin/acos/atan etc. are different
  on different platforms" ([forum](https://forums.factorio.com/viewtopic.php?p=28956), secondhand).
- Unity Burst `FloatMode.Deterministic`: it disables vectorization because LLVM can pick non-deterministic
  instructions, "notably on Arm/Neon"
  ([issue](https://issuetracker.unity3d.com/issues/sleef-calls-are-not-automatically-vectorized-when-floatmode-dot-deterministic-is-used)).

**C′. C, with correctly rounded math as the spec of the owned functions.** A correctly rounded (CR) function returns
the unique representable value nearest the true result, so any two CR implementations agree bit for bit by
definition. CORE-MATH (MIT) provides CR libm functions; its authors claim CR `tanf` runs up to 3x faster than glibc's
([libc-alpha](https://inbox.sourceware.org/libc-alpha/mwfspsxm9r.fsf@tomate.loria.fr), author claim). In September
2026, MSVC started using LLVM-libc's CR math under `/Zc:cmath`, with a fast path "very similar to fast, non-correctly
rounded math libraries" for "over 99.99% of all possible inputs", and identical bits across x86-64 and ARM64
([MSVC blog](https://devblogs.microsoft.com/cppblog/bringing-correctly-rounded-math-to-production-with-llvm-libc/)).
SLEEF promises "deterministic values across all architectures" at 1 ULP, but it is not CR, so it agrees only with
itself ([sleef.org](https://sleef.org/)). WG21's P3375 proposes only `cr_add`…`cr_sqrt` and leaves transcendentals
to users ([P3375R3](https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3375r3.html)). The standard library will
not solve this for us.

**D. Canonical data on the GPU.** Run procgen (density, placement) in compute and treat the result as authoritative,
either with integer or fixed-point-only kernels or with strict float flags. Integer hashing and integer atomics are
bit-exact on any conforming GPU, but float kernels are not: D3D allows `mad` fused or unfused and leaves
transcendentals unbounded. **Veach cannot verify it.** Real Windows GPUs are Blocked
([ADR 0032](../adr/0032-windows-verified-in-hosted-ci.md)), CI only has WARP, and WARP is "a conformance reference,
not a GPU" ([ADR 0033](../adr/0033-dx12-on-warp-in-ci.md)). A guarantee we cannot test on the hardware players have
is not a guarantee.

**Randomness for procgen.** Stateful streams (`veach::Rng`, `math.random`) give different results if stages
generate in a different order, which lazy, streamed generation always does. Counter-based hashing takes
`(seed, stage, id, index)` and returns bits with no state: Squirrel Eiserloh's noise-based RNG (GDC 2017;
SquirrelNoise5 fixed a weakness of the talk's version) and PCG-style hash finalizers
([community implementation](https://gist.github.com/kevinmoran/0198d8e9de0da7057abe8b8b34d50f86); talk not fetched,
unverified detail). Integer-only, so it is bit-exact on CPU and GPU alike.

## 3. Tradeoffs

| | A. Same binary | B. Fixed point | C. Strict IEEE + owned math | C′. C with CR spec | D. GPU canonical |
|---|---|---|---|---|---|
| Complexity | None now; sync everything later (Q6 grows) | High: a numeric type through Q2, Q3, Q4, Q9, Luau | Low: flags, one math library, lint | Low, plus a vendored CR library | High: two kernels per stage or integer-only noise |
| Performance | Best | Mixed: mul/add fast, div and trig via LUT slower, no glm/SIMD reuse | Contraction off costs a few % (Box2D: unnoticed; Jolt: ~8% for all its rules); owned trig ≈ libm | CR fast path ≈ libm (vendor claims); measure | Fastest generation |
| Determinism | Same binary only | Bit-exact everywhere, by construction | Bit-exact across our two CPUs if the rules hold; must be tested | Same, and immune to math-library swaps | Unverifiable on Windows hardware |
| Extensibility | Mods can't regenerate | Luau would need an FP userdata type: slow, alien | Luau gets a `veach.dmath` module | Same | Luau can't run in kernels |
| Testability | Replay only | Easy | Stage hashes compared Mac gate ↔ Windows CI | Same, plus CR test vectors | Metal + WARP only |
| Scale range | Any | Q48.16 fails on astrophysical ranges; needs per-quantity formats | doubles cover 1e-308…1e308 | Same | float32 on GPU; poor at real scale |

**Verdict:** C′ for canonical CPU code, fixed point only where Q1 already wants integers (positions as int64 cells,
ticks), and the GPU for visuals only.

## 4. The contract (what the recommendation means in practice)

**Three tiers.** Every system, procgen stage and data product is in exactly one tier. Data may only flow downward
(canonical → replay → visual), never upward.

| Tier | Guarantee | Where it must hold | Members |
|---|---|---|---|
| **Canonical** | Bit-identical across Apple arm64 + Apple Clang and Windows x64 + MSVC, Debug and Release, for the same source and generator manifest (Q11) | CPU only | Seed hierarchy and ids; galaxy, system and body physical parameters (mass, radius, composition, orbit elements, rotation, tilt) and everything derived from them (gravity, temperature, climate, biome assignment); the universal clock and on-rails orbit and rotation evaluation from integer ticks (Q2); the terrain density field at collider and edit resolution (Q4); procgen placement of anything gameplay sees (resources, colliders, POIs, creatures' spawn records); edit application (Q11); Luau generator hooks (Q5); names and lore text |
| **Replay** | Bit-identical for the same binary on the same OS and architecture, given seed, script and input | CPU only | The fixed-step simulation: gameplay systems, Luau systems, physics (Q9), simulation LOD stepping (Q7). Physics is canonical-capable if Q9 picks Jolt with `CROSS_PLATFORM_DETERMINISTIC` (recommended, see §8) |
| **Visual** | Matches within perceptual tolerance (FLIP goldens, [ADR 0031](../adr/0031-image-comparison-and-goldens.md)) | GPU and CPU | All rendering: GPU terrain meshing for display, atmosphere, clouds, ocean, vegetation instancing and wind, particles, starfield, impostors, photo mode, audio |

**Rules for canonical code (C++):**
1. Built with contraction and fast math off on every target, dependencies included: Clang `-ffp-contract=off
   -fno-fast-math`, MSVC `/fp:precise` explicitly, never `/fp:fast` or `/fp:contract`. Explicit `std::fma` is
   allowed, because it is correctly rounded.
2. Only `+ − × ÷ √`, comparisons, `floor/ceil/trunc/round`, `fmod`, `fma`, conversions, and `veach::dmath` for
   everything else (`sin cos tan asin acos atan atan2 exp exp2 log log2 log10 pow cbrt hypot sinh cosh tanh`).
   `std::` and `glm::` transcendentals are banned in canonical code.
3. `veach::dmath` functions are specified as **correctly rounded** in float and double, implemented from a vendored
   CR library (CORE-MATH or LLVM-libc's math, picked by spike S2). If CR double `pow` turns out to be too expensive in
   the density hot path, a documented non-CR fallback is allowed: a fixed polynomial in our own source, compiled with
   rule 1. It must be versioned like a generator, because changing it changes the universe.
4. No SIMD whose result depends on lane count: lane-wise ops are fine; horizontal reductions run in a fixed order; no
   `rsqrt`/`rcp` estimate intrinsics.
5. Order is data. No iteration over `std::unordered_*` or pointer-keyed containers where order reaches the output;
   no `std::hash` in outputs; `std::sort` only with a total order on keys (ties differ between libc++ and the MSVC STL,
   which is why Jolt replaces it). Parallel jobs write to indexed slots and reduce in index order (Box2D's per-worker
   bit arrays are the model).
6. Randomness is counter-based: `veach::hash(seed, stage_id, object_id, index)` (a SquirrelNoise5- or PCG-hash-style
   integer function). `veach::Rng` streams stay for replay-tier simulation, seeded from that hash.
7. Canonical inputs in data files are integers or decimal strings parsed with `std::from_chars` (correctly rounded by
   the standard), never through a CRT `strtod`.
8. FP environment is asserted: round-to-nearest, FTZ/DAZ off, checked at init and once per fixed step in debug builds
   on every simulation thread, after any third-party DLL may have changed it (Fiedler's and Dawson's printer-driver
   stories).

**Rules for Luau generators:** generator hooks run in a deterministic environment:
- the script compiles with `disabledBuiltins` covering every libm-backed `math.*`, `math.noise` and `math.random`;
- those names in `math` are replaced by `veach.dmath` equivalents, so ordinary-looking code (`math.sin(x)`) is
  canonical;
- `x ^ y` is lint-flagged in generators (it compiles to libm `pow`; use `math.pow`, which is remapped), or the VM's
  `luai_numpow` is pointed at `dmath::pow` for the whole engine (decided in spike S4);
- no `math.random` and no `os.clock`; randomness comes from `veach.hash(...)` keyed by the stage context.

Luau's `vector` is float32 with basic ops only, so it is deterministic once rule 1 applies to the VM's own build.
If Luau native codegen (`Luau.CodeGen`) is ever linked, generators must stay interpreted until its arm64 and x64
lowerings are proven identical (unverified whether it emits FMA).

**Rules for the GPU:** GPU output never becomes canonical or replay data. Where gameplay needs something the GPU
also draws (terrain surface, tree positions), the canonical CPU path produces the authoritative version (collider
chunks near players, placement records), and the GPU reproduces it for display within a tolerance. The one
exception path is integer-only kernels (hashing, integer noise, integer atomics). They may be canonical only if they
are proven bit-equal on Metal and WARP by the stage-hash test, and they are recorded as unverified on real DX12
hardware (Blocked). Shaders keep fast math, because the visual tier doesn't need anything stricter.

**Enforcement and tests:**
- **Flags:** set in `cmake/VeachCompiler.cmake` globally, before dependencies are added. `tools/scripts/check_repo.py`
  fails if they are missing or if `/fp:fast`, `-ffast-math` or `-ffp-contract=fast` appear anywhere.
- **Startup self-test:** at `init core`, a handful of FMA-sensitive expressions and dmath values are computed and
  compared with constants. A mismatch is a new error code (proposed `core.fp_nondeterministic`, with a hint naming the
  flag) and makes the exit code nonzero. It catches flag drift and a changed FP environment.
- **Banned-function lint:** canonical code lives in marked directories or files (proposed: `space/gen/**` and any file
  with a `// veach: canonical` header). The hygiene check greps them for `std::`/`glm::` transcendentals,
  `unordered_`, `std::hash`, `std::sort(`, `rand(`, `<random>` and estimate intrinsics. Bare violations fail CI, like
  bare TODOs do today.
- **Stage-hash goldens:** each registered canonical stage declares a test seed set. A test computes
  FNV-1a/xxHash-style hashes of each stage's output (over a fixed text or byte serialization) and compares them with
  a committed file `tests/golden/determinism/<stage>.json`. The Mac gate and Windows CI both compare against the same
  committed file, so the two platforms meet in the repo without exchanging artifacts. Changing a hash on purpose is a
  generator version bump (Q11) with a regeneration target, like `update-goldens`.
- **dmath conformance:** CR test vectors (hard-to-round cases from the CR library's test suites) run on both
  platforms in Debug and Release.
- **Replay:** stays same-platform (replay tier). In addition, one committed recording of a canonical-only scenario
  (generate a system, evaluate orbits at fixed ticks, apply edits) replays on Windows CI and on the Mac gate. That is
  the "replay on the other architecture" test the scope analysis asked for.

## 5. Extension points

| What | From Luau | From a C++ module | Toolbox core only |
|---|---|---|---|
| New procgen stage, body kind, biome rule | Yes: a generator hook in the deterministic environment, `veach.dmath` + `veach.hash`; declares tier `canonical` or `visual` | Yes: a stage registered with its tier, version and test seeds; the lint and stage-hash test pick it up | The tier model itself, the data-flow rule, the manifest (Q11) |
| New math function | Composes `veach.dmath` primitives (deterministic, since basic ops are) | Added to `veach::dmath` with CR test vectors or as a versioned polynomial | The CR library choice |
| New randomness source | `veach.hash(...)` with a new key | A new keyed hash with a version | Hash key layout of the seed hierarchy (Q3) |
| GPU procgen feature | No (visual only) | A RenderFeature, visual tier; or an integer-only canonical kernel behind the stage-hash test | Permission for any GPU stage to be canonical |
| New simulated system | `veach.system`, replay tier | Replay tier; canonical if it follows the rules and adds hash tests | — |

The registry stores the tier as data on each stage. A stage that claims `canonical` and reads a `visual` product
fails at registration with an error naming both stages (enforced by the stage graph, Q5).

## 6. Interactions with the other questions

- **Q1 coordinates:** int64 or fixed-point cell positions are canonical-friendly by construction; double positions
  are canonical under the rules. The render-side float32 camera-relative positions are visual tier. Q1 must not make
  canonical data depend on the floating origin's current offset.
- **Q2 orbits and time:** the universal clock is integer ticks. Orbital phase from `tick mod period` in integers or
  doubles, and Kepler solving with a fixed iteration count (not "until converged", which can iterate differently by
  an ULP) and dmath trig. Eclipse queries are canonical if gameplay uses them (solar power), visual otherwise.
- **Q3 pipeline:** every stage up to biome assignment is canonical; the seed hierarchy uses `veach::hash`. Threshold
  tests (habitable zone, Roche limit, tidal locking) are where one-ULP differences flip outcomes. The stage-hash tests
  catch them.
- **Q4 terrain:** the density function exists once in canonical C++ (CPU, collider resolution) and once for display
  (GPU, fast math). Q4 must size the CPU collider budget and define the allowed visual vs physical mismatch. Edits are
  applied by the canonical path.
- **Q5 procgen graph:** hosts the tier tags, the deterministic Luau environment and the "no upward flow" check. GPU
  nodes are visual unless integer-only.
- **Q6 networking:** this contract is what makes "regenerate instead of sync" legal for canonical data. It
  recommends a server-authoritative simulation (replay tier on the server, snapshots to clients) rather than lockstep,
  which would need the whole simulation in the canonical tier.
- **Q7 simulation LOD:** closed-form catch-up from ticks is canonical if two players can both observe it; aggregate
  stepping on the server is replay tier.
- **Q8 renderer:** entirely visual; keeps fast math; reads canonical data through extraction.
- **Q9 physics:** replay tier minimum. With Jolt, `CROSS_PLATFORM_DETERMINISTIC` (~8%) plus the flags makes physics
  canonical-capable. Broadphase query order and listener callbacks are not deterministic in Jolt even then, so
  gameplay must sort query results.
- **Q10 flight:** the flight model is simulation (replay tier); gravity from body parameters is canonical input.
- **Q11 versioning:** dmath, the hash function and every canonical stage are versioned parts of the generator
  manifest; a changed stage hash is a version bump. Recordings already carry the engine version.

## 7. M1-class vs M3 Pro-class

The determinism contract is identical on both: the same arm64 FP semantics, the same binary and the same flags. The
differences are budgets:

- **CPU:** canonical generation runs on CPU cores. An M1 has 4 performance + 4 efficiency cores, an M3 Pro 5–6
  performance + 6 efficiency (Apple specs, unverified here). Canonical collider-grade density near the player,
  placement and body generation therefore have roughly a third to half less headroom on M1. Turning contraction off
  costs the same few percent on both.
- **GPU:** the visual tier is free to use fast math, which matters most on M1. Visual results may differ in the
  last bits between GPU families (M1 is Apple GPU family 7, M3 family 9; unverified), which the contract allows. Our
  goldens only check the M5 Max, so a visual-tier difference on M1 would go unseen, but it could never desync
  gameplay.
- **Where it bites:** if Q4 wants high-resolution colliders far from the player, M1's CPU is the limit, not the GPU.
  The answer is a smaller canonical radius (colliders only near bodies that need them), not GPU canonical data.

## 8. Recommendation

1. **Adopt the three-tier contract (§4)**, with the data-flow rule "canonical → replay → visual, never upward", and
   **option C′** for the canonical tier: strict IEEE on both compilers, engine-owned `veach::dmath` specified as
   correctly rounded, counter-based hashing for procgen randomness, and a deterministic Luau generator environment.
2. **Fix the flags now, before any canonical code exists:** `-ffp-contract=off -fno-fast-math` globally on Clang,
   explicit `/fp:precise` on MSVC, the startup self-test and the hygiene check. This also fixes Luau's `math.noise`
   divergence for free.
3. **GPU is visual-only.** Shaders keep fast math. Integer-only canonical kernels are a reviewed exception, verified
   on Metal and WARP, and recorded as unverified on Windows hardware.
4. **Physics:** if Q9 picks Jolt, build it with `CROSS_PLATFORM_DETERMINISTIC` from the start (~8%). Reverting it
   later is cheap; turning it on after recordings and saves exist is not.
5. **Test:** committed stage-hash goldens compared by both the Mac gate and Windows CI, CR test vectors, and one
   cross-architecture replay of a canonical scenario.

**What would change my mind:**
- If **Q6 chooses lockstep** multiplayer, the whole simulation must become canonical, and Jolt's flag stops being
  optional. Fixed point (option B) would then be worth re-examining for gameplay state.
- If **spike S2 shows CR double `pow`/`exp` costing far more than libm** in the density hot path, the canonical
  density moves to a versioned polynomial dmath (rule 3's fallback), or to float32 CR with doubles only for large
  coordinates.
- If **a real Windows GPU runner appears** (ADR 0032 revisited) and integer-only GPU stages prove bit-equal on Metal,
  WARP and NVIDIA/AMD hardware, GPU canonical stages could become a regular extension point instead of an exception.
- If **Apple's libm and the MSVC CRT both ship correctly rounded math by default**, `veach::dmath` could forward to
  `std::` (results would be identical by definition), keeping only the lint.

## 9. What is expensive to undo

- **The dmath implementation and the hash function**, once a universe exists: changing either changes every
  canonical bit, so it is a generator version bump for every stage that uses it (Q11). Specifying dmath as CR is what
  makes swapping implementations free.
- **The tier assignment of the density field.** If gameplay is allowed to read GPU-generated terrain even once,
  everything built on it (edits, colliders, placement) inherits unverifiable bits. Moving it back to the CPU later
  means re-deriving every edit.
- **Lockstep vs server authority (Q6),** because it decides how much of the simulation must be canonical.
- **Jolt's cross-platform mode,** because turning it on changes physics results, which invalidates every recording
  and replay-based test.
- **The Luau generator surface.** Once mods use `math.sin` in generators, the remapping to dmath must stay forever.
  Deciding the deterministic environment before the first hook is cheap; retrofitting it breaks mods.

Cheap to undo: the compiler flags (a few percent of performance, reversible), the lint, the self-test, shader fast
math.

## 10. Proposed ADR titles

1. **Determinism tiers: canonical, replay and visual** (the contract, the data-flow rule, what sits where).
2. **Strict floating point on every CPU target** (`-ffp-contract=off -fno-fast-math`, `/fp:precise`, FP-environment
   assertions, startup self-test).
3. **veach::dmath: correctly rounded math for canonical code** (spec, vendored implementation, test vectors,
   versioning).
4. **Counter-based hashing for procgen randomness** (`veach::hash` key layout; `veach::Rng` stays for simulation).
5. **GPU output is visual-only** (fast math kept; the integer-only canonical exception and its verification).
6. **A deterministic Luau environment for generators** (`disabledBuiltins`, `math` remapped to dmath, no
   `math.random`, `^` handling).
7. **Cross-platform stage-hash tests** (committed hash goldens compared by the Mac gate and Windows CI; regeneration
   is a version bump).

ADR 0018's determinism section is extended by 1 and 2 (superseded where they conflict); ADR 0007's Luau section by 6.

## 11. Spike candidates

Each is a throwaway prototype, sized by scope, with a visible success criterion.

- **S1: Flags flip and the first cross-platform hash.** Scope: one CMake change, one test that hashes 10⁶
  evaluations of FMA-shaped glm expressions and of Luau `math.noise`, one committed hash file, one CI round.
  Success: the same hash on the Mac gate and the Windows CI log; with the flag removed on the Mac, the hash differs
  (proving the test detects contraction). A Tracy capture of the lab before and after shows the cost of contraction
  off.
- **S2: dmath candidates.** Scope: `sin cos exp log pow atan2` in float and double from CORE-MATH, LLVM-libc and a
  hand-written polynomial; a micro-benchmark and a test-vector suite. Success: bit-identical results on Mac and
  Windows for all CR candidates, and a table of ns per call against Apple libm on the M5 Max (Tracy or a benchmark
  binary). This picks the implementation and decides whether rule 3's fallback is needed.
- **S3: Canonical density vs visual density.** Scope: one SDF planet function (from Q4's spike) evaluated in
  canonical C++ and in a Slang compute kernel with fast math, at s = 0.01 and s = 1 radius. Success: a screenshot
  overlaying the collider mesh (CPU) on the visual mesh (GPU) at the surface, plus the measured maximum deviation in
  millimeters, under a threshold Q4 sets. It also gives the CPU cost per collider chunk on the M5 Max, the number Q4
  needs for its M1 budget.
- **S4: Deterministic Luau generator.** Scope: compile options with `disabledBuiltins`, `math` remapped to
  `veach.dmath`, `veach.hash`, and one generator script that places 10⁴ objects. Success: the stage hash matches on
  Mac and Windows CI; the same script with fastcalls left on reproduces the bypass (proving the test sees it); a
  Tracy zone shows generator cost with dmath vs libm.
- **S5 (belongs to Q9): Jolt cross-platform replay.** Scope: a stack of bodies on an edited terrain chunk, recorded
  on the Mac, replayed on Windows CI with `CROSS_PLATFORM_DETERMINISTIC`. Success: replay finishes with no
  `replay.diverged`; the cost is measured with the flag on and off.

## 12. Product decisions surfaced

1. **Multiplayer model: server-authoritative or lockstep?** Recommendation: server-authoritative, with canonical
   regeneration of the universe on clients. Lockstep would put all gameplay, physics and Luau systems in the canonical
   tier, raising the cost of every system and every mod.
2. **Must Mac and Windows players share one universe and one server?** Recommendation: yes. This is what the
   canonical tier buys, and the seed only means something if it holds.
3. **Do creators' Luau generators have to be multiplayer-safe?** Recommendation: yes, by construction. Generators see
   only the deterministic environment, so there is no "unsafe but faster" mode for generators (simulation scripts
   are unaffected).
4. **Are saves and recordings portable across platforms?** Recommendation: saves yes (they store seed, manifest and
   deltas over canonical data); recordings only same-platform, except canonical-only scenarios.
5. **Is a small visual difference between Mac and Windows acceptable?** Recommendation: yes, within the FLIP
   tolerances we already use. Pixel-identical cross-platform screenshots (for example for shared photo-mode seeds)
   are not promised.

## 13. Sources

Primary and project documentation:
- Bruce Dawson, Floating-Point Determinism (2013): https://randomascii.wordpress.com/2013/07/16/floating-point-determinism/
- Glenn Fiedler, Floating Point Determinism (2010): https://gafferongames.com/post/floating_point_determinism/
- Erin Catto, Box2D v3 Determinism (2024): https://box2d.org/posts/2024/08/determinism/
- Jolt Physics documentation, Deterministic Simulation: https://jrouwe.github.io/JoltPhysics/index.html
- Rapier determinism guide (search snippet; unverified detail): https://www.rapier.rs/docs/user_guides/rust/determinism
- MSVC, The /fp:contract flag and changes to FP modes in VS2022: https://devblogs.microsoft.com/cppblog/the-fpcontract-flag-and-changes-to-fp-modes-in-vs2022/
- MSVC /fp reference: https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior
- MSVC, Bringing correctly rounded math to production with LLVM libc (2026-09-16): https://devblogs.microsoft.com/cppblog/bringing-correctly-rounded-math-to-production-with-llvm-libc/
- Direct3D 11.3 functional spec, floating point rules: https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
- Apple, MTLCompileOptions.mathMode (page did not render; behavior taken from compiler output): https://developer.apple.com/documentation/metal/mtlcompileoptions/mathmode
- Photon Quantum 3, Fixed point: https://doc.photonengine.com/quantum/v3/manual/quantum-ecs/fixed-point and FP API: https://doc-api.photonengine.com/en/quantum/current/struct_photon_1_1_deterministic_1_1_f_p.html
- Unity Burst float precision and determinism: https://docs.unity3d.com/Manual/burst/float-precision-determinism.html; changelog: https://docs.unity3d.com/Packages/com.unity.burst@1.8/changelog/CHANGELOG.html; vectorization issue: https://issuetracker.unity3d.com/issues/sleef-calls-are-not-automatically-vectorized-when-floatmode-dot-deterministic-is-used
- SLEEF: https://sleef.org/
- CORE-MATH: https://github.com/mockingbirdnest/core-math (mirror), https://members.loria.fr/PZimmermann/talks/core-math-fpbench2023.pdf, https://inbox.sourceware.org/libc-alpha/mwfspsxm9r.fsf@tomate.loria.fr, https://openwall.com/lists/musl/2022/01/04/2
- WG21 P3375R3, Reproducible floating-point results: https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3375r3.html

Secondary (unverified detail):
- Factorio trig: https://forums.factorio.com/viewtopic.php?p=28956, https://forums.factorio.com/viewtopic.php?p=487846
- Squirrel Eiserloh noise-based RNG (GDC 2017), community implementation: https://gist.github.com/kevinmoran/0198d8e9de0da7057abe8b8b34d50f86; SquirrelNoise5: https://npmjs.com/package/squirrel-noise-5
- Glenn Fiedler's fixed-point library: https://github.com/mas-bandwidth/fixed

Repository (inspected 2026-11-27): `cmake/VeachCompiler.cmake`, `cmake/VeachShaders.cmake`,
`engine/core/include/veach/core/rng.hpp`, `engine/core/CMakeLists.txt`, `engine/runtime/src/replay.cpp`,
`engine/script/src/script_asset.cpp`, `engine/script/CMakeLists.txt`, Luau 0.748 `VM/src/lmathlib.cpp`,
`VM/src/lbuiltins.cpp`, `VM/src/lnumutils.h`, `Compiler/include/Luau/Compiler.h`, ADRs 0002, 0007, 0009, 0013, 0015,
0018, 0032, 0033, `docs/specs/record-replay/spec.md`, `.github/workflows/ci.yml`. Local probes: Apple clang 21.0.0
and Apple metal 32023.883 on the dev lead's Mac.
