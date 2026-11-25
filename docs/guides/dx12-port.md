# DX12 porting brief (VCH-012)

The handoff for implementing `Dx12Device`. The design is final. This is the order to build it in, what DX12 will
resist, and which test proves each step. Read [ADR 0024](../adr/0024-resource-binding-model.md) (binding),
[ADR 0025](../adr/0025-sync-and-barrier-model.md) (barriers), [ADR 0029](../adr/0029-frame-and-sync-model.md)
(frames and fences) and [ADR 0026](../adr/0026-shader-compilation.md) (shaders) first. Each is short.

## Where things are

| What | Where |
|---|---|
| The interface to implement | `engine/rhi/include/veach/rhi/rhi.hpp` (`Device`), `command_list.hpp` (`CommandList`), `types.hpp` |
| The stub (compiles today; `static_assert` proves it implements everything) | `engine/rhi/src/dx12/dx12_device.cpp` |
| The reference implementation | `engine/rhi/src/metal/metal_device.mm` (same structure: slot maps, tracker, command list) |
| Frames, fences, retirement (use as is) | `engine/rhi/include/veach/rhi/submission_tracker.hpp` |
| Generational handles | `engine/rhi/src/slot_map.hpp` |
| DXIL for every shader | `ShaderLibrary::find(entry)->dxil`, built for `sm_6_6` on every Windows build (`cmake/VeachShaders.cmake`) |
| Backend selection | `engine/rhi/src/rhi.cpp` (`create_dx12_device`), `--rhi dx12` / `"rhi": "dx12"` |

## Decisions already made for you

- **Feature floor:** D3D12 with **Shader Model 6.6** (`ResourceDescriptorHeap`), **DXR 1.1** (inline ray queries),
  **enhanced barriers**. Use the Agility SDK, pinned like every other dependency: add it to
  `cmake/VeachDependencies.cmake` (NuGet `Microsoft.Direct3D.D3D12`, URL + SHA256), plus the
  `D3D12SDKVersion`/`D3D12SDKPath` exports. Record it in ADR 0013's successor.
- **One queue** (direct), **one `ID3D12Fence`**, and `SubmissionId` values *are* the fence values (ADR 0029).
- **Descriptor handles.** `rhi::DescriptorHandle::value` = heap index in the low 32 bits, 0 in the high 32 bits.
  Slang reads it as `uint2` and indexes `ResourceDescriptorHeap[h.x]` or, for samplers,
  `SamplerDescriptorHeap[h.x]`. Checked: `present.slang` compiles to exactly that.
- **Parameter blocks** go in root constants at `b0`: `SetGraphicsRoot32BitConstants` or
  `SetComputeRoot32BitConstants`, ≤ 64 DWORDs (`rhi::max_parameter_bytes == 256`). The C++ structs are already
  checked against the HLSL layout at compile time (VCH-041), so they are byte-identical to the `cbuffer`.

## Things DX12 will resist (and the answer)

1. **Separate SRV and UAV descriptors.** `descriptor(texture, View::read)` must return an SRV index and
   `View::storage` a UAV index. Create both at resource creation when the usage has both (`sampled|storage`,
   `storage` buffers). Metal ignores `View`; you can't.
2. **Descriptor-slot lifetime.** A slot can only be reused once the GPU is done with it. Free slots inside the
   `SubmissionTracker::retire()` callback together with the resource. Never free them directly in `destroy()`.
3. **Root signature.** One global signature: 64 root-constant DWORDs at `b0`, with
   `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`. Bind the two
   shader-visible heaps once per command list (`SetDescriptorHeaps`) before any draw or dispatch.
4. **Barriers.** Map `rhi::Barrier` to `D3D12_TEXTURE_BARRIER` / `D3D12_BUFFER_BARRIER` with the table in ADR 0025
   (layout / access / sync per `ResourceState`):
   - `before == after == shader_write`: a UAV barrier (sync `ALL_SHADING`, access `UNORDERED_ACCESS` on both sides).
   - `Barrier::discard`: `LayoutBefore = UNDEFINED` and the discard flag, but keep `SyncBefore`/`AccessBefore`
     from the real `before` state, because the previous frame may still be using the transient (ADR 0029, item 5).
   - Buffers have no layouts: access and sync only.
5. **Swapchain images** start and end in `present` (`D3D12_BARRIER_LAYOUT_PRESENT`). The graph already imports them
   with `final_state = present`. Use a flip-model `IDXGISwapChain3`, `GetCurrentBackBufferIndex()` in
   `acquire_image`, and RTVs from a small non-shader-visible RTV heap (attachments are not bindless).
6. **Render passes.** `begin_render_pass` → `OMSetRenderTargets` + `ClearRenderTargetView` for `LoadOp::clear`, or
   `ID3D12GraphicsCommandList4::BeginRenderPass`. `LoadOp::discard` maps to discard. There are no encoders: the
   pass boundaries are just recording.
7. **Residency.** `CommandList::use()` is a no-op on DX12, because committed resources are resident. Keep the
   calls: Metal needs them.
8. **Acceleration structures:**
   - The buffers live in `D3D12_BARRIER_LAYOUT_UNDEFINED`, created with
     `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE`, and never change layout.
   - `accel_build → accel_read` is access/sync only.
   - Sizes come from `GetRaytracingAccelerationStructurePrebuildInfo`.
   - Refit is `ALLOW_UPDATE` at build and `PERFORM_UPDATE` on refit.
   - `TlasInstance::transform` is already the row-major 3×4 of `D3D12_RAYTRACING_INSTANCE_DESC`. Copy it.
   - Instance descriptors are CPU-written, so keep **one upload buffer per frame slot** (ADR 0029), as Metal does.
   - The AS SRV goes in the heap: `descriptor(accel)` returns its index.
9. **Readback.** `copy_texture_to_buffer` → `CopyTextureRegion` into a `D3D12_HEAP_TYPE_READBACK` buffer. Rows must
   be 256-byte aligned (`D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`). The renderer expects tight rows
   (`width * 4`), so either repack on `map()` or teach `copy_texture_to_buffer` the pitch. Repacking is simpler.
10. **Upload.** `create_buffer` with initial data and `MemoryType::gpu`: copy through an upload buffer and retire
    the staging buffer. Metal used shared memory and didn't have to.
11. **Validation.** `DeviceDesc::validation` → `ID3D12Debug::EnableDebugLayer` (+ GPU-based validation
    optional). Route `ID3D12InfoQueue1` messages to `log::error("rhi", ...)` with `rhi.gpu_error` so the exit code
    turns 1, exactly as Metal does. Device removal (`GetDeviceRemovedReason`) is also `rhi.gpu_error`.

## Order of work, and what proves each step

| # | Step | Proof |
|---|---|---|
| 1 | Device, queue, fence, `SubmissionTracker`, debug layer; `create_dx12_device` returns a device | `veach-lab --headless --rhi dx12 --frames 3` exits 0 and logs `init rhi (dx12, device '<GPU>', hardware RT: yes)`. `[frames.AC*]` tests already pass (backend-independent) |
| 2 | Descriptor heaps + slot allocator, freed via `retire()` | Unit test: create/destroy 10k textures across frames; heap usage returns to baseline after `wait_idle()` |
| 3 | Buffers, textures, samplers, upload path | Same run with `--log-level debug` shows no debug-layer messages |
| 4 | Barriers (the ADR 0025 table), command lists, submit | `render.AC*` tests (they validate the graph's barriers backend-independently); the debug layer stays quiet with a clear-only frame |
| 5 | Graphics pipelines from DXIL, render passes, root constants | — |
| 6 | Readback | `--screenshot out.png` of the lab with no script: a background-only image. Compare it visually or with `veach imgdiff` against a Metal capture |
| 7 | **Raster triangle** | `golden: triangle (raster)` with `-DRHI=dx12` against the **shared** golden (ADR 0028): mean FLIP ≤ 0.01 |
| 8 | Swapchain, present, resize | `veach-lab --window --rhi dx12 --script scripts/triangle_raster.luau`; close the window: exit 0 |
| 9 | Compute pipelines, UAVs | — |
| 10 | BLAS/TLAS build + refit | **`golden: triangle (raytraced)`** with `-DRHI=dx12` (frame 1 builds, frames 2 and 3 refit) |
| 11 | Two frames in flight under load | `veach-lab --rhi dx12 --headless --frames 600 --script scripts/triangle_raytraced.luau`: no debug-layer errors, exit 0 |
| 12 | (Optional) `last_frame_gpu_ms` via timestamp queries | The MCP `gpu_timings` tool reports a non-zero value |

For steps 7 and 10, register the DX12 goldens next to the Metal ones in `tests/CMakeLists.txt` (`if(WIN32)`,
`-DRHI=dx12`), and add the `gpu-dx12` CI job on the Windows runner ([ci-runners](ci-runners.md)). The tests skip
(never pass) on machines that can't run them, and fail with `VEACH_REQUIRE_GPU=1`.

## Definition of done for the port

Both shared goldens pass on DX12 with the default tolerance and no per-backend overrides. The `gpu-dx12` CI job
runs them on every push to `main`. The debug layer reports nothing in the golden runs. ADRs 0024, 0025 and 0029
need no change; if they do, write the superseding ADR first.
