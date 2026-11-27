# 0027: Resource binding model: bindless handles, null descriptors, validated in debug

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADRs 0024 and 0039).

## Context

The RHI is modern-only and bindless ([ADR 0004](0004-own-rhi-not-sdl-gpu.md)). It must map onto Metal and DX12
from shaders written once in Slang ([ADR 0008](0008-shaders-slang.md)). GPU-driven rendering and procedural
generation touch many resources per frame, so per-draw binding tables, descriptor sets and slot juggling are out.

What Slang (2026.22) emits for a `DescriptorHandle<T>` field in a `[[vk::push_constant]] ConstantBuffer<Params>`:

- **Metal:** `Params` becomes a `constant Params*` at `[[buffer(0)]]`. A texture handle is a `texture2d<...>`
  field, a structured buffer a `device T*`, an acceleration structure an `acceleration_structure<instancing>`; with
  Metal 3 argument buffers (tier 2) each is 8 bytes, a `gpuResourceID` or `gpuAddress`.
- **HLSL/DXIL (SM 6.6):** each handle is a `uint2`, and the shader reads `ResourceDescriptorHeap[h.x]`.

## Decision

**Shaders reach every resource through `DescriptorHandle<T>` values in a small per-draw or per-dispatch parameter
block.** No binding slots, descriptor sets or per-pass layouts.

**CPU side:** `rhi::DescriptorHandle { std::uint64_t value; }`, 8 bytes on every backend, from `Device::descriptor()`:

| View | Call | Metal value | DX12 value |
|---|---|---|---|
| Buffer, read | `descriptor(buffer)` | `gpuAddress` | SRV index in the CBV/SRV/UAV heap |
| Buffer, read-write | `descriptor(buffer, View::storage)` | `gpuAddress` | UAV index |
| Texture, sampled | `descriptor(texture)` | `gpuResourceID` | SRV index |
| Texture, storage | `descriptor(texture, View::storage)` | `gpuResourceID` | UAV index |
| Sampler | `descriptor(sampler)` | `gpuResourceID` | sampler-heap index |
| Acceleration structure | `descriptor(accel)` | `gpuResourceID` | SRV index |

On DX12 the low 32 bits are the heap index; the high 32 bits carry the slot's validation entry (below).
Acceleration-structure descriptors exist only where the device has hardware ray tracing
([ADR 0003](0003-platforms-and-hardware-target.md)).

**Parameter blocks** are C++ structs mirroring the Slang struct, set with `CommandList::set_parameters`, **at most
240 bytes** (`rhi::max_parameter_bytes`: 60 root-constant DWORDs on DX12; Metal uses `setBytes` at buffer index 0).
Layout rules, so one layout is valid on both targets:

- handles are `rhi::DescriptorHandle` (8 bytes, 8-aligned); scalars are 4 bytes; `uint2`/`float2` are 8-aligned;
- `float4` and `float4x4` are 16-aligned;
- **no `float3`/`uint3` members** (16 bytes in Metal, 12 in HLSL).

Every parameter struct is checked against Slang's reflection with `rhi::parameters_match` and `VEACH_PARAM`
`static_assert`s; structured-buffer element structs assert their offsets by hand.

**Null descriptors.** Every device reserves one valid descriptor per kind at creation: `texture` (1x1 reading zero),
`storage_texture`, `buffer` (16 zeroed bytes), `storage_buffer` and `sampler`; `Device::null_descriptor(kind)`
returns it. Anything that means "no resource" uses it (a material without a base color texture points at the null
texture). Acceleration structures have none: where one is read it is never optional. Handle 0 stays invalid, so an
unset handle is a bug the validation below reports.

**Validated handles in debug builds.** Every shader imports `veach.bindless`, which overrides Slang's
`getDescriptorFromHandle`. Built with `VEACH_VALIDATE_BINDLESS` (debug builds), it looks the handle up in a
live-descriptor table first; a handle that is out of range, freed, or of the wrong kind is counted (the first
offender recorded) and replaced by the kind's null descriptor. Release builds compile the plain dereference.

- **DX12:** the RHI keeps the table (kind and generation per heap slot, entries released only after the GPU is
  done). The table is a root SRV at `t30`, the error record a root UAV at `u31`. Any count is logged as
  `rhi.invalid_descriptor` with kind, handle and frame, so the run exits 1.
- **Metal:** handles are opaque to shaders, so debug builds use Metal's shader validation
  (`MTL_SHADER_VALIDATION=1`), which fails the command buffer on nil, freed, non-resident and out-of-bounds
  accesses; the RHI logs it as `rhi.gpu_error`.

**Per-lane handles are `nonuniform()`.** Handles read from per-instance or per-material records differ across a
wave; without the mark DX12 may use one lane's descriptor for all.

**Residency** on Metal: [ADR 0028](0028-metal-residency-set.md). The render graph declares every access, which also
drives barriers ([ADR 0029](0029-render-graph-sync-and-barriers.md)).

## Consequences

- One shader source and one CPU struct per pass on both backends; adding resources never changes a layout.
- Requires Metal 3 with tier-2 argument buffers (every Apple silicon Mac) and, on DX12, SM 6.6 with resource binding
  tier 3: the hardware floor of ADR 0003. GitHub's hosted macOS runner has neither, so Metal GPU tests run in the
  pre-push gate.
- An absent resource is a real descriptor on both backends; a wrong, freed or unset handle fails the debug run with
  a message instead of a device removal or silent garbage, WARP included.
- What DX12 resists, for anyone touching the backend:
  - separate SRV and UAV descriptors where Metal has one resource ID, hence the explicit `View`;
  - heap slots are reused only after the GPU finishes with them (retired through the submission tracker,
    [ADR 0030](0030-frame-and-sync-model.md));
  - one global root signature with `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`, no static
    samplers (2048-entry sampler heap);
  - acceleration-structure buffers live in the `RAYTRACING_ACCELERATION_STRUCTURE` state forever.

## Alternatives considered

- **Slots and descriptor sets/tables** (WebGPU, Vulkan 1.0 style): per-pass layouts, rebinding cost, no path to
  GPU-driven rendering.
- **A global bindless argument-buffer table on Metal:** an extra indirection and a layout to keep in sync; Metal 3
  resource IDs make the handle be the resource.
- **Making slot 0 valid for every kind:** impossible on D3D12 for typed views, and it hides unset handles.
- **Only the platform validators** (D3D12 GPU-based validation, Metal shader validation): GBV crashes WARP on our
  ray-query shaders, and neither knows our handles' kinds or lifetimes.
- **CPU-side validation of parameter blocks:** misses handles read from buffers, where most of them live.
