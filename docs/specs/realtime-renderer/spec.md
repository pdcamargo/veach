# Real-time renderer (interim visibility-buffer path)

**Status:** Implemented
**Area:** render, asset
**Owner:** dev lead

> The interim path itself is gone: the [GPU-driven renderer](../gpu-driven-renderer/spec.md) (ADR 0093) draws every
> camera without `PhotoMode` now. These criteria still hold for it and their tests still run.

## Summary

The real-time image of a glTF scene until the research phase designs the real PBR rasterizer: a visibility buffer
over cook-time meshlets, one compute pass that shades it with the shared materials, then the display transform. Basic
on purpose, and never dependent on ray tracing ([ADR 0040](../../adr/0040-interim-realtime-path-visibility-buffer.md)).

## Motivation

The reset ended the ray-traced real-time lighting ([ADR 0001](../../adr/0001-reset.md)). Agents, goldens and the lab
still need a real-time image of real scenes on any GPU at the engine's floor
([ADR 0003](../../adr/0003-platforms-and-hardware-target.md)).

## Requirements

- A camera without `PhotoMode` MUST be rendered by this path when the scene has meshes.
- The path MUST NOT build acceleration structures or use ray queries, and MUST render the same image with
  `VEACH_DISABLE_RAY_TRACING=1`.
- The cooker MUST split every primitive into meshlets of at most 64 vertices and 124 triangles, one material slot each,
  covering every triangle once ([ADR 0011](../../adr/0011-gltf-import-and-cooking.md)).
- The visibility pass MUST store `{instance, meshlet << 7 | triangle}` per pixel in an `rg32_uint` target cleared to
  `0xFFFFFFFF`, with a reverse-Z depth test, and alpha-test masked materials.
- The shade pass MUST light each pixel with every punctual light (unshadowed, none from behind the surface: the triangle
  for a double-sided material, the interpolated normal for a single-sided one), the sky's ambient irradiance on the
  diffuse lobe, and emission, and MUST show the sky with the sun's disk where there is no geometry and apply aerial
  perspective. It MUST interpolate the surface inside the pixel's triangle, never outside it.
- A single-sided triangle seen from behind MUST keep a vertex normal that faces the viewer (VCH-108); a double-sided
  one turns its normal with it.
- Exposure MUST follow the physical camera ([ADR 0037](../../adr/0037-exposure-and-agx.md)).
- Every pass MUST be timed and reported by MCP `gpu_timings`.

## API sketch

```lua
-- No component selects it: any active camera without PhotoMode.
local camera = veach.spawn("camera")
camera:set(veach.components.Transform, { translation = vector.create(0, 1, 4), rotation = veach.quat.identity(), scale = vector.one })
camera:set(veach.components.Camera, { fov_y = math.rad(40), ev100 = 0 })
```

## Acceptance criteria

- **rt.AC1** — Meshlets cover every triangle once, within the limits, one slot each, at level 0; cooking is
  deterministic.
- **rt.AC2** — With `VEACH_DISABLE_RAY_TRACING=1` the real-time image matches the same golden.
- **rt.AC3** — The Cornell box, FlightHelmet and Bistro (noon, sunset) match their real-time goldens
  (`tests/golden/*_realtime.png`, `bistro_*.png`), on Metal and on DX12/WARP (Bistro on Metal only).
- **rt.AC4** — Exposure follows the physical camera: `ev100 = log2(N² / t · 100 / S)` unless set explicitly.
- **rt.AC5** — `gpu_timings` lists every render-graph pass of the frame, in order, with its GPU time; without a GPU it
  says why there are none.
- **rt.AC6** — The reverse-Z depth test keeps the nearer triangle in any draw order; a pipeline that tests depth
  without a depth format is rejected on every backend.
- **rt.AC7** — An `rg32_uint` target clears to `0xFFFFFFFF` and keeps what the fragment shader writes, exactly.
- **rt.AC8** — Thin triangles standing upright in a smooth surface, with its normals and material, shade like the
  surface, whichever side faces the camera: `golden: thin triangles (realtime)`, on Metal and on DX12/WARP
  ([VCH-108](../../research/notes/vch-108-pinholes.md)).

## Out of scope

Shadows, indirect light and AO, reflections, transparency, temporal anti-aliasing, culling and LOD: the research
phase decides the real renderer (VCH-070, open question 7).

## Open questions

None.

## Links

- ADRs: [0039](../../adr/0039-realtime-renderer-is-raster-pbr.md), [0040](../../adr/0040-interim-realtime-path-visibility-buffer.md), [0011](../../adr/0011-gltf-import-and-cooking.md), [0037](../../adr/0037-exposure-and-agx.md), [0038](../../adr/0038-physical-sky.md)
- Backlog: VCH-070
- Guides: [rendering](../../guides/rendering.md)
