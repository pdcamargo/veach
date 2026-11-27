# Q9 physics at planetary scale: raw notes

Researcher notes for [09-physics.md](../09-physics.md). Date: 2026-11-27. "(computed)" marks a derivation from
standard physics, not a source. "(unverified)" marks a claim with no primary source found or fetched. API facts marked
"(header)" were read from the libraries' public headers on their default branches today
(`jrouwe/JoltPhysics` master, `erincatto/box3d` main).

## Engines: Jolt, Box3D, PhysX 5, Bullet, Havok, Rapier

### Takeaway
Jolt (MIT, v5.7.0 released 2026-11-14) is the mature choice: double-precision mode, a documented cross-platform
determinism build, multiple physics systems for space games, rollback state, used by two Guerrilla/Kojima AAA titles.
Box3D (MIT, v0.1.0, 2026-06-30) has almost the same feature list on paper (doubles, cross-platform determinism, built-in
recording with state hashes) but its author calls it alpha. PhysX is float-only, Havok is proprietary, Rapier is Rust.

### Cited Findings
- Jolt is MIT: "The project is distributed under the MIT license"; "Used by Horizon Forbidden West and Death Stranding 2:
  On the Beach"; supports Windows x64/ARM64 and macOS x64/ARM64 — [Jolt README](https://github.com/jrouwe/JoltPhysics)
- Jolt latest release v5.7.0, published 2026-11-14 (GitHub API `releases/latest`) — [Jolt releases](https://github.com/jrouwe/JoltPhysics/releases)
- Jolt v5.7.0 "Removed PhysicsSettings::mDeterministicSimulation ... The simulation is now always deterministic";
  v5.6.0 "Up to 40% performance improvement (scene dependent) and up to 70% memory reduction"; v5.6.0 "Added interface
  to run compute shaders on the GPU with implementations for DX12, Vulkan and Metal"; v5.3.0 "Optimized creation of
  MeshShape. Improves build speed by about 25% and reduces number of allocations by a factor of 1000" —
  [Jolt release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html)
- Jolt has height field (`HeightFieldShape`, static only), triangle mesh (`MeshShape`, "mostly used for static
  geometry", "fairly expensive" when dynamic), `MutableCompoundShape` ("trades construction time for runtime
  performance"), `CharacterVirtual` (collision-query character outside the step, "Multiple CharacterVirtuals can update
  concurrently"), enhanced internal edge removal (v5.0) — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- Jolt `HeightFieldShape::SetHeights` edits heights in place; start X must be a multiple of `mBlockSize`; samples
  compressed per block (`mBitsPerSample = 8` default) (header) — [HeightFieldShape.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Collision/Shape/HeightFieldShape.h).
  v5.7.0 fixed "SetHeights corrupted the bounding volume tree when the number of blocks was not a power of 2" —
  [release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html)
- Jolt `BodyCreationSettings::mGravityFactor = 1.0f` "Value to multiply gravity with for this body";
  `mMaxLinearVelocity = 500.0f` (m/s) default; `mMotionQuality` Discrete or LinearCast (CCD) (header) —
  [BodyCreationSettings.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Body/BodyCreationSettings.h)
- Jolt characters have `SetUp(Vec3)`: arbitrary up vector for spherical gravity (header) —
  [CharacterBase.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Character/CharacterBase.h)
- Jolt job systems shipped: `JobSystem.h` (interface), `JobSystemSingleThreaded.h`, `JobSystemThreadPool.h`,
  `JobSystemWithBarrier.h` (base for custom ones) — [Jolt/Core](https://github.com/jrouwe/JoltPhysics/tree/master/Jolt/Core)
- Jolt bulk add: `AddBodiesPrepare` "can be done from a background thread", `AddBodiesFinalize` "atomically adds all
  bodies", `AddBodiesAbort` "Useful when streaming in level sections"; one-by-one adds give "a really inefficient
  broadphase" (use `OptimizeBroadPhase`) — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- Jolt shape binary state "is very likely to change between versions of the library" — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- Box3D: MIT (GitHub `spdx_id: MIT`), C17, "only supports 64-bit desktop platforms", SIMD SSE2/AVX2/Neon; features
  "Cross platform determinism", "Recording and replay", "Convex hulls, capsules, spheres, triangle meshes, and height
  fields", "Character mover", "Extensive multithreading and SIMD" — [Box3D README](https://github.com/erincatto/box3d)
- Box3D v0.1.0 "Initial Release", published 2026-06-30: "I consider this to be alpha software"; repo last pushed
  2026-11-27, 6.8k stars — [Box3D releases](https://github.com/erincatto/box3d/releases)
- Box3D origin and users: began as a fork of Dirk Gregorius's Rubikon-Lite; "Despite usage in a few games now, I still
  consider Box3D to be alpha software"; users: The Legend of California (Kintsugiyama), s&box (Facepunch), Esoterica,
  and "A 1000-player space game" by Glenn Fiedler; "Large world support with doubles for position"; "Multi-threading
  hooks", "Optional internal scheduler"; PRs later "probably using a CLA" — [Announcing Box3D](https://box2d.org/posts/2026/06/announcing-box3d/)
- Kintsugiyama: The Legend of California uses Box3D for "falling redwoods, ragdolls, and voxel terrain" —
  [Kintsugiyama](https://www.kintsugiyama.com/updates/box3d-is-now-open-source)
- Box3D manual: "a work in progress. It will be updated as v1.0 approaches" — [Box3D docs](https://box2d.org/documentation3d/)
- Box3D `b3BodyDef::gravityScale`, `b3World_SetGravity` (one vector per world), `maximumLinearSpeed` default
  400 m/s, `b3EnqueueTaskCallback`/`b3FinishTaskCallback` task hooks, `b3World_SetWorkerCount`;
  `b3CreateHeightField(def)` from a `b3HeightFieldDef` (no in-place height edit found; heights quantized between
  `globalMinimumHeight`/`globalMaximumHeight` so neighbors line up); `b3Shape_SetMesh` swaps a mesh on a shape;
  `b3World_StartRecording`/`b3ValidateReplay` replays "every op" and checks embedded state hashes; a `b3RecPlayer`
  with seek and keyframe snapshots (header) — [box3d.h](https://github.com/erincatto/box3d/blob/main/include/box3d/box3d.h),
  [types.h](https://github.com/erincatto/box3d/blob/main/include/box3d/types.h), [collision.h](https://github.com/erincatto/box3d/blob/main/include/box3d/collision.h),
  [types.c](https://github.com/erincatto/box3d/blob/main/src/types.c)
- PhysX 5 has `PxScene::shiftOrigin` — [PhysX 5.5 API](https://nvidia-omniverse.github.io/PhysX/physx/5.5.0/_api_build/classPxScene.html);
  NVIDIA forum: the SDK is single precision only, no concrete plans for large-world tiling —
  [NVIDIA forum](https://forums.developer.nvidia.com/t/simulation-position-precision/245493); switching `PxReal` to
  double does not work out of the box — [NVIDIA forum](https://forums.developer.nvidia.com/t/can-i-change-pxreal-to-use-double-precision-in-the-free-version-of-physx/31210)
- Rapier: `enhanced-determinism` "enables cross-platform determinism ... across all 32-bit and 64-bit platforms that
  implements the IEEE 754-2008 standard strictly"; incompatible with `simd8`; f64 crates `rapier3d-f64` —
  [Rapier determinism](https://rapier.rs/docs/user_guides/rust/determinism), [getting started](https://rapier.rs/docs/user_guides/rust/getting_started/)
- Space Engineers runs Havok inside Keen's VRAGE engine ("Real Physics" with Havok Physics and Destruction) —
  [Keen VRAGE](https://keenswh.com/vrage)

### Inferences
- Box3D's API is changing (v0.1.0, WIP manual, CLA not yet in place). Writing the engine's physics module against it
  now means tracking breaking changes for an unknown number of releases.
- Jolt's v5.6.0 ARM64/x64 double-precision determinism bug shows that exactly our configuration (double precision,
  cross-platform, Mac arm64 plus Windows x64) is the least-tested corner of the library: we must test it ourselves.
- Both libraries have gravity scale per body plus one world gravity vector; neither has a gravity field. Spherical
  gravity is our force, applied per body each step, in both.

### Gaps
- Rapier's license: Apache-2.0 per common knowledge, not fetched (unverified). Rust toolchain cost not measured.
- Bullet 3: zlib license and `BT_USE_DOUBLE_PRECISION` from memory (unverified); maintenance status not checked.
- Havok's license terms not fetched; it is proprietary and out by [ADR 0023](../../adr/0023-license-mit.md) anyway.
- No independent benchmark of Box3D vs Jolt found. The Box3D announcement gives none.
- Whether Box3D's heightfield can be edited in place: not found in headers (none seen); treat as "recreate".
- Whether Box3D is deterministic across thread counts: secondary coverage only (unverified); Box2D v3's design is
  ([Box2D determinism](https://box2d.org/posts/2024/08/determinism/)).

## Large worlds, multiple physics worlds, moving and rotating frames

### Takeaway
Double precision fixes position resolution but not the broadphase (float) or the speed caps; every serious precedent and
Jolt's own docs split the universe into several physics worlds, each near its origin and moving slowly: the planet static
in its own (body-fixed) world, fast ships in a co-moving world, ship interiors in their own grid.

### Cited Findings
- Jolt Big Worlds: floats are fine "within roughly 5 km from the origin"; `JPH_DOUBLE_PRECISION` stores positions as
  doubles; queries take a "base offset" and return floats relative to it; "at 10000 km from the origin, the resolution
  of the broad phase is reduced to 1 m"; double mode is "5-10% slower" (a naive all-double build ">2x slower") —
  [Jolt Architecture.md](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)
- Jolt space-sim guidance: "split the universe into multiple PhysicsSystems and keep objects in each PhysicsSystem
  close to the origin and with low velocities"; "A ship that is traveling near light speed can happen in a
  PhysicsSystem that is traveling at near light speed"; "A planet exists in its own PhysicsSystem where it is static.
  Rotation of the planet around its axis or around its sun is not modeled in Jolt but applied as an additional matrix
  transform when rendering"; bodies move between systems via `Body::GetBodyCreationSettings`; "X4 Foundations has
  demonstrated" a space sim on Jolt; "The only object that can be shared between PhysicsSystems is a Shape" —
  [Jolt Architecture.md](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)
- Box3D large worlds: `BOX3D_DOUBLE_PRECISION`, positions `b3Pos` (3 doubles) with float rotation; velocities, shapes,
  solver and broadphase stay float; queries take a caller origin; broadphase bounds quantize to about 1 m at 1e7 m and
  16 m at 1e8 m; "a few percent" cost; double builds are not bit-identical to float builds; "inspired by Jolt" —
  [Box3D large worlds](https://box2d.org/documentation3d/large-worlds.html)
- KSP: Krakensbane shifts the reference frame's origin and velocity so the active vessel stays near the origin and
  moves slowly relative to the physics frame; FloatingOrigin recenters on the point of interest; bodies on rails —
  [KSP API: Krakensbane](https://www.kerbalspaceprogram.com/ksp/api/class_krakensbane.html),
  [KSP API docs mirror: FloatingOrigin](https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html),
  [KSP wiki: Kraken](https://wiki.kerbalspaceprogram.com/wiki/Kraken). Before Krakensbane (v0.17) the "Deep Space Kraken"
  produced phantom forces at high speed — [KSP wiki: Kraken](https://wiki.kerbalspaceprogram.com/wiki/Kraken)
- KSP physics range ~2.5 km for loading vessels into physics: forum only (unverified) —
  [KSP forum](https://forum.kerbalspaceprogram.com/topic/207101-question-on-krakensbane)
- Star Citizen: "Local Physics Grids" attach the player to a grid on the ship; nested grids (system → planet → ship):
  community wiki and forums only (unverified) — [starcitizen.tools BTS: Multicrew & Physics Grids](https://starcitizen.tools/Comm-Link:BTS_-_Multicrew_%26_Physics_Grids)
  (article body is images; the text was not readable)
- Space Engineers: planets do not orbit ("the sun orbits around the solar system instead"); stations built on voxels stay
  static only while touching the voxel; blocks can't be axis-aligned on a sphere — [Marek Rosa, Planets (2015)](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/).
  "Clang" is the community name for phantom forces, mostly pistons, rotors and hinges between grids; sub-grids "would
  disconnect and clang out at high speeds, particularly in space" — [Space Engineers wiki: Clang](https://spaceengineers.wiki.gg/wiki/Clang),
  [Marek Rosa, physics (2017)](https://blog.marekrosa.org/2017/11/physics/)
- Space Engineers caps speed because of "physics stability, bullet-through-paper issues and other problems" —
  [Petrzilka guest post](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/)
- PhysX: `shiftOrigin` only, single precision — see engines section.

### Inferences
- A planet's terrain must be static to be cheap (sleeping islands, no transform updates), so surface physics lives in
  the body-fixed frame (the frame that rotates with the planet). This is the Jolt-recommended layout and matches
  00-scope's "no fictitious forces by default".
- A ship interior in its own physics world is the Jolt and Star Citizen pattern; Space Engineers' shared-world
  grids are the cautionary tale (clang).

### Gaps
- No primary source on Star Citizen's physics grids (the official article is images) or Outer Wilds' origin handling.
- X4's Jolt integration details: none found beyond Jolt's one-line mention.

## Terrain colliders for streamed, editable chunks

### Takeaway
Heightfields can't express caves or overhangs on a sphere; triangle meshes per chunk, built on the CPU from the
authoritative density, are what voxel games ship. SDF collision as a custom shape is possible in Jolt (custom shapes
plus collision dispatch) but is custom solver-facing work. GPU-meshed colliders need non-blocking readback (VCH-073)
and conflict with the determinism contract.

### Cited Findings
- Jolt mesh collision: "a mesh is only considered to be colliding when one of its triangles intersect with the other
  object" (no inside); enhanced internal edges need shared edges in the same shape — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- Jolt custom shapes and `CollisionDispatch` functions are registerable and shared between systems —
  [Jolt Architecture.md](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)
- MeshShape build sped up ~25% in v5.3.0 — [release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html)
- Box3D in The Legend of California: "huge redwood trees that fall fast onto a voxel terrain" (mesh collision + CCD) —
  [Announcing Box3D](https://box2d.org/posts/2026/06/announcing-box3d/)
- Space Engineers planets are "fully destructible, volumetric and persistent" and collision between ships and planets
  needed optimization — [Marek Rosa (2015)](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)
- Veach RHI: readback is "partial"; no non-blocking completion query; "Terrain meshed on the GPU reaching CPU physics
  without stalling needs this" (VCH-073) — [rhi-capabilities.md](../../reference/rhi-capabilities.md)
- 00-scope: "GPU output is visual-only unless produced with integer/fixed math and verified bit-equal on Metal and
  WARP" — [00-scope.md](../00-scope.md)

### Inferences
- Colliders are needed only where a dynamic body can reach within a lead time, at the finest LOD. That is a small
  swept region per active body, not the whole streamed terrain.
- Async cooking must not let thread timing leak into the simulation: a collider for an edit at tick t is installed at a
  fixed later tick, and the step waits if it isn't ready. This keeps replay exact.

### Gaps
- No measured MeshShape cook time per chunk on Apple silicon; must be measured (spike).
- No primary source on how NMS or Astroneer build terrain colliders.

## Gravity per body

### Takeaway
Neither engine has gravity fields; both have one gravity vector per world and a per-body factor. Spherical gravity is a
per-body force from a gravity-source registry, evaluated each step. Across a physics world on a small planet the
gravity direction changes noticeably, so per-world gravity is wrong at small s.

### Cited Findings
- Jolt: `PhysicsSystem::SetGravity` (one vector), `mGravityFactor` per body; step listeners (`PhysicsStepListener`) are
  called from multiple threads — [Jolt Architecture.md](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md),
  [BodyCreationSettings.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Body/BodyCreationSettings.h)
- Jolt v5.1.0 "Added ability to override the gravity vector per vehicle" — [release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html)
- Box3D: `b3World_SetGravity`, `b3Body_SetGravityScale` (header) — [box3d.h](https://github.com/erincatto/box3d/blob/main/include/box3d/box3d.h)
- Space Engineers: planet gravity "will gradually decrease" with distance, "bigger planets will have more gravity" —
  [Petrzilka](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/); artificial gravity
  generator blocks, spherical generators — [ModDB](https://www.moddb.com/news/update-01044-spherical-gravity-generator-modding-for-prefab-and-respawn-ship)
- Outer Wilds: bodies and ship follow an exaggerated Newtonian model with per-body gravity fields; implementation
  details (custom integrator vs Unity physics) unverified — [Vice](https://www.vice.com/en/article/pkpde9/how-outer-wilds-developers-mobius-digital-made-space-physics-fun),
  [Giant Bomb blog](https://www.giantbomb.com/profile/gamer_152/blog/push-and-pull-physics-in-outer-wilds/264418/)

### Inferences (computed)
- With surface gravity g fixed as a creator setting, use μ = g·R² directly (never G·M), so gameplay masses never
  appear: g(r) = μ/r².
- Gravity direction across a 2 km physics region: 2 km / 63.7 km = 1.8° at s = 0.01; 0.18° at s = 0.1; 0.018° at
  s = 1. A single world gravity vector is visibly wrong at small s.
- A 1 km rocky asteroid (ρ = 2000 kg/m³, R = 500 m): g = (4/3)πGρR ≈ 2.8×10⁻⁴ m/s², escape speed ≈ 0.53 m/s. A
  player jump leaves it. Small bodies need a gameplay gravity floor or a "surface gravity" provider.

### Gaps
- No primary source for how Astroneer or NMS compute gravity on small bodies.

## Determinism (cross-platform, FMA, networking, replay)

### Takeaway
Jolt promises same-binary determinism always and cross-platform determinism with `CROSS_PLATFORM_DETERMINISTIC`
(~8% slower), but calls that "less tested"; Box2D's lineage tests it in CI across MSVC/GCC/Clang on x64 and ARM.
Both require FMA contraction off and precise float modes, and callbacks/broadphase queries in Jolt are not ordered.

### Cited Findings
- Jolt: deterministic if APIs that modify the simulation are called "in exactly the same order" and "The same binary
  code is used"; with `CROSS_PLATFORM_DETERMINISTIC` "approximately 8% slower but the simulation will be deterministic
  regardless of" compiler (MSVC2022, clang, gcc, emscripten), configuration, OS (Windows, macOS, Linux), architecture
  (x86, ARM, ...), word size; "you can't have one platform using JPH_DOUBLE_PRECISION and another not"; "Turn off
  floating point contract operations (clang: -ffp-contract=off)"; "Compile your application mode in Precise mode
  (clang: -ffp-model=precise, MSVC: /fp:precise)"; "Make sure the FPU state is consistent across platforms / threads";
  "this feature is less tested than other features" — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- Jolt non-deterministic pieces: `BroadPhaseQuery` results; listeners (`ContactListener`, `PhysicsStepListener`,
  `BodyActivationListener`) "are called from multiple threads", order not deterministic; `GetActiveBodies` order not
  deterministic — [Jolt Architecture.md](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)
- Jolt rollback: `SaveState`/`RestoreState` with `StateRecorder`; "SaveState only records the state that the physics
  engine modifies during its update step"; adding/removing bodies during rolled-back frames must be redone by hand;
  `StateRecorderFilter` to skip static/inactive bodies; the samples' "Check Determinism" rewinds and re-steps every
  frame — [Jolt Architecture.md](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md); API in
  [PhysicsSystem.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/PhysicsSystem.h) (header)
- Jolt v5.6.0 "Fixed an issue that broke cross platform determinism between ARM64 and x64 builds when compiling with
  double precision"; v5.3.0 soft-body race "could break determinism" — [release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html)
- Box2D v3: FMA off (`-ffp-contract=off`), never fast-math, own `atan2f`/`sinf`/`cosf` because `atan2f` differed by
  platform; per-thread bit arrays merged with OR for thread-independent results; CI "Falling Hinges" test on MSVC, GCC,
  Clang, x64 and ARM; Apple M2 matched AMD Ryzen; "Currently Box2D does not provide roll-back determinism" —
  [Box2D determinism (2024)](https://box2d.org/posts/2024/08/determinism/)
- Box3D: double builds are not bit-identical to float builds; DeterminismTest keeps separate expected values —
  [Box3D large worlds](https://box2d.org/documentation3d/large-worlds.html)
- Q6 notes: lockstep's preconditions fail for Veach; whole-world rollback with streamed terrain is unaffordable;
  Overwatch-style prediction of the local player — [06-networking-notes.md](06-networking-notes.md)

### Inferences
- With server authority (Q6), cross-platform physics determinism is not load-bearing for multiplayer correctness. It
  matters for replaying a Mac recording on Windows CI, for the cross-platform hash test 00-scope asks for, and for
  cheap client prediction agreeing with the server most of the time.
- Contact and trigger events must be collected and sorted (by body id pair) before gameplay sees them.

### Gaps
- Whether Jolt is deterministic across worker-thread counts: implied by "same API order + same binary" but not stated
  as such in the fetched text (unverified).

## Stability across scale s and the handover to on-rails orbits

### Takeaway (computed)
At fixed surface g, orbital speed is √(gR) ∝ √s: 790 m/s at s = 0.01, 2.5 km/s at s = 0.1, 7.9 km/s at s = 1, all above
the default speed caps (Jolt 500 m/s, Box3D 400 m/s). Only co-moving physics worlds keep relative speeds low. Surface
rotation is harmless at real day lengths and dangerous with compressed days on large planets.

### Cited Findings
- Default caps: Jolt `mMaxLinearVelocity = 500.0f`; Box3D `maximumLinearSpeed = 400.0f * lengthUnits` (header) —
  [BodyCreationSettings.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Body/BodyCreationSettings.h),
  [box3d types.c](https://github.com/erincatto/box3d/blob/main/src/types.c)
- KSP keeps 9.81 m/s² at about 1/10 radius — [00-scope.md](../00-scope.md) (KSP values unverified there)

### Inferences (computed; R_earth = 6,371 km, g = 9.81 m/s², 1/60 s step)
- Surface orbital speed √(gR): s = 1: 7,906 m/s (132 m per step); s = 0.1: 2,500 m/s (42 m/step); s = 0.01: 790 m/s
  (13 m/step).
- Surface rotation speed ωR with a 24 h day: 463 / 46 / 4.6 m/s. As a fraction of orbital speed: 0.059 / 0.019 /
  0.006. Centrifugal acceleration as a fraction of g is that ratio squared: 0.34% / 0.034% / 0.0035%.
- Coriolis 2ωv at 50 m/s, 24 h day: 0.0073 m/s² (0.07% of g).
- With a 30-minute day (ω = 3.49×10⁻³ rad/s): s = 0.01: ωR = 222 m/s, centrifugal 7.9% of g; s = 0.1: ωR = 2,224
  m/s, centrifugal 79% of g; s = 1: ωR = 22 km/s, faster than orbital speed, the planet would shed its surface.
  Coriolis at 50 m/s: 0.35 m/s² (3.6% of g).
- Handover body-fixed → inertial: v_inertial = v_fixed + ω × r. With a 30-min day at s = 0.1 a ship hovering at the
  equator already has 2.2 km/s inertial speed, 89% of orbital.
- Float ULP at 5 km: 0.49 mm; at 64 km: 7.8 mm; at 637 km: 6.25 cm; at 6,371 km: 0.5 m. (So a body-fixed world with
  its origin at the planet center is broadphase-coarse at s = 1; anchor near the players.)

### Gaps
- No source on how KSP or NMS pick their physics-to-rails handover thresholds (KSP's are moddable "physics range"
  settings; values unverified).

## Precedents summary, hardware cost, testability

### Cited Findings
- NMS: Murray described planets rotating on their axes — [Inverse](https://www.inverse.com/article/19754-no-man-s-sky-physics-explained);
  NMS physics engine and frames: no primary source (unverified).
- Astroneer: UE4, voxel planets — [Unreal spotlight](https://www.unrealengine.com/en-US/spotlights/how-system-era-softworks-leveraged-ue4-to-create-astroneer-s-wonderful-universe);
  its physics (UE4's PhysX at the time) unverified.
- Veach runs physics-free today; ARCHITECTURE: single-threaded, job system decided in VCH-015 —
  [ARCHITECTURE.md](../../ARCHITECTURE.md); replay hashes all Scriptable components —
  [record-replay spec](../../specs/record-replay/spec.md)

### Inferences
- Physics is CPU-only in both candidate libraries (Jolt's GPU compute interface is new and unneeded), so the M1 vs
  M3 Pro difference is core count: base M1 has 4 performance + 4 efficiency cores, M3 Pro 5-6 performance + 6
  efficiency (Apple spec sheets, not fetched here). Collider cooking under fast flight and editing is the part that
  scales with cores.
- Everything here runs headless on the null RHI, so CI can test physics on both platforms, including the
  cross-platform hash.

### Gaps
- No measured Jolt or Box3D step cost on Apple silicon for our workload. The M5 Max is the only machine; an M1-class
  budget can only be approximated by limiting worker count.
