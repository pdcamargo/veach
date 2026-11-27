# 0060: The real-time renderer: a GPU-driven visibility buffer with clustered lights and forward transparency

Date: 2026-11-27

## Status

Accepted. Supersedes [0040](0040-interim-realtime-path-visibility-buffer.md).

## Context

ADR 0040 kept the visibility buffer as an interim path with one unshadowed shading pass, pending the research phase's
choice between a visibility buffer and clustered forward ([0039](0039-realtime-renderer-is-raster-pbr.md)). The toolbox
needs terrain, dense alpha-tested vegetation, many small lights, transparency (water, glass, particles) and planets from
the ground to orbit, on Apple TBDR GPUs and DX12, without ray tracing ([0003](0003-platforms-and-hardware-target.md)).
Research: [08-renderer](../research/08-renderer.md).

Measured today on the M5 Max (Metal, render-graph timestamps): Bistro exterior at 1080p spends 5.7 to 7.0 ms in the
visibility pass and 0.73 to 0.80 ms in shading, out of a 6.9 to 8.1 ms frame, because every meshlet of every instance
is drawn with no culling. Sponza at 1440p: 1.18 ms visibility, 1.46 ms shade.

What decides it:

- **Vegetation on TBDR.** Alpha-tested draws defeat hidden-surface removal. In clustered forward every overlapping
  foliage layer runs the full material and lighting shader; in a visibility buffer a masked fragment does one alpha
  fetch and writes 8 bytes, and the pixel is lit once.
- **Bandwidth is not the limit.** The 8-byte visibility buffer is about 2 GB/s at 1080p60, 3% of an M1's 68 GB/s.
  Engel measured it ahead of a G-buffer at every resolution.
- **Transparency is forward in every option**, so the light list must serve both a compute shade and a fragment pass.
- **MSAA** is where clustered forward wins; a visibility buffer gets its anti-aliasing from
  [0061](0061-temporal-accumulation-no-upscaling.md) instead.

## Decision

**Opaque and masked geometry go into the visibility buffer.** The pass keeps 0040's format: `rg32_uint` holding
`{instance, meshlet << 7 | triangle}`, reverse-Z depth, vertex pulling of cook-time meshlets
([0011](0011-gltf-import-and-cooking.md)) and of runtime meshes (terrain chunks, VCH-102) built in the same meshlet
layout. Opaque draws come first, then masked ones, never interleaved.

**GPU-driven geometry.** A compute pass culls instances and then meshlets (frustum, normal cone, Hi-Z occlusion from
the previous frame's depth, re-tested against the current one) and writes indirect draw arguments. It needs indirect
draw and dispatch (VCH-071). Until VCH-071 lands, culled counts may be read back by the CPU a frame late.

**Shading is compute, binned by material kind.** A classification pass sorts pixels into one bin per material kind
(`evaluateSurface`'s switch, [0035](0035-material-model.md)) and dispatches each bin indirectly. Shading reconstructs
the surface from the triangle as 0040 does (exact barycentrics from the pixel's ray, UV gradients from the neighbors'
rays) and evaluates lights, shadows and ambient ([0062](0062-shadows-ambient-reflections.md)) and the atmosphere
([0063](0063-astronomical-rendering.md)). It writes pre-exposed radiance into `hdr_color` (`rgba16_float`).

**Clustered lights.** A z-binned light grid (Drobot-style: a 1D z-bin array plus 2D screen tiles) built each frame in
`frame_setup`, from camera-relative light positions, shared by the compute shade and the forward pass. Planetshine and
other module lights are records in the same list.

**Transparents are forward.** Water surfaces, glass and particles draw after opaque shading in a fragment pass, sorted
back to front per draw, reading the same light grid, shadows and atmosphere. It needs blend state (VCH-098).

**The RenderFeature contract grows three things:**

1. **A frame blackboard** of named resources in `FrameContext`, declared like any other render-graph access so
   barriers and residency stay derived ([0029](0029-render-graph-sync-and-barriers.md)): `hdr_color`, `depth`,
   `visibility`, `motion`, `light_grid`, `shadow_cascades`, `sky`, `exposure`, `camera_medium`, `far_layer`.
2. **Stages** for writes to shared targets, in this order: `frame_setup`, `shadows`, `opaque`, `lighting_inputs`,
   `far_layer`, `transparent`, `volumetric`, `post_hdr`, `post_display`. Within a stage, registration order decides.
3. **Shading contributions as data:** modules add lights, analytic occluders, decals and fog volumes to buffers the
   shade and forward passes already read. No feature patches the shade kernel.

**Not used:** MSAA, mesh shaders, a compute rasterizer (it needs 64-bit atomics, which Metal reportedly offers only from
M2-class GPUs), a G-buffer. Every position the GPU sees is camera-relative float32
([0045](0045-large-world-positions-and-reference-frames.md)). Photo mode ([0041](0041-photo-mode.md)) is unchanged and
shares the materials.

## Consequences

- Foliage costs one alpha fetch per covered layer, not one full shade.
- The engine gains RHI work before it gains the renderer: VCH-071 (indirect), VCH-098 (blend state, depth bias,
  comparison samplers, anisotropic filtering), VCH-072 (3D textures, arrays, cube maps) for later features.
- Every material, debug view and golden follows the compute shade model; this is expensive to reverse once many
  materials exist.
- 0040's limits (no shadows, no AO, no reflections, no transparency) go away with the features that replace them;
  0040's goldens are regenerated on purpose as each lands.
- **Validation:** VCH-092 (SP-16): the same vegetation field drawn by this renderer and by a throwaway clustered forward
  renderer, ms in Tracy at 1080p and 1440p on the M5 Max, after VCH-071. Culling must cut Bistro's visibility pass at
  1440p by at least half with the golden unchanged by FLIP. Real M1-class and M3 Pro-class numbers come from VCH-097
  (SP-21).
- **Reopen if** VCH-092 measures clustered forward faster on the vegetation field, or temporal anti-aliasing ghosting
  on thin foliage is unacceptable by eye so that MSAA becomes necessary (which favors forward).

## Alternatives considered

- **Clustered forward (forward+) with MSAA.** Natural MSAA and alpha-to-coverage, but every foliage layer runs the full
  shader on TBDR, and our dominant content is foliage.
- **Deferred G-buffer.** On Apple it can stay in tile memory, but on DX12 it goes to memory, and we want one path both
  backends verify; it stays a possible Metal-only optimization behind the same interfaces.
- **Nanite-style compute rasterization.** Needs 64-bit atomics, reportedly missing on the M1 class (unverified).
- **Keep 0040 unchanged.** No shadows, transparency, AO or reflections, and no culling.
