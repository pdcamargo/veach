# 0029: Frame and synchronization model

Date: 2026-11-25

## Status

Accepted. Supersedes the "Queues and frames" part of [ADR 0025](0025-sync-and-barrier-model.md).

## Context

v0 waited for the previous frame before recording the next one, and `destroy()` waited for the GPU to go idle.
Correct, but the CPU and GPU never overlapped, and destruction stalled. The DX12 port starts next week; it needs a
final model for fences, per-frame resources and resource lifetime, so both backends implement the same rules.

## Decision

**One graphics queue. Monotonic submission ids. N frames in flight (default 2, `frames_in_flight` in
`veach.project.json`, 1 to 4).** All of it lives in `rhi::SubmissionTracker`
(`engine/rhi/include/veach/rhi/submission_tracker.hpp`), shared by every backend.

1. **Submissions.** Each `Device::submit()` gets the next `SubmissionId` (1, 2, 3, ...). Work completes in order on
   the single queue, so completion is a single watermark: `completed()` is the highest id known done.
   - Metal: the command buffer's completion handler calls `mark_completed(id)`. The state it touches is shared
     (`std::shared_ptr`) so a late handler never sees a destroyed device.
   - DX12: one `ID3D12Fence`; `Signal(fence, id)` after each `ExecuteCommandLists`. `completed()` is
     `fence->GetCompletedValue()`, and waiting is `SetEventOnCompletion(id, event)`.
2. **Frames.** `Device::begin_frame()` starts frame N in slot `N % frames_in_flight`. It blocks only until the last
   submission of frame N − frames_in_flight (the previous user of that slot) has completed.
   `Device::end_frame()` records the last submission so far as that slot's fence. With 2 frames in flight, the CPU
   records frame N while the GPU renders frame N − 1.
3. **Per-frame resources.** Anything the CPU writes and the GPU reads later must have one copy per slot, chosen
   with `current_frame().slot`. Today that is TLAS instance descriptors (handled inside the backend). Parameter
   blocks need none: `set_parameters` copies them (Metal `setBytes`; DX12 root constants). Swapchain images are
   paced by the swapchain itself.
4. **Retirement (replaces VCH-042).** `destroy()` never waits. The handle is invalid at once. The backend object,
   and on DX12 its descriptor-heap slots, goes into `SubmissionTracker::retire()` and is released once every
   submission made before the `destroy()` has completed. `begin_frame()`, `wait()` and `wait_idle()` run the due
   releases.
5. **Transients across frames.** Pooled transient textures remember the state their last frame left them in.
   Their first barrier in a new frame uses that real state as `before`, with `Barrier::discard = true`. This
   synchronizes with the earlier frame's use (which may still be in flight) while telling DX12 the contents can be
   dropped.
6. **Explicit waits.** `wait(id)` blocks until a submission completes (screenshots, readback), and `wait_idle()`
   blocks for everything (shutdown, device destruction).

## Consequences

- CPU and GPU overlap by one frame by default, and nothing stalls on destruction.
- The DX12 port implements one fence plus the tracker, not a design. Descriptor slots get correct lifetimes for
  free by freeing them in the retire callback.
- With frames in flight, a bug that writes CPU data the GPU is still reading becomes possible. The rule is
  simple: per-slot copies for anything CPU-written, and the tracker tests (`[frames.AC*]`) pin down the
  waiting and retirement semantics.
- Frame latency grows by up to one frame. `frames_in_flight: 1` restores the old behaviour, for debugging.

## Alternatives considered

- **Per-frame fences only, with no global submission ids**: harder to reason about multiple submissions per frame
  (screenshots, uploads) and about retirement.
- **Frame-count-based retirement** (free after N frames): simpler, but wrong for resources used by a submission
  outside the frame cadence (one-off uploads, readbacks).
- **Reference-counted resources tracked per command list**: precise, but heavier bookkeeping on every bind for
  little gain with one queue.
