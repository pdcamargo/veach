# 0058: Physics: Jolt in double precision, one scene per frame, per-body gravity, deterministic collider installs

Date: 2026-11-27

## Status

Accepted. Amends [0017](0017-dependency-set.md): adds Jolt Physics v5.7.0 (MIT) to the dependency set; Box3D is
re-evaluated when it reaches 1.0.

## Context

The first slice lands on a rotating planet and digs a hole, at body scales 0.01 to 1, and Veach has no physics engine.
Needs: double positions in a frame tree ([0045](0045-large-world-positions-and-reference-frames.md)); same-binary
determinism, preferably cross-platform ([0043](0043-determinism-tiers-and-strict-floating-point.md)); collider changes
for editable terrain; a character with an arbitrary up vector; state save and restore for prediction
([0056](0056-server-authoritative-multiplayer.md)). Research: [09-physics](../research/09-physics.md).

Jolt v5.7.0 has shipped in AAA titles and has `JPH_DOUBLE_PRECISION` (5-10% slower) and
`CROSS_PLATFORM_DETERMINISTIC` (about 8% slower). Box3D v0.1.0 is determinism-first, but alpha by its author's word.

## Decision

**Library.** Jolt Physics v5.7.0, pinned in `cmake/VeachDependencies.cmake` and listed in `THIRD_PARTY_NOTICES.md`,
built with `JPH_DOUBLE_PRECISION` and `CROSS_PLATFORM_DETERMINISTIC` on every platform from the first commit, under
the strict floating-point flags of 0043. Jolt types are private to a generic engine area, `engine/physics`
([0065](0065-engine-areas-for-the-toolbox.md)), the way SDL is private to `engine/platform`. It starts on
`JobSystemSingleThreaded` and moves to a `JobSystemWithBarrier` on the engine job system when VCH-015 lands.

**Tier.** Physics is replay tier: bit-identical for the same binary and platform. The cross-platform build is
insurance and a test, not a contract; multiplayer corrects physics from the server.

**Scenes.** One physics scene per active frame, each near its own origin and moving slowly:

- a **body-fixed surface region** (terrain, bases, landed ships and walking players are static or slow; anchored near
  its occupants and re-anchored rarely, at fixed ticks);
- a **ship interior** in the ship's frame, with gravity from the ship's artificial-gravity sources and the ship's
  acceleration not applied (inertial dampers);
- a **co-moving free-flight scene** whose origin and velocity follow a fast ship away from terrain.

Bodies move between scenes only at tick boundaries, in a fixed order, with pose and velocity converted in double
(`v_inertial = v_fixed + ω × r` across a rotating frame). An unpowered ship with nothing to collide with leaves physics
and goes on rails ([0048](0048-bodies-on-rails.md)). **No Coriolis or centrifugal forces**: surface physics runs in the
body-fixed frame and the frame transform at handover is exact. Day lengths that make this wrong are refused by the
validation in [0049](0049-scale-model.md).

**Stepping.** Once per fixed step in PostSimulation, after gameplay writes forces in Simulation and before
TransformPropagation. Contact events are sorted by (body id, body id, sub-shape) before gameplay sees them.

**Gravity.** World gravity is zero in every scene. Before each step the engine adds a per-body force from the gravity
registry of [0059](0059-gravity-flight-travel.md): the dominant body by sphere of influence with μ = g·R², or an
artificial-gravity volume that overrides it. Physics never reads G or a body's mass.

**Terrain colliders.** Static `MeshShape` colliders per LOD0 chunk, meshed on the CPU from the canonical density
([0053](0053-terrain-density-field.md)), never from GPU output. They exist only in a swept region around each dynamic
body: its predicted position over the next 0.25 s plus 2 chunks (32 m), working values. A collider requested at tick
t, by streaming or by an edit ([0054](0054-terrain-edits-and-water.md)), is installed at tick t + K, in chunk-key order.
K is one engine constant for every machine and quality preset, set from VCH-088's measurements. If a collider is not
ready at t + K, the step waits: a slow machine stalls, it never simulates differently.

**Small bodies.** A creator-set gameplay gravity floor for walkable small bodies (working default 0.05 g, pending the
owner's packet) instead of physical values that let a jump reach escape speed. Collisions at orbital closing speeds are
resolved by a gameplay rule (destruction), so the solver never needs to handle more than a few hundred m/s.

## Consequences

- A shipped library with space-game precedent; the engine seam keeps a switch possible while little depends on it.
- Collider cooking scales with flight speed and digging, and the M1 class has fewer workers: that sizes the travel
  governor's floor ([0059](0059-gravity-flight-travel.md)).
- Double precision is ABI for physics state and recordings; decided once.
- Games inherit Jolt's feel; the library gets expensive to change after the first games.
- **Validation:** VCH-087 (SP-11): Jolt double plus cross-platform deterministic gives the same state hash on the macOS
  gate and Windows CI, with 1 and N workers. VCH-088 (SP-12): brush edits replay to identical chunk hashes on arm64 and
  x64, a ball falls into the dug hole, Tracy ms for mesh plus cook per chunk. VCH-089 (SP-13): walk, hover and land on a
  rotating planet at s = 0.01 and 1 with no base drift and |Δv| < 1 mm/s through frame handoffs.
- **Reopen if** Box3D reaches a stable 1.0 and VCH-087 shows Jolt diverging across platforms where Box3D does not, or
  Box3D steps and cooks our terrain clearly faster; if mesh cooking cannot keep up with digging on a 4-worker budget
  (SDF collision shapes); or if the owner wants short days on large planets (fictitious forces).

## Alternatives considered

- **Box3D v0.1.** Determinism-first with built-in recording, but alpha: rework at each breaking release, and no mature
  character controller or in-place collider edits yet.
- **PhysX 5, Bullet, Rapier, Havok, our own.** Single precision (PhysX), no cross-platform claim (Bullet), a Rust
  toolchain (Rapier), proprietary ([0023](0023-license-mit.md)), years of solver work.
- **One double world per system.** The broadphase is about 1 m coarse at 10⁷ m; 1 AU is 1.5·10¹¹ m.
- **A float world with a floating origin.** Cannot serve two players far apart, or a server.
- **Ships as moving platforms in one world.** Space Engineers' "clang".
- **Install colliders when ready.** Replays and cross-platform tests would become flaky.
