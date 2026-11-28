# RHI textures and raster state: texture kinds, texture updates, blending, depth bias, samplers

**Status:** Implemented
**Area:** rhi
**Owner:** dev lead

## Summary

3D textures, 2D texture arrays and cube maps on Metal, DX12 and null, reached through bindless `DescriptorHandle`s and
written from compute; texture updates after creation through the render graph; and the pipeline and sampler state
transparency, shadow maps and terrain materials need: blend modes, depth bias, comparison samplers and anisotropic
filtering. Criteria continue the area's numbering after [gpu-driven-rhi](../gpu-driven-rhi/spec.md) (rhi.AC1 to AC7).

## Motivation

The renderer of [ADR 0060](../../adr/0060-gpu-driven-visibility-buffer.md) needs blend state for forward transparency;
its shadows ([ADR 0062](../../adr/0062-shadows-ambient-reflections.md)) need depth bias, comparison samplers and a
texture array of cascades; the atmosphere's aerial-perspective froxel volume
([ADR 0076](../../adr/0076-amendments-from-sp-10.md), [ADR 0082](../../adr/0082-amendments-from-sp-14.md)) needs 3D
textures; terrain material layers need texture arrays and anisotropic filtering; streamed chunks and virtual textures
need partial texture uploads; a space backdrop needs a cube map. The [RHI audit](../../reference/rhi-capabilities.md)
lists them as VCH-072, VCH-076 and VCH-098.

## Requirements

- MUST create 3D textures, 2D arrays, cube maps and cube arrays on every backend, validated the same way everywhere.
- MUST read each through a `DescriptorHandle` as its Slang type, and write 2D, 3D and array textures (cube faces as
  layers) from compute as storage.
- MUST give descriptors of a run of mips and layers, and null descriptors for every texture kind.
- MUST let a render pass draw into one mip and one layer of a texture (a cascade, a cube face, a 3D slice).
- MUST take initial data for every layer of every mip, tightly packed in KTX2 order.
- MUST copy from a buffer into any box of one mip over a run of layers, and stage such copies through recycled upload
  memory in render-graph transfer passes whose barriers the graph derives.
- MUST offer per-target blend modes (opaque, alpha, premultiplied, additive), constant and slope depth bias,
  comparison samplers with 2x2 PCF under linear filtering, and anisotropic filtering up to 16x, with a default for
  terrain.
- MUST make Metal samplers filter between mips as D3D12's do.
- SHOULD keep every new GPU case small enough for WARP in hosted CI.

## API sketch

```cpp
// Texture kinds and views
rhi::TextureDesc froxels{.width = 160, .height = 90, .format = rhi::Format::rgba16_float,
                         .usage = rhi::TextureUsage::storage | rhi::TextureUsage::sampled,
                         .debug_name = "aerial perspective", .type = rhi::TextureType::d3, .depth = 32};
rhi::TextureDesc sky{.width = 512, .height = 512, .type = rhi::TextureType::cube, .layers = 6};  // +X -X +Y -Y +Z -Z
device.descriptor(texture);                                         // Texture3D / TextureCube / Texture2DArray
device.descriptor(texture, rhi::View::storage);                     // RWTexture3D / RWTexture2DArray (cube faces)
device.descriptor(texture, rhi::View::read, {.base_mip = 2, .mip_count = 1, .base_layer = 3, .layer_count = 1});
device.null_descriptor(rhi::DescriptorKind::texture_3d);

// Draw into one layer
b.depth_target(cascades, rhi::LoadOp::clear, 0.0f, {.mip = 0, .layer = cascade});

// Updates
render::upload_texture(graph, *frame.uploads, texture, {.mip = 0, .base_layer = 2, .x = 64, .width = 64, .height = 64},
                       bytes, "stream layer 2");
cmd.copy_buffer_to_texture(buffer, offset, row_pitch, texture, region);  // 512-byte offsets, 256-byte pitches

// Raster and sampler state
desc.blend = {rhi::BlendMode::premultiplied};
desc.depth_bias = {.constant = -2, .slope = -1.5f, .clamp = 0.0f};   // reverse-Z: negative pushes away
device.create_sampler({.compare = rhi::CompareOp::greater_equal});   // SamplerComparisonState, SampleCmp
device.create_sampler({.address = rhi::AddressMode::repeat, .max_anisotropy = rhi::terrain_anisotropy});
```

## Acceptance criteria

- **rhi.AC8** — 3D textures: compute writes every texel of mip 0 through the whole-texture storage view and of mip 1
  through a view of that mip; sampling at texel centers returns them exactly, and linear filtering between two slices
  blends them. Texture kinds are validated identically on every backend (null test).
- **rhi.AC9** — 2D arrays: per-layer data uploaded at creation samples back per layer and per mip; views of a run of
  layers and of one mip start where they say; a render pass cleared into layer 3 of mip 1 changes only that slice.
  Views outside the texture are reported (`rhi.invalid_texture`); transients are pooled by their whole shape.
- **rhi.AC10** — cube maps: six faces sample back by direction; compute writes a storage cube's faces as layers.
- **rhi.AC11** — null descriptors of the 3D, array and cube kinds read zero.
- **rhi.AC12** — texture updates: `upload_texture` into a box of mip 1 of two array layers, and into a box of a 3D
  texture, changes exactly that box; regions are resolved and checked (`resolve_region`), the graph derives the
  transfer pass's barriers.
- **rhi.AC13** — the upload ring aligns allocations to 512 bytes, reuses a frame slot's share only when that slot
  comes round, and sends requests beyond the share to a buffer of their own.
- **rhi.AC14** — comparison samplers: `SampleCmpLevelZero` passes or fails per texel; with linear filtering the edge
  between a lit and a shadowed texel reads 0.5 (2x2 PCF).
- **rhi.AC15** — anisotropic filtering of a footprint stretched 8:1 reads a mip at least 1.5 levels finer than
  trilinear filtering; invalid anisotropy and blending or bias settings are rejected on every backend.
- **rhi.AC16** — opaque, alpha, premultiplied and additive blending give their formulas' colors within 2.5/255.
- **rhi.AC17** — a coplanar second draw passes a `greater` depth test with a positive constant bias or a slope bias
  on a sloped triangle, and fails with no bias, a negative bias, or a slope bias on a flat triangle.
- **rhi.AC18** — the lab's blend demo (`scripts/blend_modes.luau`) matches `tests/golden/blend_modes.png` on Metal and
  on DX12 (WARP).

Every GPU criterion runs on Metal in the pre-push gate and on DX12/WARP in hosted CI (`test::gpu_device()`).

## Out of scope

- Per-subresource states in the render graph: a pass cannot read one mip of a texture while writing another
  (compute mip-chain generation, Hi-Z builds). Every barrier covers the whole texture. Add it with the first feature
  that needs it (the Hi-Z of ADR 0060).
- Depth clamping (shadow pancaking), stencil, MSAA, signed-integer and other BC formats, sparse or virtual textures.
- KTX2 arrays and cube maps in the texture cooker: `read_ktx2` still reads 2D textures only; the RHI already takes the
  KTX2 level layout, so the cooker change is local (with the terrain material layers, VCH-130).
- Changing the scene renderer's material sampler to anisotropic: its goldens would move on purpose with the renderer
  of ADR 0060.

## Open questions

None.

## Links

- ADRs: [0090](../../adr/0090-rhi-texture-kinds-updates-and-raster-state.md),
  [0060](../../adr/0060-gpu-driven-visibility-buffer.md), [0062](../../adr/0062-shadows-ambient-reflections.md),
  [0076](../../adr/0076-amendments-from-sp-10.md), [0027](../../adr/0027-resource-binding-model.md),
  [0029](../../adr/0029-render-graph-sync-and-barriers.md)
- Backlog: VCH-072, VCH-076, VCH-098 (done)
- Guides: [rendering](../../guides/rendering.md), [RHI audit](../../reference/rhi-capabilities.md)
- Tests: `tests/rhi_texture_tests.cpp`, `tests/shaders/{texture_kinds,sampler_states,blend_states}.slang`
