# Sun shadows in the real-time renderer

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

The brightest suns cast cascaded shadow maps in the real-time renderer, from the cockpit out to a kilometer, through
the GPU-driven path of ADR 0093: each cascade culled on the GPU into its own compacted draw lists and drawn depth-only,
alpha-tested foliage with its coverage test. The shading function looks the shadow up in one place. Photo mode is
unchanged.

## Motivation

Nothing cast a shadow in real time: the slice looked flat, the dug hole's walls were sunlit, Bistro's street had no
shade. ADR 0062 decided cascaded shadow maps near and analytic occluders far; ADR 0090 gave the RHI depth bias,
comparison samplers and texture arrays; ADR 0093 gave the culling and indirect draws the cascades reuse. Item 4 of the
renderer pass (VCH-140).

## Requirements

- The brightest directional lights (at most two, ADR 0062) MUST cast shadow maps; other lights MUST stay unshadowed
  by geometry, and a camera MUST be able to choose fewer (`SunShadows.suns`, 0 turns shadow maps off).
- Each shadowed light MUST have 1 to 4 cascades (default 4) ending at `SunShadows.distance` meters (default 1000), each
  a quarter of the next; distances MUST be meters at every body scale.
- A cascade MUST be fitted to the bounding sphere of its slice of the camera's frustum and snapped to whole texels of
  a grid fixed in the render frame, so static shadows neither change size as the camera turns nor shimmer as it moves,
  including when render space moves to another 64 m cell (ADR 0045, ADR 0085).
- Casters MUST be culled per cascade on the GPU by the GPU-driven culling, against the cascade's box and against the
  camera's frustum slice swept away from the light, dropping casters under 0.35 texel in radius; survivors MUST
  be drawn by indirect draws from compacted lists (ADR 0089), alpha-tested ones with their alpha test.
- Casters between a cascade and the light MUST cast even when they lie beyond its near plane.
- Acne and peter-panning MUST be held off by a slope-scaled raster bias on the casters and a normal offset and a
  constant bias on the receivers, sized in texels of each cascade.
- Lookups MUST filter with the comparison sampler (PCF), widened by a blocker search to the penumbra the light's
  angular radius gives (soft shadows that harden at contact), at least 1.5 texels wide.
- Each cascade's last tenth MUST blend into the next, and the last cascade MUST fade out over the last fifth of the
  distance; beyond it nothing in the scene shadows the sun (the surface's own facing still does).
- The lookup MUST be one call in the shared shading function (`light_shadow`), returning 1 for a light without a map.
- Nothing MUST need ray tracing; photo mode MUST be unchanged.
- What each cascade drew MUST be readable (`CullStats::shadow_meshlets_drawn`, MCP `gpu_timings`).

## API sketch

```cpp
// On a camera entity (Scriptable). Without it a camera has these defaults.
struct SunShadows {
    std::uint32_t suns = 2;           // 0 turns shadow maps off; at most 2
    float distance = 1000.0f;         // meters from the camera where the last cascade ends
    std::uint32_t cascades = 4;       // 1 to 4, each four times the reach of the one before
    std::uint32_t resolution = 2048;  // texels per side of every cascade, 256 to 4096
};

// The geometry, public for tests and tools (veach/render/shadow_cascades.hpp):
std::array<float, 4> render::cascade_splits(float distance, std::uint32_t count);
render::CascadeFit render::fit_cascade(const RenderView&, const math::dvec3& origin, math::vec3 toward_light,
                                       float near, float far, std::uint32_t resolution);
std::vector<std::uint32_t> render::shadowed_lights(std::span<const RenderLight>, std::uint32_t max);
```

```lua
camera:set(c.SunShadows, { suns = 1, distance = 200, cascades = 3, resolution = 2048 })  -- a cockpit-scale scene
```

```slang
// The seam in the shading function (realtime_shade.slang): the share of light i its shadow map lets through.
const float shadow = light_shadow(shadows, i, position, facing, l, cos_l, view_distance, light.size);
```

## Acceptance criteria

- **shadows.AC1** — Cascades end at the shadow distance, each a quarter of the next, at most four.
- **shadows.AC2** — A cascade keeps its radius and texel size as the camera turns; a static point keeps its place
  within its texel as the camera moves by fractions of a texel; render space moving 64 m with the camera in place
  leaves every texel coordinate unchanged.
- **shadows.AC3** — The two brightest directional lights cast; point and spot lights and dark lights do not; `suns`
  limits the count.
- **shadows.AC4** — On the GPU (Metal, DX12/WARP): a box shadows the floor under a sun (the shaded floor loses the
  sun's share), the same pixel is lit with `suns = 0`, and a box whose shadow falls away from the view is not drawn
  into the nearest cascade (`CullStats::shadow_meshlets_drawn`).
- **shadows.AC5** — The goldens that change on purpose are regenerated and listed with their FLIP deltas (Bistro at
  noon and sunset, FlightHelmet in real time, the runtime mesh in real time); the others, photo mode's included,
  pass unregenerated. The thin-triangles golden turns shadow maps off: it checks shading, not the fins' shadows.
- **shadows.AC6** — Screenshots of Bistro at noon and sunset (against photo mode), the slice landed and from inside the
  dug hole at s = 0.01 and s = 1, the SP-16 field and a low sun's long shadows, and a moving camera's frames, show no
  acne, no peter-panning and no shimmer ([findings](../../research/sun-shadows/README.md)).

## Out of scope

- Shadows of point and spot lights, cloud shadows, ring shadows (ADR 0062's analytic occluders; eclipses already dim
  the sun's radiance), far-terrain self-shadowing beyond the last cascade (a horizon map from the toolbox's coarse
  heights), cached or staggered cascades, quality presets choosing the cascade count and resolution (ADR 0064):
  VCH-141.
- Temporal filtering of the penumbrae (with TAA, ADR 0061).

## Open questions

None.

## Links

- ADRs: [0095](../../adr/0095-sun-shadows.md), [0062](../../adr/0062-shadows-ambient-reflections.md),
  [0093](../../adr/0093-gpu-driven-core.md), [0090](../../adr/0090-rhi-texture-kinds-updates-and-raster-state.md),
  [0045](../../adr/0045-large-world-positions-and-reference-frames.md)
- Backlog: VCH-140, VCH-141
- Guides: [rendering](../../guides/rendering.md)
- Measurements: [sun shadows](../../research/sun-shadows/README.md)
