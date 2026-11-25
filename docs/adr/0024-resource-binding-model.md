# 0024: Resource binding model (bindless)

Date: 2026-11-24

## Status

Accepted

## Context

The RHI is modern-only and bindless ([ADR 0009](0009-rhi-design.md)). It has to map cleanly onto Metal now and
DX12 next week, from shaders written once in Slang ([ADR 0003](0003-shaders-slang.md)). Tessera and Candela will
touch thousands of resources per frame, so per-draw binding tables, descriptor sets and slot juggling are out.

We compiled the same Slang shader for both targets (Slang 2026.22) to see what it emits:

```slang
struct Params {
    DescriptorHandle<RWTexture2D<float4>> output;
    DescriptorHandle<RaytracingAccelerationStructure> scene;
    DescriptorHandle<StructuredBuffer<Vertex>> vertices;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> params;
```

- **Metal:** `Params` becomes a `constant Params*` at `[[buffer(0)]]`. A texture handle is a `texture2d<...>` field,
  an acceleration structure is an `acceleration_structure<instancing>` field, and a structured-buffer handle is a
  `device T*` field. With Metal 3 argument buffers (tier 2) each of these is 8 bytes: a `gpuResourceID` or a
  `gpuAddress`.
- **HLSL/DXIL (SM 6.6):** each handle is a `uint2`, and the shader reads `ResourceDescriptorHeap[h.x]`. `Params`
  is a `cbuffer` at `b0`.

## Decision

**Shaders reach every resource through `DescriptorHandle<T>` values in a small per-draw or per-dispatch parameter
block.** There are no binding slots, descriptor sets or per-pass layouts.

**CPU side:** `rhi::DescriptorHandle { std::uint64_t value; }`, 8 bytes on every backend. The device returns one
for each view a shader can use:

| Resource view | `Device::` call | Metal value | DX12 value |
|---|---|---|---|
| Buffer, read (`StructuredBuffer`, `ByteAddressBuffer`) | `buffer_descriptor(b)` | `MTLBuffer.gpuAddress` | SRV index in the CBV/SRV/UAV heap |
| Buffer, read-write (`RW...`) | `buffer_descriptor(b, View::storage)` | `gpuAddress` | UAV index |
| Texture, sampled | `texture_descriptor(t)` | `MTLTexture.gpuResourceID` | SRV index |
| Texture, storage | `texture_descriptor(t, View::storage)` | `gpuResourceID` | UAV index |
| Sampler | `sampler_descriptor(s)` | `MTLSamplerState.gpuResourceID` | index in the sampler heap |
| Acceleration structure | `acceleration_structure_descriptor(a)` | `gpuResourceID` | SRV index (raytracing AS SRV) |

On DX12 the low 32 bits are the heap index and the high 32 bits are zero, matching Slang's `uint2` lowering.

**Parameter blocks** are C++ structs that mirror the Slang struct and are set with `CommandList::set_parameters`
(at most 256 bytes). Metal uses `setBytes` at buffer index 0 for every stage. DX12 will use root constants at
`b0`, or a root CBV if a block outgrows them. To keep one layout valid on both targets:

- handles are `rhi::DescriptorHandle` (8 bytes, 8-aligned);
- scalars are 4 bytes; `uint2`/`float2` are 8-aligned;
- `float4` and `float4x4` are 16-aligned;
- **no `float3`/`uint3` members**, because Metal's `float3` is 16 bytes and HLSL's is 12.

Every parameter struct has `static_assert`s on size and offsets. Validating them against Slang's reflection JSON
is VCH-041.

**Residency.** Metal only keeps resources resident when the encoder is told about them. The render graph already
knows every resource a pass reads or writes (it needs that for barriers, [ADR 0025](0025-sync-and-barrier-model.md)),
so it calls `CommandList::use(resource, access)` for each one. The Metal backend turns that into
`useResource:usage:(stages:)`. A TLAS also marks every BLAS it references as resident, as Metal requires.
Resources a shader reaches without declaring them are a bug, and Metal validation catches them in debug builds.

## Consequences

- One shader source and one CPU struct per pass on both backends. Adding resources never changes a layout.
- Requires Metal 3 with tier-2 argument buffers (Apple7+/Mac2 family; M1 and later) and, on DX12, Shader Model 6.6
  (`ResourceDescriptorHeap`). Both are below our hardware floor ([ADR 0002](0002-target-platforms-and-hardware.md)).
  The GitHub-hosted macOS runner's paravirtual GPU has neither (tier-1 argument buffers, no Metal 3), so GPU tests
  need a real Apple GPU (see the first-light spec).
- Things DX12 will resist, for whoever implements it (VCH-012):
  - **Separate read and read-write views.** DX12 needs an SRV and a UAV descriptor where Metal uses one resource ID,
    so the API asks for the view explicitly. Create both views when a resource has both usages.
  - **Descriptor lifetime.** A heap slot can be reused only after the GPU finishes with it, so frees go through a
    fence-tagged deferred-free list. Metal has no equivalent problem.
  - **Root signature.** It must be global and use `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`.
    Root constants cap the parameter block at 64 DWORDs (256 bytes), which is why `set_parameters` has the same limit.
  - **Static samplers are not used.** Samplers live in the sampler heap (2048 entries), which is fine for the
    expected sampler count.
  - **Acceleration-structure buffers** must be created in the `RAYTRACING_ACCELERATION_STRUCTURE` state and can never
    transition out of it.

## Alternatives considered

- **Classic slots and descriptor sets/tables** (WebGPU, Vulkan 1.0 style): per-pass layouts, rebinding cost, and
  no path to GPU-driven rendering.
- **A global "bindless table" argument buffer on Metal** (arrays of textures indexed by `uint`): an extra indirection
  and an argument-buffer layout to keep in sync. Metal 3 resource IDs make the handle *be* the resource.
- **Raw buffer pointers everywhere** (`device T*`, SPIR-V physical pointers): DXIL has no equivalent.
