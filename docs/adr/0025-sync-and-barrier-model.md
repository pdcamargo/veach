# 0025: Synchronization and barrier model

Date: 2026-11-24

## Status

Accepted

## Context

Metal tracks hazards for us when resources use `MTLHazardTrackingModeTracked` and work is split across encoders.
DX12 tracks nothing: every read-after-write, write-after-write and layout change needs an explicit barrier, and a
wrong one is silent corruption or a device removal. The RHI is shaped for both from day one, so the barrier model
must be explicit even though the first backend could ignore it.

## Decision

**Passes declare access, the render graph derives barriers.** Nothing outside the graph issues barriers.

Every graph pass declares each resource it touches with an `rhi::ResourceState`:

| State | Meaning | DX12 mapping (enhanced barriers: layout / access / sync) |
|---|---|---|
| `undefined` | contents discarded (first use of a transient) | `UNDEFINED` / `NO_ACCESS` / `NONE` |
| `shader_read` | sampled texture or read-only buffer, any stage | `SHADER_RESOURCE` / `SHADER_RESOURCE` / `ALL_SHADING` |
| `shader_write` | storage texture or RW buffer | `UNORDERED_ACCESS` / `UNORDERED_ACCESS` / `ALL_SHADING` |
| `color_target` | render-target attachment | `RENDER_TARGET` / `RENDER_TARGET` / `RENDER_TARGET` |
| `depth_write` | depth attachment | `DEPTH_STENCIL_WRITE` / `DEPTH_STENCIL_WRITE` / `DEPTH_STENCIL` |
| `copy_src` / `copy_dst` | copy source / destination | `COPY_SOURCE` / `COPY_DEST` + `COPY` |
| `accel_build` | acceleration structure being built or refit | access `RAYTRACING_ACCELERATION_STRUCTURE_WRITE`, sync `BUILD_RAYTRACING_ACCELERATION_STRUCTURE` |
| `accel_read` | acceleration structure traced against | access `RAYTRACING_ACCELERATION_STRUCTURE_READ`, sync `ALL_SHADING` |
| `present` | handed to the swapchain | `PRESENT` / `COMMON` |

From the declarations, the graph:

1. **Orders passes.** Dependencies come from resource accesses: read-after-write, write-after-read and
   write-after-write. Passes are ordered topologically, with declaration order as the tie-breaker, so the result
   is deterministic.
2. **Culls** passes whose writes nobody reads, unless they write an imported resource (an output).
3. **Emits barriers** before each pass. For every resource whose state changes, it emits
   `Barrier { resource, before, after }`. For write-after-write in `shader_write` it emits a UAV-style barrier
   (`before == after == shader_write`).
4. **Declares residency** with `CommandList::use()` for every access (Metal needs it; see
   [ADR 0024](0024-resource-binding-model.md)).

`RenderGraph::describe()` prints the compiled order, the culled passes and every barrier as text. Agents (and the
`--log-level debug` log) can check what the graph decided without a GPU debugger.

**Backends:**

- **Metal:** each graph pass records into its own encoder (render, compute, blit or acceleration-structure), and
  every resource uses tracked hazard mode, so Metal inserts the actual waits. `CommandList::barrier()` validates
  in debug builds that the transitions are consistent (`before` matches the state the backend last recorded) and
  otherwise does nothing. When we move to heaps and untracked resources for speed, the same barrier list drives
  `MTLFence` updates and waits at pass boundaries.
- **DX12:** `barrier()` becomes `ID3D12GraphicsCommandList7::Barrier` with the mapping table above (enhanced
  barriers, Agility SDK). Texture layouts follow the states; buffers only use access and sync.

**Queues and frames.** One graphics queue for now. `Device::submit()` returns a `SubmissionId`, and `wait()` blocks
on it (Metal `waitUntilCompleted`; DX12 a fence value). Screenshots and readbacks wait on their submission. The
renderer keeps at most two frames in flight. Destroying a resource waits for the GPU to go idle (v0); a fenced
deferred-free queue replaces that once resources churn (VCH-042).

## Consequences

- Barrier bugs show up on Metal in debug builds (state validation) before DX12 exists, and the graph's barrier
  list is unit-tested with the null device. No GPU is needed.
- Metal gets no benefit from the barrier list today. We accept that cost for a DX12 port that implements a table,
  not a design.
- Things DX12 will resist:
  - **Swapchain buffers** start in `PRESENT`/`COMMON` and must return there before `Present`. The graph imports
    the swapchain image as `present` and marks it as an output that ends in `present`.
  - **Initial states.** Created resources start `undefined`. On DX12 that is `COMMON`/`UNDEFINED` layout, which is
    legal as long as the first transition uses `before = undefined`. Imported resources must state their current
    state truthfully.
  - **AS buffers** never change layout; `accel_build` → `accel_read` is an access/sync-only barrier.
  - **Split barriers, async compute and aliasing of transients** are not modeled yet. They will be added as new
    graph features without changing pass declarations.

## Alternatives considered

- **Hand-written barriers in each pass**: correct on Metal by accident and wrong on DX12 by default.
- **Legacy DX12 resource-state barriers**: simpler, but coarser and deprecated in direction. Enhanced barriers map
  1:1 onto the states above and onto Vulkan-style sync if that ever matters.
- **No graph-level barriers on Metal at all** (rely on tracking only): fastest to write, but leaves the DX12 port
  to rediscover every hazard.
