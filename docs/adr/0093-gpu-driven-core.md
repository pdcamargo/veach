# 0093: The GPU-driven core: a persistent GPU scene, two-phase meshlet culling, binned shading

Date: 2026-11-28

## Status

Accepted. Implements the geometry and shading core of [0060](0060-gpu-driven-visibility-buffer.md) and replaces the
interim path of [0040](0040-interim-realtime-path-visibility-buffer.md) (already superseded by 0060) in code.

## Context

ADR 0060 chose a GPU-driven visibility buffer; SP-16 ([findings](../research/spikes/sp-16-visibility-vs-forward.md))
confirmed it on dense alpha-tested foliage and listed what the engine's core needed next: culling as the engine's
(instances and meshlets, two-phase occlusion), opaque then masked lists, material binning. Until now every scene and
golden went through the interim path: the CPU uploaded every instance record each frame and issued one draw per
instance with one draw instance per meshlet, nothing culled, one shading dispatch.

Constraints that shaped the design: no count buffers (ADR 0089: compacted lists with GPU-written `instance_count`); no
mesh shaders and no 64-bit atomics on the floor hardware (ADR 0060); every position camera-relative float32 on the
render origin's 64 m grid (ADR 0045); rasterization two-sided, because single-sided triangles seen from behind must
still be drawn and shaded by their normals (ADR 0078, `rt.AC8`); a mip chain could not be built in the render graph
(ADR 0090's not-done list); the atmosphere is being reworked in parallel (ADR 0092), so shading's calls into it must
stay where they are. Spec: [gpu-driven renderer](../specs/gpu-driven-renderer/spec.md); backlog VCH-138.

## Decision

**The GPU scene** (`engine/render/src/gpu_scene.{hpp,cpp}`). Every drawn mesh (cooked, MeshStore runtime meshes,
terrain chunks alike) is one 240-byte `InstanceRecord` (`scene_records.hpp`, `shaders/veach/scene.slang`) in one
persistent device buffer, read by culling, the visibility pass, shading and photo mode. Extraction rebuilds the records
in render space every frame; `GpuScene::update` compares them with what the buffer holds and uploads only the changed
runs through the frame's `UploadRing` and one `CommandList::copy_buffer_regions` (a new RHI call). A static scene
uploads nothing while the camera moves inside its 64 m cell; crossing a cell re-uploads every record. Each record
carries its entity's previous transform, moved into this frame's render space (a whole number of cells, exact in
float), for motion vectors; a change of render frame or camera starts over without motion. Records also carry the
mesh's object-space bounding sphere and a handle to its per-meshlet bounds (sphere, normal cone: `MeshletBounds`,
already cooked). Draws with the same materials share one run of material records.

**Culling** (`shaders/gpu_cull.slang`, compute, `CullStats` read back through `ReadbackQueue`):

- Instances first (a thread each): frustum (five planes of the reverse-Z infinite projection), then occlusion. An
  instance with one meshlet is tested as a meshlet right there; a larger one becomes tasks of 64 meshlets for
  `cull_meshlets` (a group each, dispatched indirectly).
- Meshlets: frustum, the normal cone (meshoptimizer's test) only for single-sided materials under a uniform,
  non-mirroring scale (`instance_cone_culling`), then occlusion. Rasterization stays two-sided for every material.
- **Two-phase occlusion.** Phase 1 tests last frame's HiZ pyramid through last frame's view-projection moved into this
  frame's render space, draws what passes and appends the occluded instances and meshlets to recheck lists. The pyramid
  is rebuilt from phase 1's depth; phase 2 re-tests the recheck lists against it and draws what became visible into the
  same targets. One pyramid per frame: it serves phase 2 now and phase 1 next frame (it lacks only phase 2's few
  draws, which makes the next phase 1 more conservative, never wrong). Without a valid pyramid (first frame, resize,
  another camera or render frame) phase 1 tests only the frustum.
- **The pyramid** (`shaders/hiz.slang`): `r32_float`, mip 0 half the depth buffer rounded down, each texel the farthest
  (smallest, reverse-Z) depth of its pixels; the last texel of a row or column takes the odd remainder, so texel t of
  mip L covers pixels [t, t + 1) x 2^(L + 1) exactly and the test is conservative. One dispatch per mip, reading the
  mip before: **the render graph tracks a state per mip** (`RenderGraph::mips(texture, first, count)` is a resource of
  its own; `rhi::Barrier::first_mip`/`mip_count`, subresource ranges on DX12, per-mip validation on Metal).
- **Draw lists**: eight compacted lists per phase, opaque meshlets of up to 16, 32, 64 and 124 triangles then the same
  for alpha-tested ones (`draw_list_of`), each with exact capacity computed on the CPU from per-slot meshlet counts. Each
  list is one non-indexed `draw_indirect` whose `instance_count` the GPU counts with atomics and whose vertex count is
  3 x the bucket's triangle limit, so a 12-triangle grass card runs 48 vertex invocations, not 372.
- Counts become dispatch arguments in a one-thread `cull_args` pass; every indirect dispatch comes in rows of 32,768
  groups (D3D12 caps a row at 65,535).

**Visibility** (`shaders/realtime_visibility.slang`): the lists of each phase in one render pass, opaque first with
`vis_fs`, then alpha-tested with `vis_fs_masked`. It keeps 0040's `rg32_uint` `{instance, meshlet << 7 | triangle}` and
reverse-Z depth, and adds an `rg16_float` **motion** target: previous minus current position in UV (y down), from each
vertex's previous transform and last frame's view-projection. No jitter yet (ADR 0061 adds it with TAA).

**Binned shading.** `shaders/classify.slang` (a group per 8x8 tile) puts each tile in every bin it shows (bin 0: no
geometry, the sky; bin k + 1: material kind k; at most 31 kinds) and completes motion where no geometry is (the camera's
rotation). `classify_args` turns the counts into one indirect dispatch per bin. Each bin has its own entry point,
generated per app with its materials by `veach_shade_bins()` (`realtime_shade_bins.slang`, `cmake/VeachShaders.cmake`),
calling `shade_bin` in `shaders/realtime_shade.slang` with the bin as a constant, so `evaluateSurface` folds to one
material. The lighting model, the surface reconstruction and the calls into the atmosphere are 0040's, unchanged, in
one function (`surface_pixel`, `sky_pixel`).

**The frame blackboard** (0060's contract item 1, its first part): `FrameContext::blackboard` carries the frame's
`visibility`, `depth`, `motion` and `hdr_color` for features recorded after the scene renderer (TAA next).

## Consequences

- Every scene and golden draws through this path; every real-time golden's image is the interim path's (mean FLIP
  0.00000 between the two release builds), so none was regenerated. Photo mode reads the same GPU scene and is otherwise unchanged.
- The CPU still walks every instance each frame (extraction, records, the comparison): fine for thousands, about
  31 ms per frame for SP-16's 467,767 entities (the interim path took 102 ms). A static-instance path (records written once, changes from the ECS)
  is the follow-up when such counts are real content.
- Draw order inside a list follows atomics, so it varies run to run; coplanar overlapping surfaces may pick a different
  triangle per run. Goldens and the byte-identical frames test are unaffected.
- No draw distance or LOD: distant vegetation costs every triangle (SP-16's next item). No single-pass pyramid build.
- Kinds are capped at 31 by the classification header; the cap is checked at prepare with a clear error.
- Revisit if: the vertex work of partly filled meshlets dominates (index-buffer meshlets or mesh shaders where
  available), the pyramid's missing phase-2 occluders cost measurable culling (rebuild after phase 2), or bins grow so
  many that one indirect dispatch per bin costs more than it saves.

## Alternatives considered

- **Visibility bits instead of last frame's pyramid** (draw what was visible last frame first, test everything against
  this frame's pyramid after): needs a persistent bit per instance and per meshlet keyed by stable slots across
  frames; the GPU scene's records are positional and reorder when entities come and go.
- **Rebuild the pyramid after phase 2 as well:** a second build per frame for occluders phase 1 rarely lacks.
- **The pyramid in a buffer or as one texture per mip** (as SP-16 did): sidesteps the graph instead of fixing it; every
  later mip chain (bloom, exposure, SPD) needs per-mip states anyway.
- **One shading dispatch with the kind read per pixel** (as 0040): no specialization; every pixel pays the full
  material switch's registers. Per-pixel bins (sorted pixel lists) cost a sort for a gain tiles already give.
- **Indexed draws per mesh** (one `draw_indexed_indirect` per mesh with a compacted instance list, as SP-16 per part):
  vertex reuse, but no meshlet culling and one record per mesh; Metal encodes one call per record (ADR 0089).
- **Back-face culling for single-sided materials in the rasterizer:** glTF's rule, but it drops the upright transition
  triangles ADR 0078 must shade from behind; the cone test removes only meshlets facing entirely away.
