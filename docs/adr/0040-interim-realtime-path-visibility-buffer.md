# 0040: The interim real-time path: the visibility buffer with one basic shading pass

Date: 2026-11-27

## Status

Accepted. Rewrites the pathtracer-era visibility-buffer renderer (old ADR 0037) without its ray-traced and temporal
parts. Expected to be superseded by the research phase's renderer ADR (open question 7).
Superseded by [0060](0060-gpu-driven-visibility-buffer.md).

## Context

The reset removed the real-time renderer's lighting (ReSTIR-style direct light with shadow rays, the ambient
stopgap's AO rays, the denoiser, the upscalers). The research phase will decide the real PBR rasterizer
([0039](0039-realtime-renderer-is-raster-pbr.md)), forward+/clustered or visibility buffer. Until then the engine
still needs a real-time image of real scenes for visual verification, goldens and the lab, and that image must not
need ray tracing ([0003](0003-platforms-and-hardware-target.md)).

What survives from the old renderer is raster-only and works on both backends and WARP: cook-time meshlets
([0011](0011-gltf-import-and-cooking.md)), a visibility pass that rasterizes them by vertex pulling, and surface
reconstruction from the visibility buffer with the shared materials.

## Decision

**Keep the visibility buffer for now; reduce shading to one pass.** Each frame, for a camera without `PhotoMode`
(`engine/render/src/realtime_renderer.cpp`):

1. **Visibility** (`realtime_visibility.slang`, graphics): one draw per instance, one draw instance per meshlet,
   `124 × 3` vertices with vertex pulling. Each pixel stores `{instance, meshlet << 7 | triangle}` in an `rg32_uint`
   target, reverse-Z depth. Masked materials are alpha-tested with `evaluateAlpha`; blended ones at 0.5.
2. **Shade** (`realtime_shade.slang`, compute, compiled per app with its materials): rebuild the surface from the
   triangle, exact barycentrics from the pixel's ray and UV gradients from the neighbors' rays, `evaluateSurface`,
   then:
   - every punctual light, exactly and **unshadowed**; a light behind the geometric surface contributes nothing, so
     normal maps cannot leak light; light sizes are ignored;
   - **ambient**: the sky's cosine-weighted irradiance texel ([0038](0038-physical-sky.md)) on the diffuse lobe,
     unoccluded;
   - emission seen directly (emissive surfaces do not light other surfaces);
   - the sky behind geometry and aerial perspective in front of it;
   - a shading normal facing away from the camera (normal maps at grazing angles) is bent back toward it.
3. **Display**: AgX on the exposed radiance ([0037](0037-exposure-and-agx.md)).

All targets are transient; nothing is carried between frames, nothing is jittered, and the frame renders at output
resolution. BLASes and the TLAS are not built unless a camera uses photo mode.

## Consequences

- Any GPU at the engine's floor renders real scenes in real time; a golden renders the same image with ray tracing
  disabled (rt.AC2).
- The image is basic on purpose: no shadows, no indirect light or AO, no reflections (metals look dark), no
  transparency. Goldens of it (`*_realtime.png`, Bistro) check the pipeline, not the look.
- If the research phase picks forward+/clustered, the visibility pass and the shade pass go, and the cooked meshlets
  stay only if something else uses them (GPU culling, for example).

## Alternatives considered

- **Delete the visibility buffer and keep only the lab's triangle.** No real-time image of a real scene until the new
  renderer exists: visual checks of anything glTF would need ray tracing.
- **Write a simple forward renderer now.** New code whose fate also depends on the research phase; the visibility
  path already exists and is tested on both backends.
- **Keep ray-traced shadows in the interim path.** Would make the real-time path depend on ray tracing, which ADR 0003
  rules out.
