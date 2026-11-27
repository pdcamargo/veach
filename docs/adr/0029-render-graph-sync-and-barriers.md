# 0029: Render graph synchronization and barrier model

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0025, without its frames paragraph, which is
[ADR 0030](0030-frame-and-sync-model.md)).

## Context

Metal tracks hazards for tracked resources split across encoders. DX12 tracks nothing: every read-after-write,
write-after-write and layout change needs an explicit barrier, and a wrong one is silent corruption or a device
removal. The barrier model must be explicit even where one backend could ignore it.

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

From the declarations the graph:

1. **Orders passes** topologically from read-after-write, write-after-read and write-after-write dependencies, with
   declaration order as the tie-breaker, so the result is deterministic.
2. **Culls** passes whose writes nobody reads, unless they write an imported resource (an output).
3. **Emits barriers** before each pass: `Barrier { resource, before, after }` for every state change, and a
   UAV-style barrier (`before == after == shader_write`) for write-after-write.
4. **Declares residency** with `CommandList::use()` for every access ([ADR 0028](0028-metal-residency-set.md)).

`RenderGraph::describe()` prints the compiled order, the culled passes and every barrier as text; MCP
`render_graph_plan` and `--log-level debug` show it without a GPU debugger.

**Backends:**

- **Metal:** each pass records into its own encoder (render, compute, blit or acceleration-structure), with tracked
  hazard mode, so Metal inserts the waits. `CommandList::barrier()` validates in debug builds that `before` matches
  the state last recorded, and otherwise does nothing.
- **DX12:** `barrier()` is `ID3D12GraphicsCommandList7::Barrier` with the table above. Texture layouts follow the
  states; buffers use access and sync only.

## Consequences

- Barrier bugs show up on Metal in debug builds (state validation) and are unit-tested on the null device.
- Things DX12 resists:
  - **Swapchain images** start and must end in `PRESENT`/`COMMON`: the graph imports them as `present` outputs.
  - **Initial states.** Created resources start `undefined`; imported ones must state their current state truthfully.
  - **AS buffers** never change layout; `accel_build` → `accel_read` is access/sync only.
- **Not modeled yet:** split barriers, async compute queues and aliasing of transients. They are graph features to
  add without changing pass declarations (async compute is a research-phase input, [RHI capabilities](../reference/rhi-capabilities.md)).

## Alternatives considered

- **Hand-written barriers in each pass:** correct on Metal by accident and wrong on DX12 by default.
- **Legacy DX12 resource-state barriers:** simpler but coarser. Enhanced barriers map 1:1 onto the states above.
- **No graph-level barriers on Metal at all:** fastest to write, and leaves DX12 to rediscover every hazard.
