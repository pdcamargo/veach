# 0045: Large-world positions: doubles in a reference-frame tree, float32 only on the GPU, no floating origin

Date: 2026-11-27

## Status

Accepted. Amends [ADR 0013](0013-conventions-and-math.md) (the `Transform` type; `world_up` as a constant) and
[ADR 0020](0020-renderer-data-and-extraction.md) (`RenderWorld` holds no absolute positions).

## Context

Every position in Veach is float32 in one flat world: `Transform.translation` is `math::vec3` and `GlobalTransform` a
float32 `mat4` (`engine/ecs/include/veach/ecs/transform.hpp`), copied as is into the render world. The toolbox needs millimeter work (edits, contacts, walking inside an orbiting
ship) on bodies that spin and orbit, from s = 0.01 to 1 ([0049](0049-scale-model.md)), in a galaxy ~10^21 m across,
deterministically ([0043](0043-determinism-tiers-and-strict-floating-point.md)). Apple GPUs have no fp64.
Research: [01-coordinates](../research/01-coordinates.md).

The precision budget (ULP in meters, from the frame origin):

| Where | Distance at s = 1 | float32 | float64 | Distance at s = 0.01 | float32 | float64 |
|---|---|---|---|---|---|---|
| Base or cave, local frame | 1 km | 6e-5 | 1e-13 | 1 km | 6e-5 | 1e-13 |
| Surface, from body center | 6.37e6 | **0.5** | 9e-10 | 6.37e4 | 4e-3 | 7e-12 |
| Gas giant cloud tops | 7.0e7 | **8** | 1.5e-8 | 7.0e5 | 0.06 | 1e-10 |
| Moon orbit | 3.8e8 | **32** | 6e-8 | 3.8e6 | 0.25 | 5e-10 |
| 1 AU from the barycenter | 1.5e11 | **16 km** | 3e-5 | 1.5e9 | 128 | 2.4e-7 |
| System edge, 30 AU | 4.5e12 | **524 km** | 1e-3 | 4.5e10 | 4 km | 7.6e-6 |
| 1 light-year | 9.5e15 | 1e9 | **2** | — | — | — |
| Across the galaxy, 50,000 ly | 4.7e20 | 3.5e13 | **65 km** | — | — | — |

A float32 spin quaternion misplaces a surface point by 0.38 m at s = 1, so frame orientations must be double.
Camera-relative float32 is always sub-pixel (about 1/9,000 of a 1080p pixel at any distance); what breaks rendering is
computing it in float32 from large numbers.

## Decision

**Positions are doubles relative to a parent frame.**

- `Transform.translation` becomes `math::dvec3`; `rotation` stays a float32 `quat`, `scale` a float32 `vec3`.
- **A frame is an entity** with `ReferenceFrame { kind (inertial | rotating), position dvec3, orientation dquat,
  linear_velocity dvec3, angular_velocity dvec3 }` relative to its parent frame, written by a pose provider (orbits and
  spins from the tick, [0048](0048-bodies-on-rails.md); gameplay for ships and stations).
- An ordinary entity lives in its nearest frame ancestor in the `ChildOf` hierarchy.
- **Propagation stops at frames.** `GlobalTransform` is replaced by **`FrameTransform`** (double translation, float32
  3×3 basis), the pose in the entity's frame. The rename forces every "world space" use to be reviewed.
- Frame-to-frame poses compose in double through the **lowest common ancestor**, cached once per tick.
- `math::world_up` is valid only in flat frames; "up" is the query `up(frame, position)`.
- **Handoff** re-parents an entity, converting pose and velocity in double (`v_rot = v_inertial − ω × r`, 465 m/s at
  Earth's equator at s = 1). The mechanism is engine; the policy (sphere of influence, altitude bands, hysteresis) is
  toolbox ([0059](0059-gravity-flight-travel.md)). Velocities of ordinary entities stay float32.

**The tree:** system barycenter (inertial, the largest continuous frame: 30 µm at 1 AU, 0.25 m at 0.1 ly) → binary
barycenters and body orbit frames (body-centered, inertial axes) → body-fixed rotating frames → base or ship → ship
interior. Bases, terrain and walking players live in body-fixed frames; ships in orbit in orbit frames.

**The galaxy level is integers:** `GalaxyPosition { std::int64_t x, y, z; }` in **kilometers** (±975,000 ly), derived
from the system's address ([0050](0050-universe-addresses-and-seeds.md)). Directions between systems are exact integer
differences converted to double. Nothing simulates between them.

**No floating origin in simulation.** Nothing simulated depends on where the camera or a player is.

**Rendering is camera-relative float32, computed in double at extraction.** For each view, draw and light, extraction
computes `camera_from_object` in double through the frame tree and casts it to float32. `RenderWorld` holds **no
absolute positions**. GPU content keeps vertices relative to small chunk origins; only origins are re-based per frame.
Each view carries a body context: altitude and up in double (replacing `view.position.y`), the camera position relative
to the nearest body as a split double (two float32) for GPU noise, and the frame rotation for sun directions. Photo
mode's TLAS takes camera-relative transforms fixed where accumulation starts ([0041](0041-photo-mode.md)).

**Wire and saves** carry (frame id, frame-local position), never system or galaxy doubles
([0055](0055-universe-identity-and-versioning.md), [0056](0056-server-authoritative-multiplayer.md)). Physics runs one
scene per active frame ([0058](0058-physics-jolt.md)); fictitious forces are off by default, a per-body flag.

**Where it lives** ([0065](0065-engine-areas-for-the-toolbox.md)): position types, frames, propagation, the handoff
mechanism and camera-relative extraction are engine (`engine/ecs`, `engine/render`); galaxy integers, body frame kinds
and handoff policy are `space/`.

## Consequences

- One mechanism covers bases on spinning planets, ships in orbit and walking inside moving ships.
- Every shader from now on assumes camera-relative input. Camera-relative matrices for 100k instances are 4.8 MB per
  frame; double math halves SIMD width in propagation and extraction.
- GPU noise must not use body-relative float32 (0.5 m steps at s = 1): fields take integer cell + float offset
  ([0052](0052-procgen-runtime.md)).
- The translation type, frame-relative saves and frame identity get expensive to change once content exists.

**Validated by** VCH-079 (imgdiff-clean screenshots at 1 km, 1 AU and 30 AU; Tracy float vs double propagation over
100k entities) and VCH-089 (walk, hover and land at s = 0.01 and 1: no base drift, |Δv| < 1 mm/s through handoffs).
**Reopen if** double propagation is measurably expensive at simulation-LOD entity counts (double only on frame roots),
the product wants continuous sublight flight between stars (a sector frame level), or multiplayer becomes lockstep
(int64 fixed point inside frames).

## Alternatives considered

- **Float32 plus a floating origin** (KSP, Unity): ties results to the camera; breaks determinism and multiplayer.
- **Doubles in one world frame** (Unreal 5): not galaxy-wide, and one frame can't hold a spinning base still.
- **int64 fixed-point positions:** exactness we only need in ids, paid with conversions in every math call.
- **Tile plus float offset** (Unreal's pre-5.4 FLWC): retired for jitter at tile edges.
