# 0089: Indirect work without count buffers, and readback at a fixed latency of frames in flight

Date: 2026-11-28

## Status

Accepted.

## Context

The GPU-driven renderer ([0060](0060-gpu-driven-visibility-buffer.md)) culls in compute and draws what survives, and
its shading is dispatched per material bin: both need draw and dispatch arguments written by the GPU (VCH-071).
GPU-built data (culling statistics, exposure, later terrain meshes for display) also has to reach the CPU without
stalling a frame (VCH-073). Until now the RHI had only CPU-sized `draw` and `dispatch`, and a readback was either a
blocking `Device::wait()` or a per-feature ring of buffers read when the frame slot came round
([audit](../reference/rhi-capabilities.md)).

The two backends differ where it matters:

- D3D12's `ExecuteIndirect` takes a command signature, a maximum command count and an optional GPU count buffer.
- Metal has indirect draws and dispatches, one record per call, and no count buffer. Its indirect command buffers
  (ICBs) batch draws, but the GPU can only fill them from Metal Shading Language's `command_buffer` types, which Slang
  does not emit, and CPU-encoded ICB commands take no indirect arguments.
- Metal's `[[instance_id]]` and `[[vertex_id]]` include the base instance and base vertex; D3D12's `SV_InstanceID`
  does not include `StartInstanceLocation`.

## Decision

**Indirect work.** `CommandList::dispatch_indirect(args, offset)`, `draw_indirect(args, offset, draw_count, stride)`
and `draw_indexed_indirect(indices, args, offset, draw_count, stride)`. The records are `rhi::DispatchIndirectArgs`,
`DrawIndirectArgs` and `DrawIndexedIndirectArgs`, which have the same layout as both APIs' own structures
(`static_assert`ed in both backends), so a Slang shader writes them as plain `uint`s into a `RWStructuredBuffer`
through a `DescriptorHandle`. Argument buffers are created with `BufferUsage::indirect`, index buffers (uint32 only)
with `BufferUsage::index`.

- **Multi-draw takes a CPU count and a stride; there are no count buffers.** D3D12 runs one `ExecuteIndirect` with
  a command signature per argument type and stride, cached by the device; Metal encodes one indirect draw per
  record. A GPU-decided amount of work is expressed as a compacted list drawn with `instance_count` set by the GPU
  (the visibility buffer's meshlet list is exactly this), or as records the GPU zeroes. A count buffer would be
  D3D12-only.
- **`first_instance` and `vertex_offset` stay 0 in portable code.** Shaders that need offsets read them from their
  own data.
- **Two new resource states,** `indirect_argument` (D3D12: `INDIRECT_ARGUMENT` access, `EXECUTE_INDIRECT` sync) and
  `index_read` (`INDEX_BUFFER`, `INDEX_INPUT`). The render graph derives their barriers like any other access and
  rejects them on textures, in transfer passes, and `index_read` outside graphics passes. On Metal the buffers are
  bound to the draw or dispatch call itself, which tracks the hazard against the pass that wrote them.
- **Validation is shared.** Every backend, null included, asserts the usage flags, 4-byte alignment and that the
  records fit, so a call that passes on null passes on the GPUs.

**Readback.** `Device::is_complete(SubmissionId)` reports completion without waiting (a fence read on D3D12, the
command buffer's status on Metal). On top of it, `render::ReadbackQueue`, owned by the renderer and reached by
features through `FrameContext::readbacks`, copies a buffer into a staging buffer at the end of the frame's graph (a
declared transfer pass) and hands the bytes to a callback **exactly `frames_in_flight` frames later**, right after
`begin_frame`. By then `begin_frame` has already waited for that frame, so delivery never stalls, and the arrival
frame is the same on every backend and GPU. Staging buffers come from a pool (powers of two from 256 bytes), so a
readback every frame settles at `frames_in_flight` buffers. A frame that is not submitted fails its readbacks with
`render.readback_dropped`; `flush()` waits and delivers at once (shutdown, tests).

Callbacks run on the render thread inside `Renderer::render()` and copy bytes into the module's own CPU state, never
into the World. Readback data is GPU output, so it stays visual or diagnostic and never feeds canonical simulation
([0043](0043-determinism-tiers-and-strict-floating-point.md)); physics colliders keep coming from the CPU mesher
([0053](0053-terrain-density-field.md)).

## Consequences

- The visibility-buffer renderer (0060) can cull and draw without CPU round trips, and SP-16 (VCH-092) can start.
- A feature that wants a GPU number on the CPU no longer builds its own ring of readback buffers. Photo mode's
  diagnostics and SP-14's exposure readback still have their own; they can move to the queue when next touched.
- Results are always exactly `frames_in_flight` frames old, even when the GPU finished sooner. A caller that needs
  them sooner waits explicitly (`flush`).
- Metal multi-draws cost one encoded call per record. That is cheap for the tens of records a binned shade or a few
  draw groups need; thousands of separately drawn records would need ICBs, and so Metal-specific shader code.

## Alternatives considered

- **Count buffers on D3D12, an emulation on Metal** (a compute pass zeroing records past the count). It adds an
  engine-internal shader and a pass to every Metal multi-draw for a feature the compacted-list pattern does not need.
- **Metal indirect command buffers.** GPU encoding needs MSL-only types Slang cannot produce; CPU encoding loses the
  indirect arguments.
- **Readback delivered as soon as `is_complete` says so.** Fresher by up to a frame on a fast GPU, but the arrival
  frame would depend on GPU timing: two runs of the same recording would see results on different frames, and tests
  could not pin it.
- **Readback results as components.** That would put the renderer upstream of simulation in the ECS; modules keep
  the bytes in their own state instead.
