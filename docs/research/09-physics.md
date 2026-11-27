# 09: Physics at planetary scale

Which physics engine Veach adopts, and how it is laid out so that it works on a rotating planet, inside a moving ship,
on terrain that streams in and gets dug up, under per-body gravity, at every creator scale s, deterministically.
Raw notes and every source: [notes/09-physics-notes.md](notes/09-physics-notes.md). Numbers marked *(computed)* are
derivations from standard physics; claims marked *(unverified)* have no primary source.

**Short answer.** Adopt **Jolt Physics** (MIT) in its **double-precision, cross-platform-deterministic** build, behind a
generic engine module (`engine/physics`, Jolt private to it like SDL is to `engine/platform`). Lay physics out as
**several small physics scenes, not one big world**: one per occupied region of a body, in the **body-fixed frame** with
no fictitious forces; one per ship interior; one co-moving scene per free-flying ship or cluster. Apply gravity per body
from a **gravity-source registry** that uses the same model as the on-rails orbits. Build terrain colliders **on the
CPU** from the authoritative density, only near dynamic bodies, and install them **at a deterministic tick**. Box3D is
the credible alternative; re-evaluate it at its v1.0.

## 1. The problem in Veach's terms

Veach has no physics today ([ADR 0017](../adr/0017-dependency-set.md): "physics (Box3D or Jolt, a research-phase
question)"). The toolbox needs it for the first slice (land a ship on a rotating planet, walk, dig a hole) and for
everything after (base building, ship building, ship interiors, multiplayer). The constraints it must meet:

- **License.** Permissive and MIT-compatible only ([ADR 0023](../adr/0023-license-mit.md)). Havok is out.
- **Fixed 60 Hz step, determinism, record/replay** ([ADR 0018](../adr/0018-engine-lifecycle-and-phases.md)). Physics
  steps inside the fixed-step phases, reads no wall clock, and a replay must reproduce it bit for bit: the replay hash
  covers every Scriptable component ([record-replay spec](../specs/record-replay/spec.md)). Q12 adds a cross-platform
  contract: macOS arm64 and Windows x64 must agree on anything authoritative.
- **The engine owns the loop and the threads** ([ARCHITECTURE.md](../ARCHITECTURE.md)). Veach is single-threaded
  today; a job system is pending (VCH-015). A physics library that spawns its own threads must instead run on ours.
- **Flecs through the barrel** ([ADR 0005](../adr/0005-ecs-flecs-v4.md), [0006](../adr/0006-flecs-barrel-not-wrapper.md)).
  Physics is components and systems; the library's types never leak to apps, tools or Luau.
- **Conventions** ([ADR 0013](../adr/0013-conventions-and-math.md)): meters, seconds, right-handed, +Y up. But on a
  planet "up" is a function of position, not +Y (00-scope), and `Transform` is float32 today (Q1 changes it).
- **GPU readback is partial** ([rhi-capabilities.md](../reference/rhi-capabilities.md), VCH-073): there is no
  non-blocking way for CPU physics to receive GPU-built terrain meshes. And 00-scope rules that GPU output is
  visual-only unless proven bit-equal on Metal and WARP.
- **Scale.** Physics happens at human scale, which never scales. But s changes the things physics hands over to and
  from: planet radius (curvature, gravity direction across a region), orbital speed √(gR), surface rotation speed ωR,
  entry speed, and how far a fast ship travels per step. The design must work at s = 0.01 and s = 1 with the same code.
- **00-scope's (a) items for Q9**: body-fixed frames without Coriolis or centrifugal terms by default; moving ship
  frames; per-body gravity. Related: Q10 (gravity, flight, handover to orbits), Q12 (numeric determinism).

The numbers that frame the problem *(computed; Earth radius, g = 9.81 m/s² fixed, 24 h day, 1/60 s step)*:

| s | Radius | Orbital speed √(gR) | Distance per step at orbital speed | Surface rotation ωR | Centrifugal / g |
|---|---|---|---|---|---|
| 0.01 | 63.7 km | 790 m/s | 13 m | 4.6 m/s | 0.0035% |
| 0.1 | 637 km | 2,500 m/s | 42 m | 46 m/s | 0.034% |
| 1 | 6,371 km | 7,906 m/s | 132 m | 463 m/s | 0.34% |

Default speed caps are **500 m/s in Jolt** and **400 m/s in Box3D** (from their headers). Every orbital speed above
exceeds them. A float32 position is 0.49 mm precise at 5 km and 0.5 m at 6,371 km. Neither problem is fixed by double
precision alone.

## 2. Options with precedents

### 2.1 The physics library

| Library | License | Large worlds | Determinism | Maturity, today | Verdict |
|---|---|---|---|---|---|
| **Jolt Physics** v5.7.0 (2026-11-14) | MIT | `JPH_DOUBLE_PRECISION`: double positions, float everything else, queries relative to a "base offset", 5-10% slower ([docs](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)) | Same binary: always (v5.7.0: "The simulation is now always deterministic"). `CROSS_PLATFORM_DETERMINISTIC`: ~8% slower, tested across MSVC/Clang/GCC, x86/ARM, but "less tested than other features" | Shipped in Horizon Forbidden West and Death Stranding 2 ([README](https://github.com/jrouwe/JoltPhysics)); X4 Foundations runs a space sim on it; Godot and Unreal integrations | **Adopt** |
| **Box3D** v0.1.0 (2026-06-30) | MIT | `BOX3D_DOUBLE_PRECISION`: `b3Pos` doubles, float solver and broadphase, query origin, "a few percent" cost ([docs](https://box2d.org/documentation3d/large-worlds.html)) | Cross-platform determinism is a headline feature; Box2D v3's method is CI-tested on x64 and ARM ([Catto](https://box2d.org/posts/2024/08/determinism/)). Built-in recording with state hashes and a seekable player | "I still consider Box3D to be alpha software" ([announcement](https://box2d.org/posts/2026/06/announcing-box3d/)); manual is a work in progress; used by The Legend of California, s&box, Glenn Fiedler's 1000-player space game | **Re-evaluate at v1.0** |
| PhysX 5 | BSD-3 | Single precision only; `shiftOrigin` ([API](https://nvidia-omniverse.github.io/PhysX/physx/5.5.0/_api_build/classPxScene.html), [NVIDIA forum](https://forums.developer.nvidia.com/t/simulation-position-precision/245493)) | Same-platform at best *(unverified)*; GPU features CUDA-only, irrelevant on Mac | Mature, large | Reject: no doubles, heavy |
| Bullet 3 | zlib *(unverified)* | Double build exists *(unverified)* | No cross-platform claim found | Maintenance slowed *(unverified)* | Reject |
| Havok | Proprietary | Used by Space Engineers ([Keen](https://keenswh.com/vrage)) | — | Mature | Out by ADR 0023 |
| Rapier | Apache-2.0 *(unverified)* | `rapier3d-f64` crate | `enhanced-determinism`: cross-platform on strict IEEE 754 platforms ([docs](https://rapier.rs/docs/user_guides/rust/determinism)) | Mature in Rust | Reject: a Rust toolchain in every build and FFI across the ECS boundary |
| Our own | MIT | Anything | Anything | Nothing | Reject: years of solver work, no |

Jolt and Box3D are close on paper. Both: MIT, double positions with float internals, per-body gravity scale, one
world gravity vector, triangle meshes and height fields, a character controller, task hooks for our job system.
The differences that decide it:

- **Maturity and API stability.** Jolt is at v5.7 with two AAA titles shipped. Box3D is five months old, alpha by its
  author's word, and its manual and contribution model (a CLA) are not settled. Integrating an engine module against
  an alpha API means rework at each breaking release.
- **Things Veach needs that only Jolt has today:** in-place height edits (`HeightFieldShape::SetHeights`), mutable
  compounds, a mature `CharacterVirtual` with an arbitrary up vector (`SetUp`), rollback via
  `SaveState`/`RestoreState` (Q6 client prediction), custom shapes with registerable collision dispatch (the door to SDF
  collision later), and explicit guidance for space games: one `PhysicsSystem` per planet and per fast ship.
- **Things Box3D does better:** determinism is the core design goal of its lineage, tested across thread counts and
  platforms; recording and replay with embedded state hashes is built in, which maps directly onto our `--record`; it is
  C17 (faster to compile, easier to wrap). Its author ships a large-world multiplayer space game on it, the closest
  precedent to Veach there is.

### 2.2 Large-world layout: one world, or many small ones

| Layout | What it is | Precedent |
|---|---|---|
| **A. One double-precision world per star system** | Every body in system coordinates | Nobody. Jolt's broadphase is 1 m coarse at 10⁷ m; Box3D's 16 m at 10⁸ m; 1 AU is 1.5×10¹¹ m |
| **B. One float world, re-centered** | Floating origin plus a velocity frame (Krakensbane), everything far away on rails | KSP ([Krakensbane](https://www.kerbalspaceprogram.com/ksp/api/class_krakensbane.html), [FloatingOrigin](https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html), [Kraken](https://wiki.kerbalspaceprogram.com/wiki/Kraken)) |
| **C. Several scenes, each in its own frame, near its origin, moving slowly** | Planet surface in a body-fixed scene where terrain is static; fast ships in a co-moving scene; interiors in a ship scene; bodies move between scenes | Jolt's own space-sim guidance ([Architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)); Star Citizen's local physics grids *(unverified, community sources)*; X4 on Jolt |
| D. One shared world, ships as moving platforms | Characters stand on moving ship bodies by friction | Space Engineers grids ([clang](https://spaceengineers.wiki.gg/wiki/Clang): phantom forces, sub-grids that "clang out at high speeds, particularly in space", [Rosa](https://blog.marekrosa.org/2017/11/physics/)) |

A breaks on broadphase precision and speed caps. B works for one player but a single origin can't serve two players
on opposite sides of a planet, or a server. D is what produces clang. **C is the answer**, with double precision inside
each scene so a scene can be tens of km across and re-anchoring is rare, not a per-frame chore.

### 2.3 Rotating planets

| Option | What it is | Cost |
|---|---|---|
| **Body-fixed scene, no fictitious forces** | The scene rotates with the planet. Terrain, bases and sleeping objects are static. The planet's rotation is applied as a frame transform for rendering and handover | Jolt docs recommend it: objects on the planet "are completely static so that there is no constant overhead" |
| Body-fixed with Coriolis and centrifugal forces | Same, plus −2ω×v − ω×(ω×r) per body per step | Small: two cross products per body. Needed only when rotation is fast relative to orbital speed (below) |
| Inertial scene | The planet's terrain spins as a kinematic body | Every terrain collider moves every step, no sleeping, contact jitter. Nobody does this |

How big the ignored terms are *(computed)*: with a 24 h day, centrifugal is under 0.34% of g at every s and Coriolis
under 0.1% of g at walking and driving speeds. Invisible. With a **30-minute day** (NMS-style compressed time) the
picture changes: centrifugal is 7.9% of g at s = 0.01, 79% at s = 0.1, and at s = 1 the surface moves at 22 km/s,
faster than orbit, so the planet could not hold together. The danger is not the ignored forces; it is the handover
(2.6): leaving the body-fixed scene adds ω×r to the ship's velocity.

### 2.4 Terrain colliders for streamed, editable chunks

| Option | Caves and overhangs | Edit cost | Determinism | Precedent |
|---|---|---|---|---|
| Heightfield per chunk (`HeightFieldShape`, in-place `SetHeights`) | No: a heightfield is one height per (x, z) in a local tangent patch | Lowest | CPU, deterministic | Flat-world games; not voxel planets |
| **Triangle mesh per chunk, meshed on the CPU from the authoritative density** | Yes | Re-mesh plus BVH build of each touched chunk (and seam neighbors) | Deterministic if the density math follows Q12 | Voxel terrain in The Legend of California on Box3D ([Catto](https://box2d.org/posts/2026/06/announcing-box3d/)); Space Engineers' destructible planets ([Rosa](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)) |
| SDF collision: a custom shape that queries the density directly | Yes | Zero cooking: an edit is live at once | Deterministic on the CPU | No shipped precedent found; Jolt allows custom shapes and collision dispatch |
| GPU-meshed collider read back to the CPU | Yes | Free if the renderer already meshes it | Not deterministic across Metal and WARP unless integer-only; needs VCH-073 (and VCH-075 for integer meshing) | None found |

Mesh colliders built on the CPU win now: they need no RHI work, they follow the determinism contract, and both
libraries are optimized for static meshes. The cost is duplicating the meshing algorithm (Q4 may mesh in Slang for
rendering, physics meshes in C++) and a small visual-versus-collision mismatch at coarse render LODs. Colliders exist
only at the finest LOD, only where a dynamic body can reach within a lead time: a swept volume along each body's
velocity, not a disk of the whole streamed area.

**Deterministic installs.** Cooking runs on worker threads, so its finish time varies. If "install when ready" decided
the tick a collider appears, a replay would diverge. Rule: a collider requested at tick t (by an edit or by streaming)
is installed at tick t + K, in chunk-id order; if it isn't ready, the step waits. On slow machines this shows up as a
stall, never as a different simulation.

### 2.5 Gravity

Both libraries have one gravity vector per world and a per-body factor. Neither has gravity fields. Options:

- **World gravity per scene** (Jolt `SetGravity`): wrong at small s. Gravity direction turns 1.8° across a 2 km scene
  at s = 0.01 *(computed)*: crates slide, towers lean.
- **Per-body force from a gravity-source registry** (world gravity zero, force added before each step): correct at
  every s, a few multiply-adds per body. Sources: point mass with μ = g_surface·R² (never G·M, so gameplay masses never
  appear), uniform volume (ship artificial gravity, [Space Engineers generators](https://www.moddb.com/news/update-01044-spherical-gravity-generator-modding-for-prefab-and-respawn-ship)),
  and room for more (irregular bodies). Precedents: Space Engineers' falloff with distance ([Petrzilka](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/)),
  Outer Wilds' per-body fields ([Vice](https://www.vice.com/en/article/pkpde9/how-outer-wilds-developers-mobius-digital-made-space-physics-fun);
  its implementation *unverified*).
- **Which bodies contribute.** Summing every body (n-body) makes the physics trajectory differ from the on-rails
  patched-conic trajectory Q2 propagates, so going on and off rails would jump. Use the **dominant body by sphere of
  influence**, the same rule as Q2's on-rails model.
- **Small bodies.** A 1 km rocky asteroid has g ≈ 2.8×10⁻⁴ m/s² and an escape speed of 0.53 m/s *(computed)*: a jump
  leaves it. Physics should take a creator-set gameplay gravity floor for small bodies, and leave "gravity toward the
  nearest surface" (walkable irregular asteroids) to a module.

### 2.6 Ships, interiors and the handover to orbits

- **Ship interiors** get their own scene in the ship's frame: hull as static geometry, crew and cargo dynamic, gravity
  from the ship's artificial-gravity sources. The ship's acceleration is not fed in by default ("inertial dampers"),
  so a 5 g burn doesn't throw the crew at the wall. Crossing an airlock moves a body between scenes with a frame
  transform (Jolt: `GetBodyCreationSettings` into the other system; shapes are shared). This is Star Citizen's
  model *(unverified)* and Jolt's recommendation, and it avoids Space Engineers' clang.
- **Free flight** (above the atmosphere, or anywhere away from terrain): the ship lives in a co-moving scene whose
  origin and velocity follow the ship (Krakensbane), so contacts and constraints see low relative speeds at 7.9 km/s.
- **On rails.** An unpowered ship with nothing to collide with leaves physics entirely and Q2 propagates it as a
  Kepler orbit. Same gravity model in and out, so the handover is continuous.
- **Body-fixed ↔ inertial.** At the atmosphere top (the atmosphere co-rotates, so inside it the body-fixed frame is
  the natural one: still air is zero velocity), position and velocity transform with v_inertial = v_fixed + ω×r.
  With a real day this adds up to 463 m/s at s = 1. With a 30-minute day at s = 0.1 a ship hovering at the equator is
  already at 89% of orbital speed *(computed)*. Correct physics, surprising gameplay: day length and radius must be
  validated together (see product decisions).

## 3. Tradeoffs

| | Jolt | Box3D |
|---|---|---|
| Complexity | C++17, large; we use a subset. Custom `JobSystem` on top of VCH-015 | C17, smaller; task callbacks; API still moving |
| Performance | Double mode 5-10% and cross-platform mode ~8% slower than its fastest build; v5.6 brought up to 40% speedups | Double mode "a few percent"; no published benchmark against Jolt |
| Determinism | Same-binary always; cross-platform by option, "less tested"; an ARM64/x64 double-precision bug fixed only in v5.6 | Cross-platform is the design goal and CI-tested in the Box2D lineage; recording with state hashes built in |
| Extensibility | Custom shapes, collision dispatch, constraints, step listeners, vehicles, soft bodies | Fewer hooks today |
| Testability | Headless, CPU-only; `StateRecorder` validates determinism per step | Headless; `b3ValidateReplay` |
| Scale range | Doubles plus multiple systems; broadphase coarse beyond ~10⁷ m | Same, ~10⁷ to 10⁸ m |

| Layout choice | Complexity | Performance | Determinism | Scale range |
|---|---|---|---|---|
| Many scenes (recommended) | Frame transforms at handovers, moving bodies between scenes | Static terrain sleeps; each scene small | Moves between scenes must happen at fixed ticks in a fixed order | Independent of s inside a scene; s only moves handover thresholds |
| One re-centered float world | Simple for one player | Fine | Re-centering at variable times breaks replay unless tick-scheduled | One player only |
| Body-fixed without fictitious forces | Simplest | Best | — | Valid unless days are short relative to √(R/g) |
| CPU mesh colliders, tick-scheduled | Two meshers (C++ and maybe Slang) | Cooking scales with cores | Exact | Chunk size is meters, never scaled |
| SDF collider shape | Custom contact generation, CCD and character casts | No cooking | Exact | Same |

## 4. Extension points

| What | Where | How |
|---|---|---|
| Physics components, raycasts and shape casts, contact and trigger events (sorted), materials | **Luau** | Scriptable components (`RigidBody`, `Collider`, `Velocity`, `Force`), `@veach` query functions; events delivered after the step in sorted order |
| Gravity-source parameters, artificial gravity volumes, small-body gravity floor | **Luau** (data) | Components and data records; evaluation stays in C++ (it runs per body per step, on workers) |
| Flight models (Newtonian, assisted, arcade) | **C++ module**, tunable from Luau | Systems in Simulation writing `Force`/`Torque`; Q10 owns the seam |
| New gravity-source kinds (nearest-surface, toroidal habitats) | **C++ module** | Register into the gravity-source registry: a pure, thread-safe function of position |
| New collider providers (SDF shape, gas-giant crush volume, procedural props) | **C++ module** | Register a provider keyed by body kind; the engine module accepts custom shapes |
| Vehicles, constraints, ragdolls | **C++ module** | Thin components over the library's features |
| Frame tree, scene placement and handover policy, terrain collider ring | **Toolbox core** | Space-specific; lives in `space/` ([ADR 0042](../adr/0042-space-toolbox-layering.md)) |
| Physics module, scenes, stepping, debug draw, MCP tools | **Engine** | Generic, so it moves down into `engine/physics` (ADR 0042: "Generic needs move down") |

MCP: physics components are inspectable through the existing `get_component`/`query_entities` from the start.
Planned additions when they're needed, not now: a `physics_query` tool (ray and shape casts) and per-scene stats
(bodies, active islands, step time). Debug draw goes through extraction like everything else the renderer reads.

## 5. Interactions with the other questions

- **Q1 coordinates.** A physics scene is (frame, anchor in that frame). Positions arrive as Q1's double frame
  positions minus the anchor. Q1's notes already treat local physics frames as frames in the tree. Physics needs
  Q1 to replace float32 `Transform` and Luau's float `vector` for authoritative positions.
- **Q2 mechanics and time.** The handover to on-rails uses Q2's dominant-body rule and its tick clock. ω for each body
  comes from Q2's rotation model.
- **Q3 pipeline.** μ = g·R² and the small-body gravity floor are derived properties of a body record.
- **Q4 terrain.** Physics needs a CPU path from density to finest-LOD mesh, chunk ids for ordering, edit deltas in
  meters, and seam handling between neighbor chunks (enhanced internal edges only work within one shape, so seams
  need care). VCH-073 is **not** on physics' critical path under this design.
- **Q5 procgen graph.** Any procgen output that physics collides with must come from the deterministic CPU path.
- **Q6 networking.** Server-authoritative with client prediction (Q6 notes) means physics doesn't need lockstep.
  Jolt's `SaveState`/`RestoreState` supports rewinding the local player's prediction; whole-world rollback is not
  affordable and not planned. Scenes map naturally to interest regions; transforms go on the wire frame-relative.
- **Q7 simulation LOD.** Nothing off-screen runs physics. Objects far from any scene are frozen or on rails;
  catch-up places them at rest.
- **Q8 renderer.** The renderer never reads physics; debug draw is extracted. The planet's rotation is a render
  transform of the body-fixed frame.
- **Q10 gravity, flight, travel.** Speed limits near terrain must respect collider lead distance (132 m per step at
  s = 1 orbital speed). Entry drag uses the shared atmosphere record. Q10 decides the handover altitude and the
  flight-model seam this module provides.
- **Q11 persistence.** Saves store physics state as components (position, velocity, sleeping) in frame-relative
  coordinates, never library binary state (Jolt warns it "is very likely to change between versions"). A recording
  that starts mid-session needs a physics snapshot in its keyframe.
- **Q12 determinism.** Requires `-ffp-contract=off` and precise float modes for the physics target and for any of our
  code that feeds it (forces, gravity, flight models), engine-provided transcendentals, sorted callbacks, and a CI hash
  test on macOS arm64 vs Windows x64.

## 6. M1-class vs M3 Pro-class

Physics is CPU work in both candidate libraries; the GPU class is irrelevant to it. What differs is cores: a base M1
has 4 performance and 4 efficiency cores; an M3 Pro has 5-6 performance and 6 efficiency cores (Apple spec sheets, not
fetched here).

- **Stepping** a few hundred active bodies plus a few characters is small on either *(unverified, to be measured)*.
  Sleeping terrain and bases cost nearly nothing in body-fixed scenes.
- **Collider cooking** is the part that scales: fast low flight streams new collider chunks, and terraforming
  re-meshes chunks under the brush. On an M1-class CPU with fewer workers, either the lead distance K (ticks between
  request and install) grows, or the safe approach speed near terrain drops, or the step stalls. The owner should know
  that this is a gameplay-visible difference, not just a frame-rate one.
- **Multiplayer servers** run many scenes; that budget is per server machine and out of the client hardware target.
- **Measuring.** Only the M5 Max is available. An M1-class budget can only be approximated by capping the worker
  count at 4, which ignores the M1's lower per-core speed. Mark any such number as an approximation.

## 7. Recommendation and what would change my mind

1. **Jolt Physics, pinned, built with `JPH_DOUBLE_PRECISION` and `CROSS_PLATFORM_DETERMINISTIC` on every platform,**
   compiled with `-ffp-contract=off`/precise float modes, in a generic engine module. Jolt types stay private to
   `engine/physics`. Start on `JobSystemSingleThreaded`, then a `JobSystemWithBarrier` on top of the VCH-015 job system.
2. **Many small scenes:** body-fixed surface scenes (anchored near their occupants, re-anchored rarely at fixed ticks),
   ship-interior scenes, co-moving free-flight scenes; bodies move between them at fixed ticks in a fixed order.
3. **No Coriolis or centrifugal forces by default.** The frame transform at handover is exact.
4. **Per-body gravity** from a registry, dominant body by sphere of influence, μ = g·R², same model as on-rails.
5. **CPU-meshed mesh colliders** at the finest LOD in a swept region around dynamic bodies, installed at tick t + K.
6. **Physics steps once per fixed step** in PostSimulation, after gameplay writes forces in Simulation and before
   TransformPropagation. Contact events are sorted before gameplay sees them.

**Would change my mind:**

- **To Box3D:** it reaches a stable v1.0 with a published API policy, and the bake-off spike (S5) shows Jolt's
  cross-platform build diverging between the macOS gate and Windows CI where Box3D doesn't, or Box3D cooking and
  stepping our terrain workload clearly faster. Its determinism-first design and built-in replay fit Veach well; the
  engine-module seam keeps the switch possible while little gameplay depends on the library's feel.
- **To SDF colliders:** S2 shows mesh cooking can't keep up with terraforming or fast low flight on a 4-worker budget.
- **To GPU-built colliders:** VCH-073 and VCH-075 land and integer-only meshing is proven bit-equal on Metal and WARP.
- **To modeling fictitious forces:** the owner wants short days on large planets (product decision 2).

## 8. What is expensive to undo

- **The library, once gameplay is tuned to it.** Character feel, friction, restitution and vehicle behavior differ
  between engines; every game built on the toolbox inherits them. Cheap now, expensive after the first games.
- **Double precision as ABI.** Jolt can't mix double and non-double builds across platforms; serialized state and
  replay files depend on it. Decide once.
- **The scene layout.** Body-fixed scenes and moving bodies between scenes shape networking (Q6), saves (Q11) and the
  frame tree (Q1). Moving to one big world later is a rewrite of all three.
- **No fictitious forces.** Gameplay and creator content (ballistics, sports, vehicle tuning) will be tuned without
  them. Adding them later changes every existing behavior at once.
- **Tick-scheduled collider installs.** If async installs ever run "when ready", every replay and every cross-platform
  test becomes flaky, and finding the cause is costly. Bake the rule in from the first collider.

## 9. Proposed ADR titles

- **Physics engine: Jolt Physics, double precision, cross-platform deterministic build** (amends 0017's
  "not yet added").
- **Physics scenes: one per body-fixed region, ship interior and free-flying ship, with tick-scheduled transfers.**
- **Rotating bodies: surface physics in the body-fixed frame, no fictitious forces by default.**
- **Gravity sources: a per-body gravity registry sharing the on-rails dominant-body model** (joint with Q2/Q10).
- **Terrain colliders: CPU-meshed from authoritative density, finest LOD only, installed at a deterministic tick**
  (joint with Q4).
- The VCH-015 job-system ADR must name Jolt's `JobSystem` interface as a consumer (no separate ADR).

## 10. Spike candidates

Each is a throwaway prototype, sized by scope. All run headless on the null RHI except where a screenshot is the
criterion.

| Id | Smallest prototype | Visible success criterion | Scope |
|---|---|---|---|
| **S1: Jolt in, deterministic** | Pin Jolt (double, cross-platform) in CMake; one headless test: 1,000 boxes and spheres falling onto a static mesh, 600 steps | The state hash after every step is identical on the macOS arm64 gate and Windows x64 CI, and identical with 1 and N workers. Tracy shows the step zone | One dependency, one test, one CI comparison |
| **S2: Dig a hole, fall in** | CPU surface-nets mesher for one 3×3×3 block of chunks from a noise density; Jolt `MeshShape` per chunk; a spherical brush edit at a fixed rate; a ball resting on the surface | The ball drops into the dug hole without tunneling or hanging in mid-air; Tracy shows mesh + cook per chunk at 4 and 16 workers; replay hash is the same with 1 and 16 workers (proves t + K installs) | One mesher, one collider provider, one test |
| **S3: Rotating planet and handover** | A sphere body with a body-fixed scene at s = 0.01 and s = 1 (config switch); drop a crate; launch a ship straight up, hand it to a co-moving scene at the atmosphere top, then to a Kepler propagator | Crate rests without drift on both scales; the ship's inertial trajectory is continuous across both handovers (position jump below 1 m, plotted); a 30-minute-day config shows the ω×r velocity jump the product decision is about | One body, three frame transitions |
| **S4: Walking in a flying ship** | Ship body in the planet scene, its interior as a second Jolt system with artificial gravity, a `CharacterVirtual` crew member; ship accelerates at 5 g and rolls at orbital speed | Crates and the character stay put in screenshots at 7.9 km/s; the character walks out of the airlock into the planet scene and lands | Two scenes, one transfer |
| **S5: Box3D bake-off** (optional, before the engine ADR is final) | S1 and S2 rebuilt on Box3D v0.1.x | Same criteria; a side-by-side table of step and cook cost and the cross-platform hash result | Two spikes repeated |

S1 and S2 are on the critical path to the first slice (land, dig). S3 belongs with Q2/Q10's handover spike. S4 can
come after the slice.

## 11. Product decisions surfaced

1. **Fictitious forces.** Recommendation: **off, and not a creator setting yet.** At real day lengths they are under
   0.4% of g at every scale. Offer them only if a creator asks for spinning stations or fast-spinning worlds.
2. **Day length vs radius.** Short days on big planets make the handover to orbit violent and, past a limit, make the
   planet physically impossible. Recommendation: the creator tools **warn when the surface rotation speed exceeds 30%
   of surface orbital speed** (centrifugal above 9% of g) and **refuse above 70%**. Without a warning that allows days down to
   about 28 min at s = 0.01, 1.5 h at s = 0.1 and 4.7 h at s = 1 *(computed: T = 2π / (0.3·√(g/R)))*. Alternatively days can stay compressed if the
   planet's rotation is only visual and physics treats the body as non-rotating: cheaper, but the sky and the orbit
   disagree.
3. **Small-body gravity.** Recommendation: **a gameplay floor for walkable small bodies** (creator setting, default
   0.05 g) instead of physical values that let a jump reach escape speed.
4. **Ship interiors in the first slice.** Recommendation: **out of the slice**, but the scene model is in the ADRs now.
5. **Collisions at orbital speed.** Two ships meeting at km/s relative speed: recommendation, **treat as destruction**
   (resolved by a gameplay rule, not the solver), so the solver never needs caps above a few hundred m/s.
6. **Safe approach speed near terrain** follows from collider lead distance and the hardware target. Recommendation:
   Q10 sets it per scale and per hardware tier from S2's numbers, not by hand.

## 12. Sources

Engines and libraries:
- Jolt Physics README: https://github.com/jrouwe/JoltPhysics
- Jolt documentation (Big Worlds, Deterministic Simulation, multiple PhysicsSystems): https://jrouwe.github.io/JoltPhysics/ and https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md
- Jolt release notes: https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html
- Jolt releases (v5.7.0, 2026-11-14): https://github.com/jrouwe/JoltPhysics/releases
- Jolt headers: https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Body/BodyCreationSettings.h, https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Collision/Shape/HeightFieldShape.h, https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/PhysicsSystem.h, https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Character/CharacterBase.h
- Box3D repository and releases (v0.1.0, 2026-06-30): https://github.com/erincatto/box3d, https://github.com/erincatto/box3d/releases
- Box3D announcement: https://box2d.org/posts/2026/06/announcing-box3d/
- Box3D documentation and large worlds: https://box2d.org/documentation3d/, https://box2d.org/documentation3d/large-worlds.html
- Box3D headers: https://github.com/erincatto/box3d/blob/main/include/box3d/box3d.h, https://github.com/erincatto/box3d/blob/main/include/box3d/types.h, https://github.com/erincatto/box3d/blob/main/src/types.c
- Kintsugiyama on Box3D: https://www.kintsugiyama.com/updates/box3d-is-now-open-source
- Erin Catto, Determinism (Box2D v3): https://box2d.org/posts/2024/08/determinism/
- PhysX 5 PxScene: https://nvidia-omniverse.github.io/PhysX/physx/5.5.0/_api_build/classPxScene.html
- NVIDIA forums on PhysX precision: https://forums.developer.nvidia.com/t/simulation-position-precision/245493, https://forums.developer.nvidia.com/t/can-i-change-pxreal-to-use-double-precision-in-the-free-version-of-physx/31210
- Rapier determinism: https://rapier.rs/docs/user_guides/rust/determinism; getting started (f64): https://rapier.rs/docs/user_guides/rust/getting_started/

Precedents:
- KSP Krakensbane: https://www.kerbalspaceprogram.com/ksp/api/class_krakensbane.html
- KSP FloatingOrigin (API mirror): https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html
- KSP wiki, Kraken: https://wiki.kerbalspaceprogram.com/wiki/Kraken
- KSP forum on physics range (unverified figures): https://forum.kerbalspaceprogram.com/topic/207101-question-on-krakensbane
- Star Citizen, Multicrew & Physics Grids (community wiki copy): https://starcitizen.tools/Comm-Link:BTS_-_Multicrew_%26_Physics_Grids
- Space Engineers planets (Marek Rosa, 2015): https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/
- Space Engineers physics overhaul (Marek Rosa, 2017): https://blog.marekrosa.org/2017/11/physics/
- Space Engineers gravity (Ondřej Petržílka): https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/
- Space Engineers clang: https://spaceengineers.wiki.gg/wiki/Clang
- Keen VRAGE (Havok): https://keenswh.com/vrage
- Space Engineers gravity generators: https://www.moddb.com/news/update-01044-spherical-gravity-generator-modding-for-prefab-and-respawn-ship
- Outer Wilds (Vice): https://www.vice.com/en/article/pkpde9/how-outer-wilds-developers-mobius-digital-made-space-physics-fun
- Outer Wilds physics (Giant Bomb blog): https://www.giantbomb.com/profile/gamer_152/blog/push-and-pull-physics-in-outer-wilds/264418/
- No Man's Sky (Inverse): https://www.inverse.com/article/19754-no-man-s-sky-physics-explained
- Astroneer (Unreal spotlight): https://www.unrealengine.com/en-US/spotlights/how-system-era-softworks-leveraged-ue4-to-create-astroneer-s-wonderful-universe

Veach:
- [ADR 0017](../adr/0017-dependency-set.md), [0018](../adr/0018-engine-lifecycle-and-phases.md), [0023](../adr/0023-license-mit.md), [0005](../adr/0005-ecs-flecs-v4.md), [0006](../adr/0006-flecs-barrel-not-wrapper.md), [0013](../adr/0013-conventions-and-math.md), [0042](../adr/0042-space-toolbox-layering.md)
- [rhi-capabilities.md](../reference/rhi-capabilities.md), [record-replay spec](../specs/record-replay/spec.md), [00-scope.md](00-scope.md), [Q6 notes](notes/06-networking-notes.md), [Q1 notes](notes/01-coordinates-notes.md)
