# 0085: Amendments from VCH-111: the render origin on a 64 m grid, frame up, FrameTransform's frame, pose sources

Date: 2026-11-27

## Status

Proposed (the dev lead accepts or amends). Amends [0045](0045-large-world-positions-and-reference-frames.md) and
[0046](0046-64-bit-values-across-luau-json-mcp.md), and the SP-03 decision "one render origin per frame, the primary
camera's position" ([findings](../research/spikes/sp-03-precision-frames.md)). Implemented by VCH-111
([frames](../specs/frames/spec.md)).

## Context

VCH-111 put ADR 0045 into the engine: `ReferenceFrame` entities, double `Transform.translation`, `FrameTransform`
instead of `GlobalTransform`, composition through the lowest common ancestor, `up`, the handoff mechanism,
camera-relative extraction, and the Luau `Position` of 0046. Building it surfaced details the ADRs leave open or get
slightly wrong. Measured: every golden is byte-identical except Bistro (2 and 3 of 230,400 pixels off by 1/255, from
composing glTF node translations in double; FLIP 0.00000), propagation of 100k entities is as fast as float32 (flat
0.41 ms, wide 1.02 vs 1.13 ms, chains 16.7 vs 16.8 ms, release, M5 Max), a handoff inertial → body-fixed → inertial at
95 m/s moves the body 4.7e-10 m and changes its velocity by 8e-14 m/s, and the Cornell box inside a frame 0.1 ly out
renders byte-identically to the box at the origin.

## Decision

1. **The render origin is the primary camera's position in its frame, rounded to a 64 m grid**, not the position
   itself. Render-space coordinates stay under 56 m from the camera (float32 spacing 4 µm), static content keeps its
   float32 matrices while the camera moves inside a cell (what the GPU-driven renderer needs to avoid re-uploading
   instances), and a flat scene within 32 m of the origin renders exactly as world-space rendering did, so the goldens
   did not move. Render space has the camera frame's axes. `RenderWorld::space` records the frame and origin for
   extractors that rebase their own content (`render::RenderSpaceMap`); nothing on the GPU sees it.
2. **`ReferenceFrame` has an `up` field** (`axis_y` | `radial`): `up(frame, position)` needs to know whether a frame is
   a ship or a body without naming the toolbox. `kind = inertial` ignores `angular_velocity`.
3. **`FrameTransform` carries its `frame`** (the entity, 0 for the root world). Extraction groups by it without walking
   `ChildOf`, and `frame_of` is defined as the nearest `ReferenceFrame` ancestor, found through entities without
   `Transform`.
4. **A frame entity has no `Transform`.** Its pose is its `ReferenceFrame`; its children start from the identity.
   Both on one entity logs `ecs.frame_has_transform`, and the `Transform` is ignored.
5. **Pose sources write in `PostSimulation` or earlier, for the end of the step** (`Time.elapsed + Time.delta`), so
   frames and the entities in them describe the same instant at propagation and extraction (a one-tick lag would put a
   body-fixed base 7.75 m off in an inertial view at Earth's equator). The engine's generic source is `FrameSpin`
   (spin about +Y from exact tick time, `veach::dmath`); the toolbox drives body frames from its own clock through the
   same contract.
6. **What is cached per tick is the tree** (`FrameLink`: parent frame and depth, refreshed by the frame pass in
   TransformPropagation and PreExtract). Poses are composed on demand through the lowest common ancestor, subtracting
   positions there before rotating, and extraction caches one pose per frame for its pass. A pair cache was not
   needed: composition is a few dozen flops per tree level.
7. **In Luau every double vector is a `Position`**, including `ReferenceFrame`'s velocities; the frame API lives in
   `veach.frames` (`of`, `convert`, `offset`, `up`) rather than at the top of `@veach`. Handoff stays C++ until a
   script needs it.
8. **glm conversions are explicit everywhere** (`GLM_FORCE_EXPLICIT_CTOR`): a `dvec3` never turns into a `vec3`
   silently. It cost five call sites.

## Consequences

- Features that read `view.position.y` as altitude use `RenderView::height_in_frame` and `up` (the atmosphere does).
- A far second camera (split screen across the world) would need per-view draw matrices; not a case the toolbox has.
- The toolbox's body frames must write their pose for the step's end; VCH-115 binds the orrery to frames that way.

## Alternatives considered

- **The camera position itself as the origin** (SP-03): every static matrix changes every frame, and every golden
  changes in the last bit.
- **A per-pair pose cache keyed by (from, to)**: more state to invalidate for no measured gain.
- **Frames with a `Transform` for their pose**: two ways to move a frame, and the propagation would need to know which
  one wins.
