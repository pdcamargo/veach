# RHI capabilities: what the GPU layer can do for the toolbox

An audit of `engine/rhi` ([ADR 0004](../adr/0004-own-rhi-not-sdl-gpu.md)) against the GPU compute the space toolbox
will need: terrain meshed on the GPU and handed to CPU physics, procgen in compute, GPU-driven drawing of many
objects. Taken at the reset (2026-11-27) from the code (`engine/rhi/include/veach/rhi/`, the three backends) and from
what tests actually exercise. It records what exists; it decides nothing. The gaps are research-phase inputs in the
[backlog](../backlog/BACKLOG.md) (VCH-071 to VCH-076). Updated as gaps close: indirect work and readback (VCH-071,
VCH-073) on 2026-11-28; texture kinds, texture updates and raster state (VCH-072, VCH-076, VCH-098) the same day
([spec](../specs/rhi-textures/spec.md), [ADR 0090](../adr/0090-rhi-texture-kinds-updates-and-raster-state.md)).

Backends: **Metal** (macOS, real GPU in the pre-push gate), **DX12** (Windows; verified only on WARP in hosted CI,
[ADR 0033](../adr/0033-dx12-on-warp-in-ci.md)), **null** (no GPU: every call succeeds and does nothing, except that
upload and readback buffers get host memory so `map()` works).

Legend: **yes** = implemented and exercised by a test or a golden; **partial** = works with a limit noted;
**no** = not in the RHI.

| Capability | Metal | DX12 | null | Notes | Gap |
|---|---|---|---|---|---|
| Compute pipelines | yes | yes | no-op | `Device::create_compute_pipeline`; `threads_per_group` must match `[numthreads]`. Photo mode, the shade pass, the sky LUTs and the display pass are compute. | |
| Dispatch | yes | yes | no-op | `CommandList::dispatch(x, y, z)` inside a compute pass; 3D grids work. | |
| Indirect dispatch | yes | yes (WARP) | no-op (checked) | `dispatch_indirect(args, offset)` with `DispatchIndirectArgs` a compute pass wrote; the graph declares `indirect_argument` ([spec](../specs/gpu-driven-rhi/spec.md), rhi.AC1). | |
| Indirect draw | yes | yes (WARP) | no-op (checked) | `draw_indirect` and `draw_indexed_indirect` (uint32 indices), several records per call with a stride: one `ExecuteIndirect` on DX12, one call per record on Metal. No count buffers (Metal has none; [ADR 0089](../adr/0089-indirect-work-and-fixed-latency-readback.md)). Keep `first_instance` and `vertex_offset` 0. rhi.AC2. | |
| Multisampling (MSAA) | partial | no (rejected) | accepted | Spike SP-16: `sample_count` 2/4/8 on memoryless attachments, resolved on tile (`PassBuilder::resolve`), `alpha_to_coverage`; Metal only, unused by the real-time renderer ([SP-16](../research/spikes/sp-16-visibility-vs-forward.md)). | |
| Read-write storage buffers | yes | yes | no-op | `BufferUsage::storage`, `descriptor(buffer, View::storage)` (a UAV on DX12). Any size; structured or byte-address. | |
| Texture kinds | yes | yes (WARP) | checked | `TextureType` `d2`, `d2_array`, `d3`, `cube`, `cube_array` (`depth`, `layers`), sampled as Texture2D/2DArray/3D/Cube/CubeArray through `DescriptorHandle`s; initial data per mip in KTX2 order. Null descriptors for every kind. rhi.AC8 to AC11. | |
| Views of parts of a texture | yes | yes (WARP) | checked | `descriptor(texture, view, TextureRange)`: a run of mips and layers (a texture view on Metal, an SRV/UAV on DX12, made once per range). A render pass draws into one mip and layer (`TargetSlice`). Barriers name a run of mips (`Barrier::first_mip`, `mip_count`; a subresource range on DX12): the render graph keeps a state per mip (`RenderGraph::mips`), so a pass reads one mip while it writes the next. gpudriven.AC1. | |
| Read-write storage textures | yes | yes (WARP) | no-op | `TextureUsage::storage` on 2D, 3D (RWTexture3D) and arrays and cubes (RWTexture2DArray), one mip per view. Not BC7, sRGB or depth. | |
| Texture formats | partial | partial | n/a | `rgba8` (unorm, sRGB), `bgra8`, `rgba16_float`, `rgba32_float`, `r8_unorm`, `rg8_unorm`, `r16_float`, `rg16_float`, `r32_float`, `r32_uint`, `rg32_uint`, `depth32_float`, BC7. No signed integer formats, no other BC formats. | |
| 32-bit buffer atomics | yes | yes | no-op | `InterlockedAdd` / `InterlockedCompareExchange` on `RWStructuredBuffer<uint>`: photo mode's diagnostics, the debug descriptor checks. | |
| Texture atomics, 64-bit atomics | no | no | no | Never written or tested on either backend. | VCH-075 |
| Wave intrinsics | partial | partial | n/a | Used on both backends before the reset (and passed WARP); no current user. Unchecked across Apple GPU generations. | |
| Upload to GPU buffers | yes | yes | yes | `MemoryType::upload` (persistently mapped) and initial data at creation; `copy_buffer` to device-local buffers, `copy_buffer_regions` for many ranges in one blit (the GPU scene's changed instance records). | |
| Upload to textures after creation | yes | yes (WARP) | checked | `CommandList::copy_buffer_to_texture` into any box of one mip over a run of layers; `render::upload_texture` stages tightly packed data in `render::UploadRing` (`FrameContext::uploads`, recycled per frame slot) and adds a transfer pass. rhi.AC12, AC13. | |
| Blend state | yes | yes (WARP) | checked | `GraphicsPipelineDesc::blend` per color target: opaque, alpha, premultiplied, additive. rhi.AC16, golden `blend_modes`. | |
| Depth bias | yes | yes (WARP) | checked | `GraphicsPipelineDesc::depth_bias`: constant (whole steps), slope, clamp. rhi.AC17. | |
| Comparison samplers | yes | yes (WARP) | checked | `SamplerDesc::compare` (SamplerComparisonState, `SampleCmp`); linear filtering is 2x2 PCF. rhi.AC14. | |
| Anisotropic filtering | yes | yes (WARP) | checked | `SamplerDesc::max_anisotropy` 1 to 16; terrain uses `rhi::terrain_anisotropy` (8). Samplers filter between mips on both backends. rhi.AC15. | |
| Readback to the CPU | yes | yes (WARP) | yes (zeros) | `render::ReadbackQueue` (`FrameContext::readbacks`): a buffer's bytes reach a callback exactly `frames_in_flight` frames later, never waiting; staging buffers are pooled. Underneath: `MemoryType::readback` buffers, `copy_buffer`, `copy_texture_to_buffer`. rhi.AC5 to AC7. | |
| Non-blocking completion query | yes | yes (WARP) | yes | `Device::is_complete(SubmissionId)`: the command buffer's status on Metal, a fence read on DX12. rhi.AC4. | |
| Async compute queue | no | no | no | One graphics queue ([ADR 0030](../adr/0030-frame-and-sync-model.md)); the render graph has no cross-queue synchronization. | VCH-074 |
| Several submissions per frame | yes | yes | yes | Each `submit()` gets a `SubmissionId`; screenshots and readbacks use their own. | |
| Per-pass GPU timing | yes | yes | reports why not | Timestamps per render-graph pass, MCP `gpu_timings`. | |
| GPU memory accounting | yes | yes | yes | `Device::memory_usage()`: per-resource sizes as the driver allocated them. | |
| Hardware ray tracing | yes | yes (WARP) | no | Optional ([ADR 0003](../adr/0003-platforms-and-hardware-target.md)); BLAS/TLAS build and refit, inline ray queries. Photo mode only. | |
| Mesh shaders | no | no | no | `Capabilities::mesh_shaders` is reported but there is no API; meshlets are drawn by vertex pulling. | |

## What the toolbox's first likely uses need

- **GPU terrain meshing to CPU physics:** compute (yes), storage buffers (yes), atomics for vertex/index allocation
  (32-bit yes), indirect draw of the result (yes), and readback that does not stall a frame (yes, a fixed
  `frames_in_flight` frames late). Colliders stay CPU-built ([ADR 0053](../adr/0053-terrain-density-field.md)): GPU
  results are visual only ([ADR 0043](../adr/0043-determinism-tiers-and-strict-floating-point.md)).
- **Density fields, clouds, volumetric atmosphere:** 3D textures written in compute and sampled (yes).
- **Procgen and simulation in the background of a frame:** async compute (no, VCH-074); without it, compute runs in
  the frame's graph on the one queue.
- **Streaming chunks and textures:** texture updates after creation (yes, through the upload ring and the graph).
- **Shadows, transparency, terrain materials:** depth bias, comparison samplers, blend state, texture arrays and
  anisotropic filtering (yes).
