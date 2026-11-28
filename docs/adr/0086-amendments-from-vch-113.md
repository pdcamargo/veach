# 0086: Amendments from building engine/physics: the gravity hook's contract, rings for rigid bodies, four threads per step, Jolt optimized in MSVC Debug

Date: 2026-11-28

## Status

Accepted. Amends [0058](0058-physics-jolt.md), [0075](0075-amendments-from-sp-11.md) (decision 3) and
[0083](0083-terrain-colliders-with-an-adjacency-ring.md). Found while building `engine/physics` (VCH-113,
[spec](../specs/physics/spec.md), [guide](../guides/physics.md)).

## Context

`engine/physics` now holds Jolt behind `veach::physics`: scenes per frame, a `JobSystemWithBarrier` on
`veach::JobSystem`, the gravity hook, fictitious forces, velocity-tracking free flight, transfers, collider installs at
t + 2 with the adjacency ring, characters, sorted contacts, an ECS binding and MCP tools. SP-11's, SP-12's, SP-13's and
the seams sweep's committed hashes all reproduce through it unchanged, and SP-13 was checked step for step against its
old Jolt code for whole landings at both scales. Building it settled details the ADRs leave open, measured the one
0083 left for later, and found a portability bug in the sweep's code.

## Decision

1. **The gravity hook returns an acceleration; physics multiplies.** `GravityFn(scene, frame, position, velocity) →
   m/s²` in the parent frame's axes, called on the main thread for awake dynamic bodies (body id order) and
   characters. Physics adds the fictitious accelerations of rotating free flight, then applies `a · m` in double,
   rounded to float32 once, with `m` the creator's mass or Jolt's. A force-returning hook would need the body's mass
   in the toolbox and round twice.
2. **Rigid bodies reject adjacency-ring contacts too**, in the scene's contact listener (`OnContactValidate`), so
   0083's rings are final for every body. Measured over SP-12's terrain: eight boxes sliding at 6 m/s kick up at
   per-chunk border edges (p99 |Δv·up| 0.63 m/s against 0.33 inside); with rings 0.20 at borders and 0.25 inside, a
   merged mesh 0.31. Rolling balls show no border effect with any layout.
3. **A step spreads over at most four threads** (`WorldSettings::max_concurrency`). On the M5 Max a 200-body pile
   steps in 0.18 ms on 4 threads, 0.30 on 8 and 0.78 on all 17 workers; 2,000 bodies in 1.78, 1.89 and 2.45 ms.
   Capped at 4, all 17 workers give 0.49 and 2.00 ms: the rest is wake-up cost of a wide pool. The bits never depend
   on it.
4. **MSVC Debug optimizes Jolt** (`/O2 /Ob2`, no `/RTC1`, in Jolt's directory only), replacing 0075's "MSVC Debug
   stays unoptimized": unoptimized Jolt made SP-13's tests take 86 s on Windows CI. `JPH_DEBUG`, asserts and the debug
   runtime stay. x64 MSVC also gets `/arch:AVX2` explicitly (0075 decision 2).
5. **Scenes with no bodies skip `PhysicsSystem::Update`**; the step order is fixed: due colliders, gravity, characters,
   then each scene's update in scene id order, then sorted contacts. `optimize` (Jolt's broad-phase rebuild) is an
   explicit call and part of the deterministic input.
6. **Transfers take the destination's parent-frame state from the caller.** Converting across frames is
   `engine/ecs`'s (VCH-111, `handoff`, `frame_pose`, in double); physics keeps a body's id and settings across scenes
   and lets only an empty scene change its frame. A still scene's re-anchor keeps its origin tick.
7. **The adjacency ring welds vertices in a defined order** (a triangle's a, b, then c). The sweep's
   `cook_with_ring` numbered them inside one function call's arguments, whose evaluation order C++ leaves open: Clang
   goes left to right (the committed hashes), MSVC may not, so Windows could cook different shapes.
8. **The ECS binding lives in one file and holds the world in the module struct** (`PhysicsModule::physics`, dying
   with the world). Bodies are direct children of their frame entity, whose `PhysicsScene` makes the scene; the step
   runs in PostSimulation after the frame pose sources registered before it, and writes poses for the end of the
   step (0085 decision 5).

## Consequences

- The toolbox's gravity registry is a plain function of frame and position; physics never sees a planet.
- A surface scene full of edited terrain behaves for crates and craft as for characters at chunk borders.
- Windows Debug test time for physics drops with Jolt optimized; the CI run that lands this change shows by how much.
- Physics steps are cheap enough that a wide pool is the bottleneck; pinning physics to a subset of workers is a
  possible later gain, not needed now.

## Alternatives considered

- **A hook that returns a force:** the toolbox would need each body's mass, and fictitious forces would round
  separately.
- **Rings for characters only:** leaves sliding bodies bumping at every chunk border.
- **All worker threads for every step:** slower at every measured size.
- **Leaving MSVC Debug unoptimized:** keeps Windows the slowest gate for physics tests.
