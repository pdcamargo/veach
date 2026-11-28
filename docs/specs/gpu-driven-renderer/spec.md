# GPU-driven real-time renderer: the core

**Status:** Implemented
**Area:** render, rhi
**Owner:** dev lead

## Summary

The geometry and shading core of the real-time renderer ADR 0060 chose: a persistent GPU scene, culling of instances
and meshlets on the GPU with two-phase occlusion, a visibility buffer drawn from compacted indirect lists that also
writes motion vectors, and shading binned by material kind. It replaces the interim path (ADR 0040) for every scene,
golden and app, with the same images.

## Motivation

The interim path drew every meshlet of every instance, one draw per instance, and re-uploaded every instance record
each frame: Bistro's visibility pass was most of its frame, and the terrain's visibility pass grew with every chunk
near the ground. SP-16 ([findings](../../research/spikes/sp-16-visibility-vs-forward.md)) confirmed ADR 0060 and listed
this core as the next step. The temporal accumulation of ADR 0061 needs its motion vectors.

## Requirements

- Every drawn mesh (cooked, runtime, terrain chunk) MUST be one record in a persistent GPU buffer, in render space,
  and only the records that changed since the last frame MUST be uploaded.
- Instances and then meshlets MUST be culled on the GPU against the frustum and a HiZ pyramid, and meshlets of
  single-sided materials under a uniform, non-mirroring transform against their normal cone.
- Occlusion MUST run in two phases: last frame's pyramid first, then what it found occluded against the pyramid of
  this frame's phase-1 depth, drawn in the same frame, so a camera that moves or jumps never shows a hole.
- Survivors MUST be drawn by indirect draws whose instance counts the GPU writes (ADR 0089), opaque lists before
  alpha-tested ones, rasterized two-sided (ADR 0078).
- The visibility pass MUST keep 0040's `rg32_uint` format and reverse-Z depth, and MUST write each pixel's motion
  (previous minus current position in UV) from each instance's previous transform and the previous view; where there
  is no geometry, the camera's rotation gives it.
- Shading MUST be binned: 8x8 tiles listed per bin (the sky, and each material kind), one indirect dispatch per bin
  with its own entry point. The lighting model, surface reconstruction and atmosphere calls MUST be the interim
  path's, so the images match it.
- The render graph MUST track a state per mip, so a pass can read one mip while it writes the next.
- The frame's visibility, depth, motion and HDR color MUST be available to features recorded later (the frame
  blackboard), and what culling did MUST be readable (`CullStats`, MCP `gpu_timings`).
- Nothing MUST need ray tracing; photo mode MUST read the same GPU scene and stay unchanged.

## API sketch

```cpp
// Apps: the shading library is now realtime_shade_bins (an entry point per material kind, veach_add_scene_shaders).
renderer.add_feature(std::make_unique<render::SceneRenderer>(
    render::SceneShaders{lab_shaders::path_tracer(), lab_shaders::realtime_visibility(),
                         lab_shaders::realtime_shade_bins()},
    lab_shaders::material_kinds, assets));

// A later feature (TAA) reads the frame's targets:
const std::optional<render::GraphResource> motion = frame.blackboard->get("motion");  // also visibility, depth, hdr_color

// Mip chains in the graph:
const GraphResource mip = graph.mips(pyramid, level);  // a resource of its own; ctx.range(mip) for the view

// What culling did, frames_in_flight frames ago:
const render::CullStats& stats = scene_renderer.cull_stats();  // instances_visible, meshlets_drawn[2], ...
```

## Acceptance criteria

- **gpudriven.AC1** — A run of mips is a graph resource of its own: passes read mip n - 1 while writing mip n, with a
  barrier per mip run; a pass touching a mip twice is rejected; mips left apart end in one state.
- **gpudriven.AC2** — The GPU scene uploads every record the first time, nothing for a still scene or a camera moving
  inside its render cell, one record for an instance that moved (and once more when it stops), every record when
  render space moves to another cell.
- **gpudriven.AC3** — Culling drops an instance behind the camera (frustum) and one behind a wall (rechecked in phase
  2, still hidden), draws only the visible meshlets, and the image equals the one drawn before any pyramid existed.
- **gpudriven.AC4** — A camera that jumps to see what the last frame's pyramid hid draws it in the same frame: the image
  equals a first frame from that camera, and phase 2 reports the disoccluded instance.
- **gpudriven.AC5** — Motion vectors: a moving box's pixels hold where it was last frame; still geometry and the sky
  hold zero.
- **gpudriven.AC6** — Every real-time golden passes through this path unregenerated (Cornell box with two material
  kinds and the sky in their own bins, FlightHelmet with and without ray tracing, runtime mesh, thin triangles, the
  frames goldens, blend modes, space backdrop, Bistro at noon and sunset), on Metal and on DX12/WARP (Bistro on Metal).

## Out of scope

- Temporal accumulation and jitter (ADR 0061, the next task), shadows, clustered lights, transparency (ADR 0060).
- LOD and draw distances for distant vegetation; a single-pass HiZ build; a static-instance path that writes records
  once instead of comparing them every frame (all in VCH-137's follow-ups).

## Open questions

None.

## Links

- ADRs: [0093](../../adr/0093-gpu-driven-core.md), [0060](../../adr/0060-gpu-driven-visibility-buffer.md),
  [0089](../../adr/0089-indirect-work-and-fixed-latency-readback.md), [0078](../../adr/0078-sidedness-decides-the-shading-surface.md),
  [0045](../../adr/0045-large-world-positions-and-reference-frames.md)
- Backlog: VCH-137
- Guides: [rendering](../../guides/rendering.md)
- Measurements: [GPU-driven core against the interim path](../../research/gpu-driven/README.md)
