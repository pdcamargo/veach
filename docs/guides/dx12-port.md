# DX12 port (VCH-012)

How `Dx12Device` is built. The design is final; this guide gives the order, what DX12 resists, and which CI job
proves each step. Nobody has a Windows machine ([ADR 0030](../adr/0030-windows-verified-in-hosted-ci.md)): every
step is one small commit, proven by the hosted Windows job on WARP ([ADR 0031](../adr/0031-dx12-on-warp-in-ci.md)),
so a red job points at one change. Spec and criteria: [dx12-backend](../specs/dx12-backend/spec.md).

Read first: [ADR 0024](../adr/0024-resource-binding-model.md) (binding),
[ADR 0025](../adr/0025-sync-and-barrier-model.md) (barriers), [ADR 0029](../adr/0029-frame-and-sync-model.md)
(frames and fences), [ADR 0026](../adr/0026-shader-compilation.md) (shaders).

## Where things are

| What | Where |
|---|---|
| The interface | `engine/rhi/include/veach/rhi/rhi.hpp` (`Device`), `command_list.hpp`, `types.hpp` |
| The backend (Windows only) | `engine/rhi/src/dx12/dx12_device.cpp` |
| The reference implementation | `engine/rhi/src/metal/metal_device.mm` (same structure: slot maps, tracker, command list) |
| Frames, fences, retirement (use as is) | `engine/rhi/include/veach/rhi/submission_tracker.hpp` |
| DXIL for every shader | `ShaderLibrary::find(entry)->dxil`, `sm_6_6`, built on every Windows build (`cmake/VeachShaders.cmake`) |
| Runtime DLLs | Agility SDK + WARP, pinned in `cmake/VeachDependencies.cmake`, deployed by `veach_deploy_d3d12()` |
| DX12 tests | `tests/dx12_tests.cpp` (Windows only), the `golden: ... (dx12)` tests in `tests/CMakeLists.txt` |

## Checking a change before CI

You can't run Windows code on a Mac, but you can type-check it. With `clang-cl` (Homebrew LLVM) and
[xwin](https://github.com/Jake-Shadle/xwin) (`cargo install xwin`, then `xwin --accept-license splat --output ~/.xwin`),
compile a file with `-fsyntax-only` against `~/.xwin/crt/include`, `~/.xwin/sdk/include/{ucrt,um,shared,winrt}`
and the Agility SDK's `build/native/include`. This catches most compile errors before a push. MSVC is still the
judge: watch the Windows job.

## Decisions already made

- **Feature floor:** Shader Model 6.6 (`ResourceDescriptorHeap`), resource binding tier 3, enhanced barriers.
  DXR 1.1 sets `hardware_ray_tracing`. Adapter: the best hardware one that qualifies, else WARP.
- **One direct queue, one `ID3D12Fence`.** `SubmissionId` values are the fence values (ADR 0029).
  Command allocators come from a pool. Each one remembers its last submission, and is reset only after that
  submission completes.
- **Descriptor handles:** `DescriptorHandle::value` holds the heap index in its low 32 bits. Slang reads it as
  `uint2` and indexes `ResourceDescriptorHeap[h.x]` or `SamplerDescriptorHeap[h.x]` (checked in the generated
  HLSL). Index 0 of each heap is reserved, so a zero handle stays "none" as on Metal.
- **Parameter blocks** are root constants at `b0` (Slang emits `cbuffer params : register(b0)`), at most 64
  DWORDs (`rhi::max_parameter_bytes == 256`). The C++ structs are checked against the HLSL layout at compile time
  (VCH-041).

## Things DX12 will resist (and the answer)

1. **Structured buffers need a stride.** A `StructuredBuffer<T>` SRV has to know `sizeof(T)`, so `BufferDesc` has
   `stride`. With `stride == 0`, the views are raw (`ByteAddressBuffer`).
2. **Separate SRV and UAV descriptors.** `descriptor(x, View::read)` returns an SRV index and `View::storage` a
   UAV index. Create both at creation when the usage allows both. Upload and readback heaps can't have UAVs.
3. **Descriptor-slot lifetime.** Free a slot in the `SubmissionTracker::retire()` callback, together with the
   resource. Never free it directly in `destroy()`.
4. **Root signature.** One global signature: 64 root-constant DWORDs at `b0`, flags
   `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`. Each list calls `SetDescriptorHeaps`
   *before* `SetGraphicsRootSignature`/`SetComputeRootSignature`, which directly indexed heaps require. Then it
   sets both signatures once.
5. **Barriers.** `rhi::Barrier` maps to `D3D12_TEXTURE_BARRIER`/`D3D12_BUFFER_BARRIER` with ADR 0025's table:
   - `before == after == shader_write` is a UAV barrier.
   - `discard` is honored as a plain transition from the real `before` state, which keeps the contents. That is
     always correct; discarding is an optimization for later. The real `before` matters because the previous
     frame may still use the transient (ADR 0029).
   - `present` is layout `PRESENT` with no access and sync `NONE`.
   - Buffers have no layouts.
   - New textures start in `COMMON`. The first barrier says `before = undefined`, which is legal from any layout.
6. **BLAS → TLAS in one pass.** The lab builds both in one graph pass, so the graph emits no barrier between
   them. Metal orders them through tracked encoders. DX12 needs a global barrier (AS write → AS read, sync
   `BUILD_RAYTRACING_ACCELERATION_STRUCTURE`) before every TLAS build or refit.
7. **Render passes.** `OMSetRenderTargets`, then `ClearRenderTargetView` for `LoadOp::clear`. RTVs live in a small
   CPU-only heap, because attachments are not bindless. Set the viewport and scissor to the attachment size, which
   Metal does implicitly. Use `CULL_MODE_NONE`, because Metal doesn't cull by default and the triangle is
   counter-clockwise.
8. **Swapchain images** start and end in `present` (`D3D12_BARRIER_LAYOUT_PRESENT`). Use a flip-model
   `IDXGISwapChain3`, `GetCurrentBackBufferIndex()` in `acquire_image`, and `wait_idle()` before `ResizeBuffers`.
9. **Residency.** `use()` is a no-op: committed resources are resident.
10. **Acceleration structures:**
    - Buffers are created with `RAYTRACING_ACCELERATION_STRUCTURE | ALLOW_UNORDERED_ACCESS` in layout
      `UNDEFINED`.
    - Sizes come from `GetRaytracingAccelerationStructurePrebuildInfo`; scratch is `max(build, update)`.
    - A refit is `PERFORM_UPDATE` in place.
    - `TlasInstance::transform` is already `D3D12_RAYTRACING_INSTANCE_DESC::Transform`.
    - Instance descriptors use one upload buffer per frame slot.
    - `descriptor(tlas)` is an AS SRV with no resource, only its GPU address.
11. **Readback.** `CopyTextureRegion` needs rows aligned to 256 bytes, and the renderer expects tight rows. When
    the rows aren't already aligned, copy into a pitched staging buffer and repack it into the readback buffer
    in `map()`.
12. **Upload.** `MemoryType::gpu` buffers with initial data are copied from a staging upload buffer on their own
    submission, and the staging buffer is retired.
13. **Validation.** The debug layer comes from the Agility SDK. `ID3D12InfoQueue1::RegisterMessageCallback`
    routes errors to `log::error(..., rhi.gpu_error)`, and device removal is reported the same way.

## Steps, and the CI job that proves each

Each step is one commit (or a few, all small) and ends when the **Windows** job of `.github/workflows/ci.yml` is
green with its proof. From step 3 on, the Windows job sets `VEACH_REQUIRE_GPU=1`, so a DX12 golden that skips
counts as a failure.

| # | Step | Proof in the Windows job | Criterion |
|---|---|---|---|
| 1 | Agility SDK + WARP pinned and deployed; adapter choice; debug layer to logs; queue, fence, frames, command-list pool | `unit: [dx12.AC1] ...` | dx12.AC1 |
| 2 | Descriptor heaps and slot allocator (freed via `retire()`); buffers (`stride`), textures, samplers; upload path | `unit: [dx12.AC2] ...` | dx12.AC2 |
| 3 | Barriers (the ADR 0025 table); render passes with clear; RTVs; readback with repack | `golden: background (dx12)` | dx12.AC3 |
| 4 | Root signature, root constants, graphics pipelines from DXIL, viewport and scissor | `golden: triangle (raster, dx12)` | dx12.AC4 |
| 5 | Compute pipelines and UAVs; BLAS and TLAS build and refit; the BLAS → TLAS barrier | `golden: triangle (raytraced, dx12)` | dx12.AC5 |
| 6 | Two frames in flight under load | `dx12: ray-traced soak` (300 frames) | dx12.AC6 |
| 7 | MCP on DX12 | `mcp: golden through MCP (dx12)` | dx12.AC7 |
| 8 | Swapchain, present, resize (VCH-002) | `lab: windowed DX12 run`, or the criterion moves to VCH-050 with the reason | dx12.AC8 |

GPU timestamps (`last_frame_gpu_ms`) are not a step: VCH-049.

## Definition of done

Both shared goldens pass on DX12 (WARP) with the default tolerance and no per-backend override, and the debug
layer reports nothing in any DX12 test. ADRs 0024, 0025 and 0029 need no change; if one does, write the
superseding ADR first.
