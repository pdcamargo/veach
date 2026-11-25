# Views and cameras

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

A `Camera` component on an entity with a `Transform` turns into a render view during extraction. The view holds
reverse-Z perspective matrices built from `veach::math`. Render features read the primary view from
`FrameContext`. Gameplay, Luau and MCP only ever write the two components, never GPU state.

## Motivation

So far every image is drawn in normalized device coordinates. Placing anything in a world needs a camera, and
the camera has to cross the extraction boundary ([ADR 0016](../../adr/0016-renderer-data-and-extraction.md)) the
same way as everything else. Backlog: VCH-011.

## Requirements

- `veach::Transform` (ecs, reflected, Scriptable): `translation` (vec3, meters), `rotation` (quat, glTF order
  `x, y, z, w`, identity by default), `scale` (vec3, 1 by default), relative to the parent. Cameras read the
  world pose, `GlobalTransform` ([transforms](../transforms/spec.md), VCH-007).
- `veach::Camera` (render, reflected, Scriptable): `fov_y` (radians, default 60°), `near_plane` (meters, default
  0.1; not `near`, a macro in Windows headers), `active` (default true). Infinite far plane, reverse-Z, through `math::perspective_reverse_z`. The camera looks
  down its local −Z with +Y up ([conventions](../../guides/conventions.md)). Scale does not affect the view.
- MUST extract every active camera into `RenderWorld::views`, ordered by entity id, with `view`, `projection`,
  `view_projection`, `inverse_view_projection`, `position`, `near_plane` and the target size. A camera without a
  GlobalTransform sits at the origin.
- `FrameContext::view` MUST point at the first view, or be null when there is none. Features fall back to their
  view-less behavior, so every existing golden stays unchanged.
- Raster and ray-traced rendering MUST agree through the same view, on Metal and DX12.

## API sketch

```lua
local veach = require("@veach")
local camera = veach.spawn("camera")
camera:set(veach.components.Transform, {
	translation = vector.create(1.5, -0.05, 2.6),
	rotation = { x = 0, y = 0.258819, z = 0, w = 0.965926 },  -- 30° about +Y
	scale = vector.create(1, 1, 1),
})
camera:set(veach.components.Camera, { fov_y = math.rad(50), near_plane = 0.1, active = true })
```

```cpp
struct RenderView {  // engine/render/include/veach/render/renderer.hpp
    math::mat4 view, projection, view_projection, inverse_view_projection;
    math::vec3 position;
    float near_plane;
    std::uint32_t width, height;
};
```

## Acceptance criteria

- **views.AC1**: An active camera with a Transform becomes one view. A point `near_plane` meters in front of it maps to
  NDC (0, 0) at depth 1. A point farther away has a depth in (0, 1). A point to its right has NDC x > 0.
  `inverse_view_projection` undoes `view_projection`.
- **views.AC2**: Inactive cameras are skipped. Several active cameras come out in entity-id order. With no camera
  there are no views and `FrameContext::view` is null.
- **views.AC3**: `golden: triangle (camera, raster)` and `golden: triangle (camera, raytraced)` both match
  `tests/golden/triangle_camera.png`, the triangle seen through a perspective camera set from Luau. They run on
  Metal (pre-push gate) and DX12 (WARP, hosted CI).
- **views.AC4**: Without a camera, the existing goldens pass unchanged: features keep their NDC behavior.

Tests: `[views.AC1]`, `[views.AC2]` in `tests/render_tests.cpp`; the camera goldens in `tests/CMakeLists.txt`
(`apps/lab/scripts/triangle_camera_*.luau`); AC4 by the unchanged `golden: background` and `golden: triangle`
tests. Parameter blocks holding a `float4x4` are `alignas(16)`, because Metal rounds them up, and the VCH-041
layout check now enforces it.

## Out of scope

- Several views per frame (split screen, shadow views), orthographic cameras, jitter: when a feature needs them.

## Open questions

None.

## Links

- ADRs: [0010 conventions](../../adr/0010-conventions-and-math.md),
  [0016 extraction](../../adr/0016-renderer-data-and-extraction.md)
- Backlog: VCH-011, VCH-007
