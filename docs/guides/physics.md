# Physics

`engine/physics` (`veach::physics`, [spec](../specs/physics/spec.md)) runs rigid bodies, characters and terrain
colliders on Jolt Physics, one scene per reference frame ([ADR 0058](../adr/0058-physics-jolt.md) and its amendments
0075, 0077, 0080, 0083, 0086). Jolt is private: include `<veach/physics/physics.hpp>` (the core) or
`<veach/physics/module.hpp>` (the ECS binding), never a Jolt header (`check_repo.py` rejects one outside
`engine/physics/src`).

## Scenes

A `PhysicsWorld` holds scenes. Each is its own Jolt `PhysicsSystem`, near its own origin in a parent reference frame:

| Kind | What lives there | Origin | Fictitious forces |
|---|---|---|---|
| `surface` | terrain colliders, bases, crates, walking characters, landed craft | a lattice point in the body-fixed frame, re-anchored rarely | never ([ADR 0058](../adr/0058-physics-jolt.md)) |
| `interior` | the inside of a ship, in the ship's frame | the ship's frame | never (inertial dampers) |
| `free_flight` | one fast craft away from terrain | moves with the craft: `origin + velocity · (tick − origin_tick) / 60` | Coriolis and centrifugal when its frame spins ([ADR 0080](../adr/0080-amendments-from-sp-13.md)) |

Positions are double everywhere. A body's `state()` is in scene coordinates as Jolt stores it (double position,
float32 rotation and velocities, velocities relative to the scene's moving origin); `motion()` is the same body in
the scene's parent frame, in double. `SceneFrame{frame, angular_velocity}` tags the frame (an entity id; physics never
reads it) and gives its spin in its own axes.

```cpp
veach::physics::PhysicsWorld world({.jobs = &jobs});
const auto pad = *world.create_scene({.name = "pad", .frame = {.frame = planet.id(), .angular_velocity = w},
                                      .lattice_origin = voxel::chunk_origin(center)});
const auto flight = *world.create_scene({.name = "flight", .kind = SceneKind::free_flight,
                                         .frame = {.frame = system.id()}});
```

Moving things at tick boundaries:

- `set_origin(scene, origin)` re-anchors a scene: bodies and characters shift in double, velocities stay. Terrain
  scenes re-anchor on a lattice point (`set_origin(scene, LatticePoint)`).
- `recenter(flight, craft)` makes the craft's offsets exactly zero (SP-13 did it past 256 m or 4 m/s).
- `transfer(body, scene, motion, {.frame, .recenter})` moves a body into another scene with its pose and velocity in
  that scene's parent frame. Converting between frames is the caller's (`veach::handoff`, `frame_pose`, in double);
  physics keeps the body's id and settings. A frame change needs an empty destination (a free-flight scene that
  follows one craft through a handoff altitude).

When to do any of these is toolbox policy ([ADR 0080](../adr/0080-amendments-from-sp-13.md) decision 3): the engine
gives the mechanisms.

## The step

`world.step()` advances one fixed step of 1/60 s (the `PhysicsModule` calls it in PostSimulation):

1. terrain colliders due at this tick, in chunk-key order, waking the bodies over swapped chunks;
2. the gravity hook for every awake dynamic body, in body id order, plus fictitious accelerations in rotating free
   flight; the force is `a · m` rounded to float32;
3. characters, in id order;
4. `PhysicsSystem::Update` of each scene with bodies, in scene id order, on `veach::JobSystem`;
5. contact events, sorted by (scene, body, body, sub-shape, sub-shape): `world.contacts()`.

Gameplay writes forces (`add_force`, `add_impulse`, `set_rotation`) between steps; they reach Jolt immediately and
never wake a body.

## Gravity

World gravity is zero. The hook returns the acceleration at a body or character, in the parent frame's axes; the
toolbox's gravity registry (the dominant body, μ = g R², artificial-gravity volumes) is one such function:

```cpp
world.set_gravity([&](const veach::physics::GravityQuery& q) {
    return gravity_registry.acceleration(q.frame, q.position);   // m/s²
});
```

It runs on the main thread, only for awake dynamic bodies with `gravity = true` and for characters: a sleeping pile
gets nothing and stays asleep. Without a hook nothing falls.

## Bodies and characters

`add_body(scene, BodySettings{...})` takes a `Shape` (box, sphere, capsule, `offset`, `mesh`, `height_field`; shared
by reference count), a motion type, position and rotation in scene coordinates and Jolt's material parameters. A
`mass` override computes the inertia from the shape; without one the shape's volume at 1000 kg/m³ decides.

A character is Jolt's `CharacterVirtual` with an arbitrary up. Each step gameplay sets its wish:

```cpp
world.set_input(walker, {.up = up,                       // −ĝ, filtered as the game likes
                         .rotation = facing,
                         .move = forward * 5.0});         // empty: standing (nothing added)
```

The step keeps the velocity along up (or the ground's velocity when standing on it), adds g·dt from the hook and
`move`, and moves the character with sliding, stairs (`step_up`) and floor sticking (`step_down`).

## Terrain colliders (for the voxel bridge, VCH-114)

The voxel area hands LOD0 meshes over as `ColliderMesh`es; physics never includes voxel (voxel depends on physics):

```cpp
veach::physics::ColliderMesh m;
m.key = {.level = k.level, .x = k.x, .y = k.y, .z = k.z};   // voxel::ChunkKey, field for field, same order
m.origin = voxel::chunk_origin(k);                          // lattice units
m.extent = voxel::chunk_units(k.level);
m.vertices = mesh.fixed;                                    // fixed point: lattice units × 2^16, the canonical integers
m.indices = mesh.indices;                                   // counter-clockwise seen from outside the solid
// With its neighbors' border triangles as adjacency (ADR 0083): the 27 chunks around it, z, y, x from −1 to +1.
m = veach::physics::with_adjacency_ring(m.key, m.origin, m.extent, neighborhood);
world.request_colliders(pad, {std::move(m)});              // installed before the step of tick + 2
```

- Requests at tick t cook on the job system right away and install before the step of t + 2 (K = 2,
  [ADR 0077](../adr/0077-amendments-from-sp-12.md)), together and in chunk-key order; a later request for a chunk in
  the same tick wins. A cook that is not done by then makes the step wait: a slow machine stalls, it never simulates
  differently.
- An install replaces a chunk's shape in place (its body stays) and wakes what rests on it. A mesh without triangles
  removes the collider. `install_colliders` installs cooked ones at once, for loading a region before its first step.
- The scene must sit on a lattice point with the frame's axes, so vertices are exact (`physics.not_on_lattice`).
- Adjacency triangles inform Jolt's internal-edge handling and never collide, for characters and rigid bodies alike.
  When a chunk is remeshed, the bridge resubmits the chunks whose ring it feeds in the same request.
- `colliders(scene)` lists installed colliders with their shape hashes and install slots, for hashes and tools.

## The ECS binding

`engine.import<veach::PhysicsModule>()` gives the world a `PhysicsWorld` (`veach::physics_world(world)`), with scene 0
for the root world:

```cpp
planet.set(veach::ReferenceFrame{.kind = veach::FrameKind::rotating, .up = veach::FrameUp::radial});
planet.set(veach::PhysicsScene{.kind = veach::physics::SceneKind::surface});      // a scene in that frame
world.entity("crate").child_of(planet)
    .set(veach::Transform{.translation = {6.371e6 + 1.0, 0.0, 0.0}})
    .set(veach::RigidBody{.shape = veach::BodyShape::box, .mass = 500.0});         // PhysicsBody{scene, body} appears
```

- A `RigidBody` entity is a direct child of a frame with a `PhysicsScene` (or of nothing); its `Transform` is its
  pose in that frame. Setting `RigidBody` again rebuilds the body; removing it removes the body.
- The module steps in PostSimulation after the frame pose sources registered before it (CoreModule's `FrameSpin`), so
  scenes take their frame's spin for the step, and writes dynamic and kinematic bodies' poses to their `Transform`s
  for the end of the step (ADR 0085). Write forces in Simulation through `physics_world(world)`.
- Components hold ids and parameters only; the physics state is the module's.

## MCP tools

`physics_scenes`, `physics_bodies {scene?, limit?}` and `physics_contacts {limit?}` are read-only: scenes with their
origin, frame and counts, bodies with scene and frame positions, characters' ground state, colliders' chunk keys, and
the last step's sorted contacts. They report `imported: false` in an app without `PhysicsModule`. `veach-lab` imports
it.

## Determinism

Physics is replay tier: same inputs, same order, same bits, on any thread count; with Jolt's cross-platform
deterministic build the bits match on macOS arm64 and Windows x64, which the committed SP-11, SP-12, SP-13 and seams
hashes check. What keeps it so:

- every call that reaches Jolt happens on the main thread in a fixed order (bodies by id, colliders by chunk key);
- rotations enter as `dquat` rounded to float32 and normalized; gravity forces are computed in double, rounded once;
- `optimize(scene)` rebuilds the broad phase and is part of the input: call it at the same points (after a bulk add);
- contact events are sorted before anyone sees them.

## Measured

`veach-bench-physics` (Release, M5 Max, SP-11's pile, ms per step mean; `--cap` is `WorldSettings::max_concurrency`):

| Bodies | 1 thread | 2 | 4 | 8 | 17 | 17, capped at 4 (the default) |
|---|---|---|---|---|---|---|
| 200 | 0.24 | 0.21 | 0.18 | 0.30 | 0.78 | 0.49 |
| 2,000 | 3.36 | 2.45 | 1.78 | 1.89 | 2.45 | 2.00 |

The Jolt state hash is the same on every line of a body count. SP-11's Jolt thread pool measured 0.22 / 0.17 / 0.29 ms
(1 / 4 / 8 threads) and 2.91 / 1.45 / 1.65 ms for the same piles: the engine's job system matches it at 200 bodies
and is 15 to 20 % slower at 2,000. Spreading a step over all 17 workers costs more in wake-ups than it gains, hence the
cap of 4. SP-13's whole scene (two scenes, two characters, crates, a craft) steps in about 0.013 ms.
