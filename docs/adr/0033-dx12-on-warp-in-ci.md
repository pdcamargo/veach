# 0033: DX12 is verified on WARP in hosted CI, on a pinned Agility SDK

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0031), with ray tracing made optional.

## Context

Windows is verified only on GitHub-hosted runners ([ADR 0032](0032-windows-verified-in-hosted-ci.md)), which have
no GPU. The DX12 backend still has to pass the same golden images as Metal
([ADR 0031](0031-image-comparison-and-goldens.md)). WARP, Microsoft's software D3D12 device, ships on NuGet
(`Microsoft.Direct3D.WARP`) versioned with the Agility SDK: it supports SM 6.6 descriptor heaps, enhanced barriers
and DXR 1.1 inline ray queries, and 1.0.12 fixed a BVH-build race. The OS's own D3D12 on the runner varies with the
image and has no debug layer.

## Decision

- **Pin the runtime.** `cmake/VeachDependencies.cmake` pins the D3D12 Agility SDK **1.620.2** and WARP **1.0.22**,
  each a NuGet URL with a SHA256. Bump the two together: WARP releases follow Agility SDK releases.
- **Deploy it next to every executable that can create a device.** `veach_deploy_d3d12()`
  (`cmake/VeachTargets.cmake`) copies `D3D12Core.dll` and `d3d12SDKLayers.dll` to `D3D12\` and `d3d10warp.dll`
  next to the exe. `dx12_device.cpp` exports `D3D12SDKVersion` and `D3D12SDKPath = ".\\D3D12\\"`. The debug layer
  comes from the package, so hosted runners need nothing installed. WARP is for testing only and never ships
  ([ADR 0023](0023-license-mit.md)).
- **Feature floor:** Shader Model 6.6, resource binding tier 3 and enhanced barriers (the hardware target of
  [ADR 0003](0003-platforms-and-hardware-target.md)). Less fails with `rhi.backend_unavailable`, which golden tests
  treat as a skip, or as a failure under `VEACH_REQUIRE_GPU=1`.
- **Ray tracing is optional.** DXR 1.1 is reported as `Capabilities::hardware_ray_tracing` and used only by photo
  mode ([ADR 0041](0041-photo-mode.md)). WARP supports it, so CI runs the photo-mode goldens too.
- **Adapter choice:** the first hardware adapter meeting the floor, in high-performance order; otherwise WARP.
  `VEACH_DX12_ADAPTER=warp` forces WARP. The device name says `(WARP)`.
- **CI:** the Windows job runs every DX12 test and golden on WARP with `VEACH_REQUIRE_GPU=1`, against the shared
  goldens at their normal limits.
- **Debug builds** turn on the debug layer; its errors are logged as `rhi.gpu_error`, so they fail the run like a
  Metal validation error.

## Consequences

- Every DX12 feature gets a CI proof on every push.
- WARP is a conformance reference, not a GPU: vendor driver bugs, performance and hardware timing races stay
  unverified until a Windows GPU runner exists (Blocked).
- WARP is slow, so CI scenes stay small and soak tests count hundreds of frames, not thousands.
- Every DX12-capable executable carries the runtime DLLs in its build folder.

## Alternatives considered

- **The OS's D3D12 runtime (no Agility SDK):** support changes with each runner image, no debug layer.
- **A paid GPU runner:** real hardware, but against ADR 0032 for now.
