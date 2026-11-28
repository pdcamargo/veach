# GPU-driven RHI: indirect work and non-blocking readback

**Status:** Approved
**Area:** rhi
**Owner:** dev lead

## Summary

Draws and dispatches whose arguments a compute pass wrote, on Metal, DX12 and null, reachable from the render graph;
and a way for GPU results to reach CPU code a fixed number of frames later without the CPU ever waiting. The first
RHI step of the GPU-driven visibility-buffer renderer.

## Motivation

The renderer of [ADR 0060](../../adr/0060-gpu-driven-visibility-buffer.md) culls instances and meshlets in compute
and draws only what survives; its shading dispatches one bin per material kind. Both need GPU-written arguments
(VCH-071). Culling statistics, auto-exposure and other GPU-built values need to reach the CPU without stalling a
frame; today a readback either blocks in `Device::wait()` or each feature keeps its own ring of buffers (VCH-073).
The [RHI audit](../../reference/rhi-capabilities.md) lists both gaps.

## Requirements

- MUST dispatch with group counts read from a GPU buffer, inside a compute pass.
- MUST draw, non-indexed and indexed (uint32), with arguments read from a GPU buffer, inside a render pass, for
  several records `stride` bytes apart in one call.
- MUST use argument layouts that are D3D12's and Metal's own, so shaders write them as plain `uint`s through
  `DescriptorHandle`s.
- MUST make argument and index buffers render-graph accesses with their own states, with barriers derived and
  misuse rejected by `render.graph_invalid`.
- MUST check indirect calls the same way on every backend, null included.
- MUST let any code ask whether a submission has completed without waiting.
- MUST deliver a readback's bytes exactly `frames_in_flight` frames after the frame that recorded it, with no CPU
  wait in the normal frame loop, reusing staging buffers.
- MUST expose readback to render features through `FrameContext`, as bytes in a callback, never GPU handles.
- MUST fail readbacks of a frame that is not submitted with `render.readback_dropped`.

## API sketch

```cpp
// RHI
enum class BufferUsage { ..., indirect, index };
enum class ResourceState { ..., indirect_argument, index_read };
struct DispatchIndirectArgs { uint32_t group_count_x, group_count_y, group_count_z; };
struct DrawIndirectArgs { uint32_t vertex_count, instance_count, first_vertex, first_instance; };
struct DrawIndexedIndirectArgs { uint32_t index_count, instance_count, first_index; int32_t vertex_offset;
                                 uint32_t first_instance; };
cmd.dispatch_indirect(args, offset);
cmd.draw_indirect(args, offset, draw_count = 1, stride = sizeof(DrawIndirectArgs));
cmd.draw_indexed_indirect(indices, args, offset, draw_count = 1, stride = sizeof(DrawIndexedIndirectArgs));
bool Device::is_complete(SubmissionId);

// Render graph
b.read(args, rhi::ResourceState::indirect_argument);
b.read(indices, rhi::ResourceState::index_read);

// Readback
frame.readbacks->read(graph, buffer, size, [](Result<render::ReadbackData> data) { /* data->bytes, data->frame */ });
renderer.readbacks().latency();  // frames_in_flight
```

## Acceptance criteria

- **rhi.AC1** — `dispatch_indirect` launches exactly the group counts a compute pass wrote (Metal; DX12 on WARP).
- **rhi.AC2** — A compute pass culls 64 items and compacts the survivors; a two-record `draw_indirect` whose first
  record is empty and a `draw_indexed_indirect` each draw exactly the survivors, checked pixel for pixel by readback
  (Metal; DX12 on WARP).
- **rhi.AC3** — The render graph emits barriers into `indirect_argument` and `index_read` and rejects them on
  textures, in transfer passes, and `index_read` in compute passes; the null RHI checks and runs the same frame.
- **rhi.AC4** — `Device::is_complete` treats id 0 as complete and an unsubmitted id as not, and sees a GPU
  submission complete by polling alone.
- **rhi.AC5** — A readback arrives exactly `frames_in_flight` frames after its frame, not earlier, with no stall and
  the bytes the GPU wrote (Metal; DX12 on WARP; null with zeros).
- **rhi.AC6** — Readbacks of an unsubmitted frame fail with `render.readback_dropped`; `flush()` delivers in-flight
  readbacks at once; a readback every frame settles at `frames_in_flight` staging buffers.
- **rhi.AC7** — A render feature reads back through `FrameContext::readbacks` inside the renderer, and its results
  arrive `frames_in_flight` frames after the frame that recorded them.

## Out of scope

- GPU count buffers (D3D12-only; ADR 0089) and Metal indirect command buffers.
- Texture readback through the queue (screenshots keep their own path).
- An async compute queue (VCH-074) and wider atomics (VCH-075).
- Moving photo mode's diagnostics and SP-14's exposure readback onto the queue (when next touched).

## Open questions

None.

## Links

- ADRs: [0089](../../adr/0089-indirect-work-and-fixed-latency-readback.md), [0060](../../adr/0060-gpu-driven-visibility-buffer.md), [0029](../../adr/0029-render-graph-sync-and-barriers.md), [0030](../../adr/0030-frame-and-sync-model.md)
- Backlog: VCH-071, VCH-073
- Guides: [rendering](../../guides/rendering.md), [RHI capabilities](../../reference/rhi-capabilities.md)
