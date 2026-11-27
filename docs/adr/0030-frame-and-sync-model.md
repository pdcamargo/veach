# 0030: Frame and synchronization model

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0029).

## Context

Both backends need one model for fences, per-frame resources and resource lifetime, so the CPU and GPU overlap and
nothing stalls on destruction.

## Decision

**One graphics queue. Monotonic submission ids. N frames in flight (default 2, `frames_in_flight` in
`veach.project.json`, 1 to 4).** It lives in `rhi::SubmissionTracker`
(`engine/rhi/include/veach/rhi/submission_tracker.hpp`), shared by every backend.

1. **Submissions.** Each `Device::submit()` gets the next `SubmissionId` (1, 2, 3, ...). Work completes in order on
   the single queue, so completion is one watermark: `completed()` is the highest id known done.
   - Metal: the command buffer's completion handler calls `mark_completed(id)`; the state it touches is shared
     (`std::shared_ptr`), so a late handler never sees a destroyed device.
   - DX12: one `ID3D12Fence`, `Signal(fence, id)` after each `ExecuteCommandLists`; waiting is
     `SetEventOnCompletion`.
2. **Frames.** `Device::begin_frame()` starts frame N in slot `N % frames_in_flight` and blocks only until the last
   submission of frame N − frames_in_flight has completed. `end_frame()` records the last submission so far as
   that slot's fence.
3. **Per-frame resources.** Anything the CPU writes and the GPU reads later has one copy per slot, chosen with
   `current_frame().slot` (TLAS instance descriptors, inside the backend). Parameter blocks need none:
   `set_parameters` copies them. Swapchain images are paced by the swapchain.
4. **Retirement.** `destroy()` never waits. The handle is invalid at once; the backend object (and on DX12 its
   descriptor-heap slots) goes into `SubmissionTracker::retire()` and is released once every submission made before
   the `destroy()` has completed. `begin_frame()`, `wait()` and `wait_idle()` run due releases.
5. **Transients across frames.** Pooled transient textures remember the state their last frame left them in; their
   first barrier in a new frame uses it as `before`, with `Barrier::discard = true`.
6. **Explicit waits.** `wait(id)` blocks until a submission completes (screenshots, readback); `wait_idle()` blocks
   for everything (shutdown, device destruction).
7. **Readback without stalling** follows from 2 and 3: copy into a per-slot `MemoryType::readback` buffer and read
   it when that slot comes round again in `begin_frame()`, which already waited for it (the path tracer's
   diagnostics counters do this).

## Consequences

- CPU and GPU overlap by one frame by default; nothing stalls on destruction.
- Writing CPU data the GPU is still reading becomes possible. The rule: per-slot copies for anything CPU-written;
  the tracker tests (`[frames.AC*]`) pin the waiting and retirement semantics.
- Frame latency grows by up to one frame; `frames_in_flight: 1` restores lockstep for debugging.
- A second queue (async compute) would need cross-queue fences; not modeled yet.

## Alternatives considered

- **Per-frame fences only, no global submission ids:** harder to reason about several submissions per frame and
  about retirement.
- **Frame-count retirement** (free after N frames): wrong for resources used outside the frame cadence (one-off
  uploads, readbacks).
- **Reference-counted resources per command list:** precise, but heavier bookkeeping on every bind for little gain
  with one queue.
