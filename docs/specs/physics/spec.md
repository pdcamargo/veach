# Physics: Jolt behind veach::physics, one scene per frame

**Status:** Implemented
**Area:** physics, runtime
**Owner:** dev lead

## Summary

`engine/physics` (`veach::physics`) is the engine's rigid-body area. Jolt Physics, built double precision and
cross-platform deterministic, is private to it. A `PhysicsWorld` holds **scenes**, each a Jolt `PhysicsSystem` near its
own origin in a reference frame: a body-fixed **surface** region, a ship **interior**, a **free-flight** scene that
follows a fast craft. It steps once per fixed step on `veach::JobSystem`, adds gravity from a hook the toolbox
supplies, installs terrain colliders handed over by the voxel area at t + 2 in chunk-key order, runs character
controllers with any up, and reports contact events in a fixed order. A thin ECS binding and three MCP tools sit on
top. ([ADR 0058](../../adr/0058-physics-jolt.md), amended by [0075](../../adr/0075-amendments-from-sp-11.md),
[0077](../../adr/0077-amendments-from-sp-12.md), [0080](../../adr/0080-amendments-from-sp-13.md),
[0083](../../adr/0083-terrain-colliders-with-an-adjacency-ring.md) and
[0086](../../adr/0086-amendments-from-vch-113.md); [ADR 0065](../../adr/0065-engine-areas-for-the-toolbox.md))

## Motivation

SP-11, SP-12, SP-13 and the seams sweep proved the physics design with Jolt called straight from test code, three
copies of the same setup and a thread pool that may not ship. The first slice (VCH-091) lands on a rotating planet and
digs a hole: terrain streaming (VCH-114) needs a collider-install API, and the slice app (VCH-115) needs scenes,
transfers and characters it can drive. Backlog: VCH-113.

## Requirements

**Area**

- MUST be the static library `veach::physics`, depending on `veach::core` and `veach::ecs` and privately on Jolt;
  no public header names a Jolt type, no file outside `engine/physics/src` includes a Jolt header, and the area never
  links or includes `rhi` or `render` (`check_repo.py`).
- MUST initialize Jolt's process-wide state (allocator hooks, factory, type registry, trace and assert hooks) once,
  the first time any physics code runs (`physics::initialize()`, thread-safe), and keep it until exit.
- MUST run Jolt's jobs on `veach::JobSystem` through a `JobSystemWithBarrier` subclass: Jolt's own thread pool never
  ships. With one thread every job runs on the caller. Results MUST NOT depend on the thread count.
- Physics is replay tier ([ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md)): the same inputs
  in the same order give the same bits; the build flags make them the same on macOS arm64 and Windows x64, and the
  spikes' hashes are checked on both.

**Scenes**

- A scene has a kind (surface, interior, free flight), a parent frame tag and spin (`SceneFrame`), an origin in the
  parent frame (double) and, for free flight, the origin's velocity; its own `PhysicsSystem`, `TempAllocator`, body
  capacity and contact listener. World gravity is zero in every scene.
- A free-flight scene's origin MUST advance in double on exact tick time, `origin + velocity · (tick − origin_tick) /
  60`, so Jolt sees only the residual velocity ([ADR 0080](../../adr/0080-amendments-from-sp-13.md) decision 2).
  `recenter(scene, body)` makes the body's offsets exactly zero.
- `set_origin` MUST re-anchor a scene at a tick boundary, shifting every body (in Jolt body id order) and character
  in double and rebuilding the broad phase; a scene holding terrain colliders re-anchors on a lattice point.
- `transfer(body, scene, motion, options)` MUST move a body between scenes at a tick boundary with its pose and
  velocity given in the destination's parent frame (the caller converts across frames in double, VCH-111), keeping
  its id and settings, optionally changing an empty destination's frame and re-centering it on the body.

**The step**

- `PhysicsWorld::step()` MUST run, in this order: colliders due at this tick (chunk-key order); the gravity hook for
  every awake dynamic body (body id order), plus Coriolis and centrifugal accelerations in a free-flight scene whose
  frame spins ([ADR 0080](../../adr/0080-amendments-from-sp-13.md) decision 1), applied as `a · m` rounded to float32;
  characters (id order); `PhysicsSystem::Update` of every scene that has bodies (scene id order, 1/60 s, one collision
  step); then the contact events of the step.
- The gravity hook (`GravityFn`) MUST be called only for awake dynamic bodies with `gravity = true` and for
  characters: a sleeping body gets no force and keeps sleeping ([ADR 0058](../../adr/0058-physics-jolt.md)). Physics
  never reads G or a celestial body's mass; gameplay or the toolbox's gravity registry supplies g.
- Contact events (added, removed) MUST be sorted by (scene, body, body, sub-shape, sub-shape) with the lower body id
  first, and be identical on any thread count.

**Bodies and characters**

- Bodies are static, kinematic or dynamic, with Jolt's parameters (friction, restitution, damping, max speed, mass
  override, sleeping, locked rotation) and shapes (box, sphere, capsule, offset, triangle mesh, height field). Rotations
  enter Jolt as `dquat` rounded to float32 and normalized; state is read back as Jolt stores it (double position,
  float32 rotation and velocities).
- A character controller MUST work with any up: each step gameplay gives up, rotation and an optional walking
  velocity; the step keeps the velocity along up (or the ground's velocity when standing on it), adds g·dt from the
  hook, and moves the character with sliding, stairs and floor sticking (Jolt's `CharacterVirtual`).

**Terrain colliders** (the interface VCH-114 integrates against)

- A `ColliderMesh` is canonical and in lattice coordinates ([ADR 0074](../../adr/0074-amendments-from-sp-08.md)): a
  `ChunkKey` (level, x, y, z; voxel's key, ordered the same), the chunk's minimum corner and edge in lattice units,
  fixed-point vertices (lattice units × 2^16 for voxel's LOD0 lattice of 0.5 m), indices counter-clockwise seen from
  outside the solid, and optionally one adjacency byte per triangle. A mesh without triangles removes the collider.
- `cook_collider` MUST be thread-safe and pure (the same mesh gives the same shape hash); vertices relative to the
  chunk corner are exact in float32.
- `request_colliders(scene, meshes)` at tick t MUST cook on the job system and install before the step of tick t + 2
  ([ADR 0077](../../adr/0077-amendments-from-sp-12.md) decision 4), all requests due at a tick together in chunk-key
  order, a later request for a chunk winning; if a cook is late the step waits. `install_colliders` installs at once
  (loading a region before its first step).
- An install MUST replace an existing chunk's shape in place (its body and id stay) and wake the bodies over the chunk
  (ADR 0077 decision 5). Terrain scenes sit on a lattice point with the frame's axes (decision 6); a request to a scene
  without a lattice origin fails with `physics.not_on_lattice`.
- `with_adjacency_ring` MUST build a chunk's collider from its own triangles plus every neighbor triangle with a vertex
  on its box, welded by lattice position in first-use order, the neighbors' marked adjacency
  ([ADR 0083](../../adr/0083-terrain-colliders-with-an-adjacency-ring.md)). Contacts with adjacency triangles MUST be
  rejected for rigid bodies and characters alike. A neighbor's remesh recooks the chunks whose ring it feeds in the
  same request (VCH-114's bridge decides which; physics installs what it is given).

**ECS binding and tools**

- `PhysicsModule` MUST own the world's `PhysicsWorld` (`physics_world(world)`), register `PhysicsScene`,
  `PhysicsSceneId`, `RigidBody` and `PhysicsBody` (reflected, Scriptable; ids and parameters only), make a scene for
  each frame entity with a `PhysicsScene` (scene 0 is the root world's) and a body for each `RigidBody` that is a
  direct child of such a frame (or of nothing), at its `Transform`.
- It MUST step once per fixed step in `PostSimulation`, after the frame pose sources registered before it (CoreModule's
  `FrameSpin`, which write the pose for the end of the step, [ADR 0085](../../adr/0085-amendments-from-vch-111.md)) and
  before TransformPropagation, taking each scene's spin from its frame, then write every dynamic and kinematic body's
  pose to its `Transform` for the end of the step. Gameplay writes forces in `Simulation`.
- `physics_scenes`, `physics_bodies` and `physics_contacts` MUST be read-only MCP tools.

## API sketch

```cpp
#include <veach/physics/physics.hpp>
namespace ph = veach::physics;

ph::PhysicsWorld world({.jobs = &jobs});                          // the engine's job system
const ph::SceneId pad = *world.create_scene({.name = "pad", .kind = ph::SceneKind::surface,
    .frame = {.frame = planet.id(), .angular_velocity = {0.0, omega, 0.0}},
    .lattice_origin = voxel::chunk_origin(center)});              // on the LOD0 lattice, the frame's axes
world.set_gravity([&](const ph::GravityQuery& q) { return gravity.at(q.frame, q.position); });

const ph::BodyId crate = *world.add_body(pad, {.shape = ph::Shape::box(math::vec3(0.5f)),
                                                .position = {0.0, 1.0, 0.0}, .mass = 500.0});
world.add_force(crate, thrust);                                    // gameplay, before the step
const ph::CharacterId walker = *world.add_character(pad, {.shape = capsule, .position = feet, .up = up});
world.set_input(walker, {.up = up, .rotation = facing, .move = forward * 5.0});

// Terrain from the voxel bridge (VCH-114): requested at tick t, installed before step t + 2, in chunk-key order.
static_cast<void>(world.request_colliders(pad, {ph::with_adjacency_ring(key, origin, 32, neighborhood)}));

world.step();                                                      // the module does this in PostSimulation
for (const ph::Contact& c : world.contacts()) { /* sorted */ }

// Free flight that follows a craft, and a handoff into it in double (converted by the caller across frames).
static_cast<void>(world.transfer(craft, flight, {.position = r, .velocity = v, .rotation = q},
                                 {.frame = ph::SceneFrame{.frame = planet.id(), .angular_velocity = w},
                                  .recenter = true}));
world.recenter(flight, craft);                                     // when its offsets grow

// ECS: engine.import<veach::PhysicsModule>(); then
planet.set(veach::PhysicsScene{.kind = ph::SceneKind::surface});
world.entity("crate").child_of(planet).set(veach::Transform{...}).set(veach::RigidBody{.mass = 500.0});
```

## Acceptance criteria

- **physics.AC1** — `veach::physics` links core, ecs and Jolt (private); `check_repo.py` fails on a Jolt include
  outside `engine/physics/src` and on an rhi or render dependency in `engine/physics`. The spike code that called Jolt
  directly (`tests/sp11`, `tests/sp12/sp12_physics.*`, `tests/support/jolt_runtime.hpp`, the Jolt parts of SP-13 and
  the seams sweep) is gone.
- **physics.AC2** — SP-11's pile (200 bodies in a bowl, 600 steps) gives SP-11's committed state hashes through the
  engine at the origin (`0x171ce203da644de9` chained) and 6,371 km out (`0x48bceab5bca8cea8`), and the same step
  hashes and Jolt state on 1 and 5 job threads.
- **physics.AC3** — Colliders requested at tick t are absent after the steps of t and t + 1 and installed by the step
  of t + 2, in chunk-key order (their install slots increase with the key), a later request for a chunk wins, a swap
  keeps the body and wakes a sleeping ball that then falls to the new surface, an empty mesh removes the collider, a
  malformed one is `physics.shape_invalid`, a scene off the lattice `physics.not_on_lattice`. SP-12's dig-and-collide
  run reproduces its committed sample, mesh, collider (`0x8927396d27667325`), install (`0x8097f58466b787c5`) and ball
  (`0x25dd581926b6d60f` chained) hashes through the engine on 1 and 4 workers and in reverse order (`[sp12.AC2-4]`).
- **physics.AC4** — Re-anchors, re-centering and transfers convert in double and keep ids; a frame change into a
  scene with other bodies is `physics.invalid_transfer`; fictitious forces apply in rotating free flight only. SP-13's
  walk (`0x16e318bdcca0b65b` at s = 0.01, `0xd9d5140a4fbe07bd` at s = 1) and landing hashes (`0x9e25df28fd4d4a02`,
  `0x5fff33f2b5008ada`, also recorded and replayed through the input path), its |Δv| < 1 mm/s at every handoff and
  its fictitious-force control run reproduce through the engine (`[sp13.AC1-3]`), and the seams sweep's pole landing
  (`0xcd52b59f83d39b34`, `[seams.AC6]`).
- **physics.AC5** — `with_adjacency_ring` welds the ring and marks it; a body over ring triangles alone falls through
  them. The seams sweep's walks (`0xcdb2b78d799d3a05`) and border-edge comparison with rings (`0xa3aba7a8cbc3268d`)
  and its dig at every site (`0x3cfd9dbf51875426`) reproduce through the engine (`[seams.AC2-4]`). Rigid bodies,
  measured: eight boxes sliding over SP-12's terrain kick up at per-chunk border edges (p99 |Δv·up| 0.63 m/s against
  0.33 inside) and not with rings (0.20 at borders, 0.25 inside; merged mesh 0.31); rolling balls show no border
  effect with any collider layout.
- **physics.AC6** — The gravity hook is called once per step for each awake dynamic body with gravity and each
  character, never for a sleeping body, a static body or one with `gravity = false`.
- **physics.AC7** — The pile's contact events are sorted (lower body first) and identical, event for event, on 1 and 5
  job threads.
- **physics.AC8** — A character walks 6 m along a plane whose up is (1, 1, 0)/√2, on the ground and on the plane;
  SP-13's and the seams sweep's walkers (over both poles) reproduce their hashes.
- **physics.AC9** — With `PhysicsModule` imported, a `RigidBody` crate in the root scene falls onto a static slab and a
  ball in a planet frame's `PhysicsScene` falls toward the frame's origin; the module steps once per fixed step and
  writes their `Transform`s; removing `RigidBody` removes the body; a body whose parent is not its frame logs
  `physics.invalid_transfer`.
- **physics.AC10** — `physics_scenes`, `physics_bodies` and `physics_contacts` answer through `McpTools::call`, and an
  unknown scene is `mcp.invalid_arguments`.
- **physics.AC11** — `veach-bench-physics` reports ms per step of the pile per body and thread count on the engine's
  job system, with the Jolt state hash equal on every line of a body count (measured, not a test; see the guide).

## Out of scope

- Shipping the voxel bridge that decides which chunks' rings a remesh dirties, and the swept collider region around
  each dynamic body (VCH-114).
- Frame-aware policy: when to hand off, re-anchor or re-center, handoff bands, the travel governor, the gravity
  registry (VCH-115, the toolbox). Physics supplies the mechanisms.
- Continuous collision detection for fast bodies entering a surface scene ([ADR 0080](../../adr/0080-amendments-from-sp-13.md)).
- Luau bindings for forces and the gravity hook; save and restore of physics state for prediction
  ([ADR 0056](../../adr/0056-server-authoritative-multiplayer.md)); debug draw.

## Open questions

None.

## Links

- ADRs: [0058](../../adr/0058-physics-jolt.md), [0075](../../adr/0075-amendments-from-sp-11.md),
  [0077](../../adr/0077-amendments-from-sp-12.md), [0080](../../adr/0080-amendments-from-sp-13.md),
  [0083](../../adr/0083-terrain-colliders-with-an-adjacency-ring.md), [0086](../../adr/0086-amendments-from-vch-113.md),
  [0065](../../adr/0065-engine-areas-for-the-toolbox.md), [0073](../../adr/0073-job-system-enkits.md)
- Backlog: VCH-113 (this), VCH-114 (terrain streaming), VCH-115 (the slice app)
- Guides: [physics](../../guides/physics.md), [extending](../../guides/extending.md) (m)
- Research: [09-physics](../../research/09-physics.md), [SP-11](../../research/spikes/sp-11-jolt.md),
  [SP-12](../../research/spikes/sp-12-dig-collide.md), [SP-13](../../research/spikes/sp-13-walk-hover-land.md),
  [seams sweep](../../research/spikes/seams-sweep.md)
