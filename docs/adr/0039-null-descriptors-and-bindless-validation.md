# 0039: Null descriptors per kind, and validated bindless handles in debug builds

Date: 2026-11-25

## Status

Accepted. Extends [ADR 0024](0024-resource-binding-model.md).

## Context

Shaders reach every resource through a `DescriptorHandle` (ADR 0024). A handle that names nothing, the wrong
kind of resource, or a freed one is undefined behavior: on WARP it removed the device with no message, and on
real GPUs it reads garbage or faults. Two gaps made that likely:

- **"No texture" had no valid handle.** Materials without a texture carried handle 0. On DX12 that is a
  view-heap slot nobody writes; on Metal it is a nil resource. A shader that loads the descriptor before testing
  the material's flags (compilers may hoist the load) reads it. The path tracer worked around it with its own
  1x1 texture.
- **Nothing checked handles.** The D3D12 debug layer does not look inside shaders; GPU-based validation does,
  but it crashes WARP on the path tracer, and Metal's shader validation does not know which handles are live.

## Decision

**Null descriptors.** At creation every device reserves one valid descriptor per kind: `texture` (a 1x1
`Texture2D` reading zero), `storage_texture`, `buffer` (a 16-byte zeroed buffer), `storage_buffer` and `sampler`.
(An acceleration structure is never optional where one is read, so it has none.) `Device::null_descriptor(kind)` returns it. Anything that means
"no resource" uses it; a material without a base color texture points at the null texture. On DX12 they sit in
fixed slots (view heap 1 to 4, sampler heap 1); on Metal they are resources owned by the device, made resident
on every encoder.

Handle 0 stays invalid. A single slot 0 cannot be valid for every kind on D3D12, whose shared heap holds typed
views (a null `Texture2D` view read as a `StructuredBuffer` is undefined), and an unset handle is a bug the
check below should report rather than mask.

**Validated handles in debug builds.** Every shader imports `veach.bindless`, which overrides Slang's
`getDescriptorFromHandle` (the function every `DescriptorHandle` dereference goes through). Built with
`VEACH_VALIDATE_BINDLESS` (debug builds), it looks the handle up in a live-descriptor table before using it; a
handle that is out of range, freed, or of the wrong kind is counted, with the first offender recorded, and
replaced by the kind's null descriptor so the shader goes on safely. Release builds compile the plain
dereference.

- **The table** (DX12) is kept by the RHI as resources are created and released (on release only after the GPU
  is done with them, so in-flight frames stay valid): one entry per heap slot, the kind and a generation.
  Handles carry their slot's entry in their upper 32 bits, so a stale handle to a reused slot is caught.
- **Binding (DX12).** The table is a read-only buffer at `t30` and the error record a read-write buffer at
  `u31`, both root descriptors (the root constants shrink from 64 to 60 DWORDs, so parameter blocks are at most
  240 bytes). They are bound in every build; only debug shaders read them.
- **Metal.** A Metal handle is an opaque resource ID or address that shaders cannot inspect, so debug builds turn
  on Metal's shader validation (`MTL_SHADER_VALIDATION=1`) instead: it reports nil, freed, non-resident and
  out-of-bounds accesses by failing the command buffer, which the RHI logs as `rhi.gpu_error`.
- **Reporting.** The DX12 record lives in CPU-readable memory; the RHI reads it as frames complete, and any count
  logs `rhi.invalid_descriptor` with the kind, the handle and the frame, so the run exits with code 1.

**Per-lane handles are marked `nonuniform`.** Slang assumes a `DescriptorHandle` is the same across a wave
unless it is wrapped in `nonuniform()`; handles read from per-hit records (an instance's vertex buffers, a
material's textures) are not, and without the mark DX12 may use one lane's descriptor for every lane. The
path tracer's helpers wrap every such access.

## Consequences

- An absent resource is a real, valid descriptor on both backends; shaders never read an unwritten slot.
- A wrong, freed or unset handle fails the debug run with a message naming it, instead of a device removal or
  silent garbage, on every backend including WARP.
- Debug shaders do a table lookup per descriptor load. Release shaders are unchanged.
- Parameter blocks shrink from 256 to 240 bytes.

## Alternatives considered

- **Make slot 0 valid for every kind:** impossible on D3D12 for typed views, and it hides unset handles.
- **Only the platform validators** (D3D12 GPU-based validation, Metal shader validation): GBV crashes WARP on
  the path tracer and neither knows our handles' kinds or lifetimes; they stay useful as extra tools.
- **Validate on the CPU when parameter blocks are set:** catches handles passed in parameters but not handles
  read from buffers (material and instance records), which is where most of them live.
