# 0009: RHI design

Date: 2026-11-24

## Status

Accepted

## Context

The renderer is raytracing-first on hardware-RT GPUs ([0002](0002-target-platforms-and-hardware.md)), with Metal first and DX12 next. A broad, legacy-compatible RHI would spend effort on paths we will never use. Headless runs, CI and agents need a backend that requires no GPU. v0 has no renderer, so the RHI must be a seam, not a framework.

## Decision

**Scope.** A thin, modern-only RHI (`engine/rhi`, namespace `veach::rhi`): bindless resources, hardware ray tracing, explicit synchronization. There are no legacy binding models, no fixed-function paths and no non-RT fallback.

**Backends.**

- `null`: no GPU. It validates native handles and does nothing else. It is the default (`"rhi": "null"`) and is what headless and CI runs use.
- `metal`: a stub compiled on Apple. It creates the real `MTLCreateSystemDefaultDevice()`, reports `Capabilities` (`device_name`, `hardware_ray_tracing` via `supportsRaytracing`, `bindless`, `mesh_shaders`), and binds the device to a `CAMetalLayer` in `create_surface`.
- `dx12`: a stub compiled on Windows. `create_dx12_device` returns `rhi.not_implemented` (`TODO(VCH-012)`). Requesting a backend not built for the platform is `rhi.backend_unavailable`.

**Interface v0.** `Device` exposes only `backend()`, `capabilities()`, `create_surface(const NativeWindowHandle&)` and `destroy_surface()`.

- Every backend runs `validate_surface_handle()` first, which logs the handle at debug level.
- The engine creates a surface for the primary window when it has a native handle.
- Resources, command recording, pipelines and acceleration structures arrive with the render graph (VCH-010), each designed against both Metal and DX12 before it lands.

**Selection.** `rhi` in `veach.project.json`, overridable with `--rhi null|metal|dx12`.

## Consequences

- Headless runs never need a GPU, and CI stays green on hosted runners.
- The Metal stub already proves the window → `NativeWindowHandle` → RHI handshake on real hardware.
- The real design work, resources and command buffers, is deliberately deferred. This ADR will be extended by new ADRs, not edited.
- We own every backend line. That is the cost of thinness.

## Alternatives considered

- **NVRHI.** No Metal backend.
- **Diligent Engine.** Broad legacy support (GL, D3D11) we don't want, and a large API surface.
- **SDL_GPU.** Portable, but no ray tracing and no bindless model at the level we need.
- **WebGPU (Dawn/wgpu).** No hardware RT.
- **Vulkan everywhere + MoltenVK.** Indirect access to Metal RT, a translation layer on the primary platform, and worse Apple tooling.
