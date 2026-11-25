# DX12 backend

**Status:** Implemented
**Area:** rhi
**Owner:** dev lead

## Summary

`--rhi dx12` runs Veach on D3D12 through the pinned Agility SDK. It renders the same images as Metal: the shared
goldens pass unchanged. There is no Windows machine, so every criterion is proven by a test in the hosted Windows
CI job, on WARP ([ADR 0030](../../adr/0030-windows-verified-in-hosted-ci.md),
[ADR 0031](../../adr/0031-dx12-on-warp-in-ci.md)). How to build it: [dx12-port](../../guides/dx12-port.md).

## Motivation

Windows is a target platform ([ADR 0002](../../adr/0002-target-platforms-and-hardware.md)). The RHI, the render
graph's barriers and the frame model were designed for DX12 from the start (ADRs 0024, 0025, 0029). This spec
makes the backend real. Backlog: VCH-012, VCH-002.

## Requirements

- MUST implement every `rhi::Device` and `rhi::CommandList` call, with the binding, barrier and frame models of
  ADRs 0024, 0025 and 0029 unchanged.
- MUST run on any adapter that has Shader Model 6.6, resource binding tier 3 and enhanced barriers. It MUST fall
  back to WARP when no hardware adapter qualifies, and MUST fail with `rhi.backend_unavailable` otherwise.
- MUST route debug-layer errors to error logs (`rhi.gpu_error`), so a debug-layer error fails the run.
- MUST pass the shared goldens (`tests/golden/*.png`) at the default tolerance, with no `.dx12` override.

## API sketch

Nothing new for users: `--rhi dx12`, `"rhi": "dx12"`. `VEACH_DX12_ADAPTER=warp` forces WARP. `BufferDesc` gains
`stride`, the element size of a structured-buffer view (DX12 needs it; Metal ignores it).

## Acceptance criteria

Each is one step of [dx12-port](../../guides/dx12-port.md), proven in the Windows CI job.

- **dx12.AC1**: `create_device(dx12)` on a hosted runner picks WARP, meets the feature floor, reports hardware
  ray tracing, and completes five frames of empty submissions with no debug-layer errors.
- **dx12.AC2**: Creating and destroying 70,000 storage buffers (two descriptors each) across frames never runs out
  of the 65,536-slot heap, because slots are freed only once the GPU is done with them. No debug-layer errors.
- **dx12.AC3**: A cleared 100×10 target, whose rows aren't 256-byte aligned, reads back with tight rows.
- **dx12.AC4**: `golden: background (dx12)` (only the clear pass) matches `tests/golden/background.png`, and
  `golden: triangle (raster, dx12)` matches `tests/golden/triangle_raster.png`.
- **dx12.AC5**: `golden: triangle (raytraced, dx12)` matches `tests/golden/triangle_raytraced.png`. Frame 1 builds
  the BLAS and TLAS; frames 2 and 3 refit the TLAS.
- **dx12.AC6**: `dx12: ray-traced soak`: 300 frames with two frames in flight exit 0 with no debug-layer errors.
- **dx12.AC7**: `mcp: golden through MCP (dx12)`: the MCP test drives the lab on DX12 to the ray-traced golden.
- **dx12.AC8**: `lab: windowed DX12 run`: a swapchain is created from the HWND and presents, the window is resized
  on frame 10, and the run shuts down cleanly with no errors. One exception: on the hosted runner some `Present`
  calls fail with `E_INVALIDARG`, with the window visible and sized like the swapchain, and neither debug layer
  explains it. Measured without the resize too (three 60-frame runs, one failure each at about 0.5 s), so it is
  not a resize vs frames-in-flight race. The test tolerates exactly that HRESULT, and only where
  `VEACH_CI_HOSTED_WINDOWS=1` (the hosted job sets it); elsewhere any error fails it
  (`tests/cmake/windowed_test.cmake`). Presenting on a real display is VCH-050 (blocked).

## Out of scope

- Real DXR hardware, vendor drivers, performance: VCH-050 (blocked, ADR 0030).
- Async compute, copy queues, placed resources and aliasing: not modeled by the RHI yet (ADR 0025).

## Open questions

None.

## Links

- ADRs: [0024](../../adr/0024-resource-binding-model.md), [0025](../../adr/0025-sync-and-barrier-model.md),
  [0028](../../adr/0028-golden-images-across-backends.md), [0029](../../adr/0029-frame-and-sync-model.md),
  [0030](../../adr/0030-windows-verified-in-hosted-ci.md), [0031](../../adr/0031-dx12-on-warp-in-ci.md)
- Backlog: VCH-012, VCH-002, VCH-049, VCH-050
- Guide: [dx12-port](../../guides/dx12-port.md)
