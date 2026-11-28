# Reference frames and double positions

**Status:** Implemented
**Area:** ecs, render, script, runtime
**Owner:** dev lead

## Summary

Positions are doubles relative to a reference frame ([ADR 0045](../../adr/0045-large-world-positions-and-reference-frames.md)).
A frame is an entity with `ReferenceFrame`; an ordinary entity lives in its nearest frame ancestor, and propagation
computes its `FrameTransform` (double translation, float32 basis) in that frame. Poses between frames compose in
double through their lowest common ancestor. Extraction makes everything relative to one render origin near the
primary camera, so the GPU only ever sees small float32 numbers. Luau sees positions as a `Position` userdata
([ADR 0046](../../adr/0046-64-bit-values-across-luau-json-mcp.md)). A world without frames is one flat frame and
renders exactly as before.

## Motivation

Float32 world positions break the real-time renderer 1 km from the origin and collapse geometry at 1 AU (SP-03,
SP-14). The toolbox needs millimeter work on spinning planets and inside moving ships, so every later slice
foundation (physics scenes per frame, terrain in body frames, bodies on rails) builds on this. Backlog: VCH-111, the
first foundation of the first slice (VCH-091).

## Requirements

Positions and hierarchy:

- `Transform.translation` MUST be `math::dvec3`; `rotation` stays a float32 `quat` and `scale` a float32 `vec3`.
- Every entity with `Transform` MUST get a `FrameTransform` (Flecs `With`), written only by propagation:
  `translation` (dvec3), `basis` (float32 3x3: rotation x scale) and `frame` (the frame entity it is relative to; 0 is
  the root world). It replaces `GlobalTransform`.
- An entity's frame MUST be its nearest `ReferenceFrame` ancestor in `ChildOf`, or the root world. An ancestor
  without `Transform` contributes nothing to the pose (as before), but the frame is still found through it.
- Propagation MUST stop at frames: a child of a frame starts from the identity, never from the frame's own pose.
- Propagation MUST compose the translation in double (`parent.translation + dmat3(parent.basis) * local.translation`)
  and the basis in float32 (`parent.basis * local.basis()`), so a flat scene's draw matrices are the ones float32
  propagation computed: bit for bit at the root.

Frames:

- `ReferenceFrame { kind (inertial | rotating), up (axis_y | radial), position dvec3, orientation dquat,
  linear_velocity dvec3, angular_velocity dvec3 }`, all relative to the parent frame and in its axes. Inertial frames
  ignore `angular_velocity`. A frame entity MUST NOT have a `Transform` (`ecs.frame_has_transform`, logged when both
  are present).
- Pose sources write `ReferenceFrame` (orbits and spins from the tick, gameplay for ships). They MUST run in
  `PostSimulation` or earlier and write the pose for the end of the current fixed step, `Time.elapsed + Time.delta`,
  so frames and the entities in them describe the same instant at propagation and extraction.
- `FrameSpin { tilt dquat, rate, phase }` is the engine's generic source: a rotating frame turning about its own +Y,
  `orientation = tilt * rotation(+Y, phase + rate * t)` from exact tick time (never integrated), computed with
  `veach::dmath`.
- The frame pass (TransformPropagation and PreExtract, before entity propagation) MUST refresh each frame's
  `FrameLink` (parent frame, depth, rotation matrix) once per tick; composition reads it.
- `frame_pose(world, from, to)` MUST compose in double through the lowest common ancestor, never through the root
  unless that is the ancestor. The pose carries kinematics: `x_to = p + R x_from`,
  `v_to = v + ω × (R x_from) + R v_from`.
- `up(world, frame, position)` MUST return the frame's +Y for `axis_y` frames and the root world, and the direction
  away from the frame's origin for `radial` frames. `math::world_up` is valid only in the former.
- `handoff(world, entity, to_frame, velocity?)` MUST re-parent the entity directly under `to_frame` (or the root),
  converting its pose and, if given, its velocity in double so that nothing moves: in a rotating frame
  `v_rot = v_inertial − ω × r`. The policy (when to hand off) is not the engine's.

Rendering:

- Extraction MUST choose one render origin per frame: the primary camera's position in its frame, rounded to a 64 m
  grid. Every `RenderView`, `MeshDraw` and `RenderLight` position is relative to it, in the camera frame's axes,
  computed in double and cast to float32. `RenderWorld` MUST hold no absolute position.
- Each view MUST carry its body context: `altitude` (height along `up` from the frame's origin, double before the
  cast) and `up` (render space), which replace `view.position.y`.
- With no frames and cameras within 32 m of the origin (every existing scene), the render origin is 0 and every
  golden MUST stay byte-identical.

Scripts, data and tools:

- Luau MUST see every `dvec3` field as a `Position` userdata: fields `x`, `y`, `z` (exact doubles);
  `Position + vector → Position`, `Position − vector → Position`, `Position − Position → vector`; `==`; `tostring`
  with round-trip digits. No `Position + Position` and no scaling. `veach.position(x, y, z)` and
  `veach.position(v)` construct one. A `Position` default declares a double field in a script component.
- Writing a `vector` where a `Position` is expected MUST fail with `script.invalid_value` and a hint to use
  `veach.position`.
- `veach.frames`: `of(entity) → Entity?`, `convert(position, from?, to?) → Position`, `offset(from, to) → vector`
  (in `from`'s frame), `up(frame?, position) → vector`.
- JSON (scenes, MCP, replay) MUST carry a `dvec3` as `[x, y, z]` with shortest round-trip digits; a `dquat` as
  `{x, y, z, w}`.
- The `frames` MCP tool MUST return the frame tree: each frame's path, parent, kind, up, pose and entity count.
- Replay hashes MUST include `ReferenceFrame` and the double translations bit for bit.

## API sketch

```cpp
// engine/ecs/include/veach/ecs/transform.hpp
struct Transform {
    math::dvec3 translation{0.0};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    math::vec3 scale{1.0f};
    [[nodiscard]] math::mat3 basis() const;  // rotation x scale
};
struct FrameTransform {
    math::dvec3 translation{0.0};
    math::mat3 basis{1.0f};
    ecs::entity_t frame = 0;  // 0: the root world
    [[nodiscard]] math::quat rotation() const;
    [[nodiscard]] math::dvec3 point(math::vec3 local) const;
};

// engine/ecs/include/veach/ecs/frames.hpp
enum class FrameKind : std::uint8_t { inertial, rotating };
enum class FrameUp : std::uint8_t { axis_y, radial };
struct ReferenceFrame { FrameKind kind; FrameUp up; math::dvec3 position; math::dquat orientation;
                        math::dvec3 linear_velocity; math::dvec3 angular_velocity; };
struct FrameSpin { math::dquat tilt; double rate; double phase; };
struct FramePose { math::dvec3 position; math::dmat3 rotation; math::dvec3 velocity; math::dvec3 angular_velocity;
                   math::dvec3 point(math::dvec3) const; math::dvec3 velocity_of(math::dvec3 p, math::dvec3 v) const; };

[[nodiscard]] Entity frame_of(const World&, Entity);
[[nodiscard]] Result<FramePose> frame_pose(const World&, Entity from, Entity to);
[[nodiscard]] math::dvec3 up(const World&, Entity frame, const math::dvec3& position);
[[nodiscard]] Result<> handoff(World&, Entity entity, Entity to_frame, math::dvec3* velocity = nullptr);
```

```lua
local veach = require("@veach")
local ship = veach.spawn("ship")
ship:set(veach.components.ReferenceFrame, {
	kind = "inertial", up = "axis_y",
	position = veach.position(1.496e11, 0, 0), orientation = veach.quat.identity(),
	linear_velocity = veach.position(0, 0, 0), angular_velocity = veach.position(0, 0, 0),
})
local pilot = veach.spawn({ name = "pilot", parent = ship })
pilot:set(veach.components.Transform, { translation = veach.position(0, 1.8, 0), rotation = veach.quat.identity(), scale = vector.one })
local here = veach.frames.convert(veach.position(0, 1.8, 0), ship, nil)  -- in the root world
local d: vector = here - veach.position(1.496e11, 0, 0)                   -- (0, 1.8, 0)
```

## Acceptance criteria

- **frames.AC1**: Precision: an entity 1 m from the origin of a frame placed at 1 AU, or at 0.1 ly, keeps its
  position to 1e-15 m relative to its frame; in the root world a double at 1 AU resolves 3.1e-5 m and at 0.1 ly
  0.125 m (SP-03's budget), and conversion from the frame to the root world loses no more than that.
- **frames.AC2**: Propagation stops at frames; a flat world's `FrameTransform` matches float32 propagation bit for bit
  at the root and to float32 rounding below; frame membership is found through ancestors without `Transform`.
- **frames.AC3**: Composition through the lowest common ancestor: two sibling frames 1 AU out give each other's
  pose to 1e-9 m, where composing through the root loses 1e-5 m; poses with rotations and velocities agree with
  hand-computed values; the inverse undoes the pose.
- **frames.AC4**: Handoff: a body moving at 95 m/s near a planet's surface, handed inertial → body-fixed → inertial,
  keeps its inertial position within 1e-6 m and its inertial velocity within 1 mm/s (|Δv| < 1e-6 m/s measured); the
  rotating-frame velocity is `v − ω × r`.
- **frames.AC5**: A `FrameSpin` frame's orientation comes from the tick: after 10^6 steps it equals the closed form,
  and a pose source written with `space::orientation` keeps a body-fixed base fixed in the inertial view.
- **frames.AC6**: `up`: +Y in the root and `axis_y` frames, radial in `radial` frames.
- **frames.AC7**: Extraction is camera-relative with one render origin: `RenderWorld` of a scene placed 1 AU away
  (in the root world, or in a frame) equals the scene at the origin to float32 rounding, the frame case bit for bit;
  views carry altitude and up.
- **frames.AC8**: Screenshots: the Cornell box and its camera 1 AU from the origin match
  `tests/golden/cornell_box_realtime.png`; inside a frame 0.1 ly out, byte-identical to it; seen from the root world
  into a frame 1 AU out, within the golden's limits.
- **frames.AC9**: Luau `Position`: exact doubles in and out of `Transform`, arithmetic and its errors, a `vector`
  where a `Position` belongs fails with `script.invalid_value`, `veach.frames` functions, script components with
  `Position` fields; generated types type-check.
- **frames.AC10**: JSON and MCP round-trip double positions exactly (`get_component`, `set_component`, `eval`), and
  the `frames` tool lists the tree.
- **frames.AC11**: Flat worlds are unchanged: every golden byte-identical (checked by rendering before and after),
  record/replay still replays, and propagation of 100k entities stays within noise of float32.

## Out of scope

- Deep-hierarchy layout (VCH-106), int64 ids and arrays in reflection (VCH-099), persistent ids (VCH-103).
- Physics scenes per frame and fictitious forces (VCH-113), body frames bound to orbits and handoff policy (VCH-115).
- Per-view draw matrices for a second camera far from the primary one; split doubles for GPU noise (ADR 0052).

## Open questions

None. Implementation decisions beyond ADR 0045 and 0046 (the 64 m render-origin grid, `ReferenceFrame::up`,
`FrameTransform::frame`, pose sources in PostSimulation, what is cached): [ADR 0085](../../adr/0085-amendments-from-vch-111.md).

Tests: `[frames.AC1]` to `[frames.AC7]`, `[frames.AC9]`, `[frames.AC10]` in `tests/frames_tests.cpp`; AC8 is the
`golden: cornell box ... (frames)` tests and `frames: a scene in a far frame renders byte-identically to the origin`
(`apps/lab/scripts/frames_*.luau`); AC11 is every golden test, the replay tests and `veach-bench-transforms`.

## Links

- ADRs: [0045](../../adr/0045-large-world-positions-and-reference-frames.md),
  [0046](../../adr/0046-64-bit-values-across-luau-json-mcp.md), [0065](../../adr/0065-engine-areas-for-the-toolbox.md),
  [0080](../../adr/0080-amendments-from-sp-13.md)
- Research: [01-coordinates](../../research/01-coordinates.md), [SP-03](../../research/spikes/sp-03-precision-frames.md),
  [SP-13](../../research/spikes/sp-13-walk-hover-land.md), [SP-14](../../research/spikes/sp-14-exposure-depth.md)
- Specs: [transforms](../transforms/spec.md), [render-views](../render-views/spec.md)
- Backlog: VCH-111
- Guides: [conventions](../../guides/conventions.md), [luau-api](../../guides/luau-api.md)
