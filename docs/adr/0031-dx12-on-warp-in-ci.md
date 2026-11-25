# 0031: DX12 is verified on WARP in hosted CI, on a pinned Agility SDK

Date: 2026-11-25

## Status

Accepted

## Context

Windows is verified only on GitHub-hosted runners ([ADR 0030](0030-windows-verified-in-hosted-ci.md)), and they
have no GPU. The DX12 backend (VCH-012) still has to pass the same golden images as Metal
([ADR 0028](0028-golden-images-across-backends.md)), the ray-traced one included.

WARP is Microsoft's software D3D12 device. It ships on NuGet (`Microsoft.Direct3D.WARP`), versioned with the
Agility SDK, and has to be conformant. What its release notes (checked 2026-11-25) say:

- DXR 1.1 support since 1.0.8, which covers inline ray queries (`RayQuery`, our `triangle_rt.slang`).
- 1.0.12 fixed a race in BVH builds that could corrupt acceleration structures.
- 1.0.18 added DXR 1.2 and Shader Model 6.9.
- 1.0.22 (2026-10-27) targets Agility SDK 1.620 and supports dynamically indexed `RayQuery` arrays.
- It has supported enhanced barriers and SM 6.6 descriptor heaps for many versions.

So WARP can run both goldens. The OS's own D3D12 on the runner image is older, varies with the image, and has no
debug layer unless the Graphics Tools feature is installed.

## Decision

- **Pin the runtime.** `cmake/VeachDependencies.cmake` pins the D3D12 Agility SDK **1.620.2** and WARP
  **1.0.22**, each as a NuGet URL with a SHA256. This extends the dependency set of
  [ADR 0013](0013-dependency-set.md). Bump the two together: WARP releases follow Agility SDK releases.
- **Deploy it next to every executable that can create a device.** `veach_deploy_d3d12()` in
  `cmake/VeachTargets.cmake` copies `D3D12Core.dll` and `d3d12SDKLayers.dll` to `D3D12\`, and `d3d10warp.dll`
  next to the exe. `dx12_device.cpp` exports `D3D12SDKVersion` and `D3D12SDKPath = ".\\D3D12\\"`. The debug layer
  comes from the package, so hosted runners need nothing installed.
- **Feature floor:** Shader Model 6.6, resource binding tier 3 and enhanced barriers. Anything less fails with
  `rhi.backend_unavailable`, which golden tests treat as "skip" (or as a failure under `VEACH_REQUIRE_GPU=1`).
  DXR 1.1 is reported as `Capabilities::hardware_ray_tracing`.
- **Adapter choice:** the first hardware adapter that meets the floor, in high-performance order; otherwise
  WARP. `VEACH_DX12_ADAPTER=warp` forces WARP on a machine that has a GPU. The device name says `(WARP)`.
- **CI:** the Windows job runs every DX12 test and golden on WARP with `VEACH_REQUIRE_GPU=1`. Goldens are the
  shared ones at the default tolerance. A `.dx12.png` override follows ADR 0028's rules, and a WARP-only
  difference is not a reason for one.
- **Debug builds** turn on the debug layer. Its errors are logged at error level (`rhi.gpu_error`), so a debug
  layer error fails the run like a Metal validation error does.

## Consequences

- Every DX12 feature, ray queries included, gets a CI proof on every push. The port is done when both shared
  goldens pass on WARP unchanged.
- WARP is a conformance reference, not a GPU. It cannot catch vendor driver bugs, performance problems or a
  hardware-specific timing race. That stays blocked until a Windows GPU runner exists (VCH-050).
- WARP is slow: ray tracing at 1280×720 costs it a noticeable fraction of a second per frame. CI scenes stay
  small, and soak tests are counted in hundreds of frames, not thousands.
- Every DX12-capable executable carries about 15 MB of runtime DLLs in its build folder.

## Alternatives considered

- **Skip the ray-traced golden on Windows and verify RT on DX12 later:** not needed, because WARP does DXR 1.1.
- **The OS's D3D12 runtime (no Agility SDK):** what it supports changes with each runner image, and it has no
  debug layer on hosted runners. Pinning makes CI results reproducible.
- **A paid GPU runner:** real hardware, but against ADR 0030 for now. That is VCH-050.
