# Real-time renderer v0

**Status:** Draft
**Area:** render, asset, rhi
**Owner:** dev lead

## Summary

The renderer Veach ships with, not a preview. Geometry is split into meshlets at cook time and rasterized into
a visibility buffer; one compute pass shades each pixel with the same Slang materials, BRDF and light sampling
as the reference path tracer. Every light type lights the scene directly, with ray-traced hard shadows. A
temporary ambient term with ray-traced AO keeps shadows from going black until Candela exists. Exposure comes
from a physical camera and one AgX display transform serves both renderers. Per-pass GPU timings are available
over MCP, and a harness measures the real-time direct lighting against the path tracer's.

## Motivation

The path tracer is ground truth, at seconds per image. Games need the same look at 60 fps at 1440p (VISION).
Tessera (virtualized geometry) and Candela (GI) build on this renderer: Tessera takes over its meshlets,
Candela replaces its ambient stopgap. Roadmap: VISION item 5.

## Requirements

**Geometry** ([ADR 0037](../../adr/0037-realtime-renderer-visibility-buffer.md))
- The cooker MUST split each primitive into meshlets with meshoptimizer (≤ 64 vertices, ≤ 124 triangles, one
  material slot each) and store meshlets, meshlet vertices, meshlet triangles and bounds in `MeshAsset`, with
  the LOD fields Tessera will fill (level 0, no parent, in v0). The cooker version changes, so caches rebuild.
- The renderer MUST rasterize every instance's meshlets into an `rg32_uint` visibility buffer
  (`{instance, meshlet << 7 | triangle}`) with reverse-Z depth, by vertex pulling (no mesh shaders, no vertex
  input layouts). Alpha-masked materials MUST be tested with the shared `evaluateAlpha`.
- The RHI MUST gain what this needs: depth attachments in render passes, depth test and write state and cull
  mode in graphics pipelines, and the `rg32_uint` format, on Metal and DX12.

**Shading**
- One compute pass MUST shade every pixel: rebuild the triangle, intersect the pixel's camera ray for
  barycentrics, derive UV gradients from the neighboring pixels' rays, call the generated `evaluateSurface`,
  and light it.
- Surface reconstruction, the BRDF, light sampling and shadow rays MUST live in shared Slang modules used by
  both renderers. A material is written once (`veach_add_material`) and MUST render in both.
- `SurfaceInput` MUST carry UV gradients; texture sampling uses them (`SampleGrad`). The path tracer passes
  zero gradients (mip 0), as today.

**Lighting**
- Directional, point and spot lights: evaluated exactly per pixel, each with one shadow ray (hard shadows).
- Emissive surfaces: `emissive_samples` (default 4) light samples per pixel from the emissive-triangle
  distribution, with shadow rays; emission seen directly.
- **Stopgap, to be replaced by Candela:** ambient = sky radiance × diffuse albedo × AO, with AO from `ao_rays`
  (default 4) cosine rays of length `ao_radius` (default 1 m). Candela replaces it; at most a short-range AO
  stays, for detail Candela misses. Settings live in a `RealtimeLighting` component (Scriptable) on the camera.
- The path tracer MUST NOT apply AO. It MAY show AO as a debug view (`PathTracer.mode = ao`).

**Exposure and display** ([ADR 0038](../../adr/0038-exposure-and-tonemapping.md))
- `Camera` gains `aperture`, `shutter` and `iso`; `ev100 = 0` means "derive it from them".
- Both renderers MUST write linear radiance to an `rgba16_float` HDR target; one display-transform pass applies
  the exposure and AgX and writes sRGB `scene_color`.

**Measurement**
- Per-pass GPU timings (VCH-049): every render-graph pass is timed with GPU timestamps (Metal counter sample
  buffers at encoder boundaries, DX12 timestamp queries). MCP `gpu_timings` returns them by pass name.
- The path tracer MUST gain a direct-lighting-only mode (`PathTracer.mode = direct`: next-event estimation and
  BSDF-sampled hits on emitters at the first bounce, no indirect light, no sky).
- A comparison harness MUST render each scene with the real-time renderer (ambient and AO off) and with the
  converged direct-only path tracer, and report mean FLIP and the 99.9th-percentile FLIP per scene.
- This spec MUST report frame times at 1440p on the development machine (Apple M5 Max) for the Cornell box,
  FlightHelmet, Sponza and Bistro, with the per-pass split.

**Out of scope**
- Soft shadows, area-light penumbrae and denoising (next milestone, VCH-021/VCH-022); GPU culling, LOD and
  streaming (Tessera, VCH-016); global illumination (Candela, VCH-017); upscaling (VCH-023); automatic
  exposure; transparency beyond alpha masking (blended materials render as masked at 0.5 in v0).

## Acceptance criteria

- **rt.AC1**: Cooked meshes carry meshlets: every triangle is in exactly one meshlet, meshlets respect the
  64/124 limits and one slot each, and the LOD fields are level 0.
- **rt.AC2**: The real-time Cornell box and FlightHelmet match their goldens on Metal and DX12/WARP
  (`golden: ... (realtime)`), with mean FLIP and a 99.9th-percentile limit.
- **rt.AC3**: `GlowMaterial` renders the same in both renderers: a view filled by the glow block's emissive face
  matches between the real-time renderer and the path tracer (FLIP below the golden tolerance).
- **rt.AC4**: Each light type casts a hard ray-traced shadow: a test scene per type shows lit and shadowed
  pixels where geometry says they must be.
- **rt.AC5**: The ambient stopgap is off by default in comparisons, never applied by the path tracer, and the
  path tracer's AO debug view renders.
- **rt.AC6**: Exposure follows the camera: doubling the shutter time doubles the pre-tonemap value, and `ev100`
  derived from aperture, shutter and ISO matches the formula.
- **rt.AC7**: MCP `gpu_timings` lists every pass of the frame with a GPU time, on Metal and DX12.
- **rt.AC8**: The comparison harness reports per-scene mean and 99.9th-percentile FLIP between the real-time
  direct lighting and the converged direct-only path tracer.
- **rt.AC9**: Frame times at 1440p on the M5 Max for the four scenes are reported below.
- **rt.AC10**: With reverse-Z depth (test `greater`, write on, the depth target cleared to 0) the nearer of two
  overlapping triangles wins in any draw order, a back face (clockwise) is culled with `CullMode::back`, and a
  pipeline that tests depth without a depth format is rejected (`rhi.invalid_pipeline`), on Metal and DX12.
- **rt.AC11**: An `rg32_uint` target clears to `0xFFFFFFFF` in both channels and stores exactly the integers a
  fragment shader writes, on Metal and DX12.

## Results

Filled in when implemented: per-scene frame times (1440p, M5 Max) with the per-pass split, and per-scene FLIP
against the direct-only reference.

## Links

- ADRs: [0037 visibility buffer](../../adr/0037-realtime-renderer-visibility-buffer.md),
  [0038 exposure and tonemapping](../../adr/0038-exposure-and-tonemapping.md),
  [0033 materials](../../adr/0033-material-model.md), [0034 path tracer](../../adr/0034-lights-and-reference-path-tracer.md)
- Backlog: VCH-056 (this spec), VCH-049 (per-pass timings), VCH-016 Tessera, VCH-017 Candela, VCH-021, VCH-022
