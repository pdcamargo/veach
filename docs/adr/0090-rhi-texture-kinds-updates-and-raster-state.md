# 0090: Texture kinds as types with layer-addressed faces, uploads through a frame-recycled ring, raster state as presets

Date: 2026-11-28

## Status

Accepted.

## Context

The renderer of [0060](0060-gpu-driven-visibility-buffer.md), its shadows ([0062](0062-shadows-ambient-reflections.md)),
the atmosphere's froxel volume ([0076](0076-amendments-from-sp-10.md)) and terrain material layers need what the RHI
lacked ([audit](../reference/rhi-capabilities.md)): 3D textures, arrays and cube maps (VCH-072), texture updates after
creation (VCH-076), and blend state, depth bias, comparison samplers and anisotropic filtering (VCH-098). The backends
differ where it matters:

- Metal has a texture type per shape and reaches parts of a texture through texture views; D3D12 has one resource
  dimension and a view descriptor per shape and part. Metal shaders cannot write a cube as a cube; HLSL writes its faces
  as a 2D array.
- D3D12 copies from buffers only at 512-byte offsets with 256-byte row pitches; Metal accepts that layout.
- D3D12's depth bias constant is an integer; Metal's is a float with the same meaning. D3D12 samplers always filter
  between mips; Metal's default sampler ignores them.

## Decision

1. **A texture's shape is its `TextureType`** (`d2`, `d2_array`, `d3`, `cube`, `cube_array`), and the type is the
   Slang type its read descriptor has. A one-layer array stays an array, so a shader's type never depends on a count.
   `depth` is only for `d3`, `layers` only for arrays and cubes.
2. **Cube faces are layers**, 6 per cube in the order +X, -X, +Y, -Y, +Z, -Z, as in D3D12, Vulkan and KTX2. Uploads,
   render targets and storage writes address a face as a layer; storage views of cubes are `RWTexture2DArray`.
3. **Parts of a texture are views, made once and owned by the texture**: `descriptor(texture, view, TextureRange)`
   returns a bindless handle to a run of mips and layers, cached per range and freed with the texture. A part of a
   cube reads as a 2D array. Storage views see one mip. Render passes name one mip and layer (`TargetSlice`).
4. **Initial data and copies use KTX2's level layout**: per mip, layers, then depth slices, then rows. A KTX2 level is
   one span, with no reshuffling in the cooker or loader.
5. **Updates go through `render::UploadRing` and the render graph.** The ring is one upload buffer split into a share
   per frame in flight; a share is reused only when its frame slot comes round, after `begin_frame` has waited for
   the frame that used it ([0030](0030-frame-and-sync-model.md)), so no fences are tracked per allocation. Requests
   beyond a share get a buffer of their own, destroyed when the slot comes round. `render::upload_texture` stages
   tightly packed data with D3D12's alignment and adds a transfer pass, so barriers stay derived
   ([0029](0029-render-graph-sync-and-barriers.md)). Graph barriers still cover whole textures.
6. **Blend state is four presets per color target**: opaque, straight alpha, premultiplied alpha, additive (which
   keeps the destination's alpha). Depth bias is `{constant (integer steps), slope, clamp}`, with D3D12's meaning on
   both backends. Comparison samplers are `SamplerDesc::compare`, in the depth test's `reference op texel` order.
7. **Anisotropic filtering defaults to 8x for terrain** (`rhi::terrain_anisotropy`); samplers stay at 1x unless asked.
   8x is where the returns flatten on grazing ground; the cost on Apple TBDR GPUs is measured with the renderer
   (VCH-097).
8. **Metal samplers filter between mips like D3D12's** (`mipFilter` follows `filter`). This is a fix: Metal sampled
   mip 0 of every minified texture.

## Consequences

- The froxel volume, cascades in a 2D array, terrain layers in arrays and a backdrop cube map are expressible today
  on Metal, DX12 and null, each checked by a test on Metal and on WARP.
- Textured real-time goldens changed once (the mip fix): less aliasing, and closer to what WARP draws.
- A pass cannot yet read one mip of a texture while writing another; mip-chain and Hi-Z builds need per-subresource
  graph states first.
- Blending beyond the presets (min/max, separate write masks, dual source) needs a new preset or a full blend
  description; nothing asks for one.
- The ring's share size (8 MB per frame by default) bounds what a frame uploads without an extra buffer; streaming
  will tell whether it should grow.

## Alternatives considered

- **Array-ness from the layer count** (one-layer arrays as 2D): fewer types, but a shader's binding would change type
  when a count reaches 1, which D3D12 does not tolerate.
- **Cube faces as a separate address** (`face` next to `layer`): cleaner for cubes alone, but every copy, view and
  target would carry two indices, and cube arrays would need both.
- **Per-allocation fences in the upload path**: needed only if uploads outlive a frame slot, which nothing does.
- **A full blend description** (factors and operations per target): more general, more ways to differ between
  backends, and no user needs it.
