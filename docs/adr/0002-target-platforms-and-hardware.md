# 0002: Target platforms and hardware

Date: 2026-11-24

## Status

Accepted

## Context

Veach is a raytracing-first renderer for real-time games ([VISION.md](../VISION.md)). Supporting non-RT hardware would mean a second rendering path for GI, reflections and shadows, which roughly doubles renderer work and dilutes the quality target. Development happens on macOS (Apple Silicon) first and on Windows right after. The RHI has to fit two very different APIs, so it must be shaped by both from the start.

This ADR records a **product decision owned with the project lead**. Changing the hardware stance needs their sign-off, not just a new ADR.

## Decision

- **Platforms:** macOS on Apple Silicon and Windows (x64). Nothing else is a target for now.
- **GPU backends:**
  - Metal is the first real backend.
  - DX12 exists as a stub from day one (`engine/rhi/src/dx12/`, compiled only on Windows) so the interface is designed for two backends.
  - A null backend runs everywhere, for headless runs and CI. See [0009](0009-rhi-design.md).
- **Hardware stance:** GPUs with hardware ray tracing.
  - Windows: DXR 1.1-class GPUs.
  - Mac: Apple M3 or newer.
  - M1 and M2 may run but are not optimized for. No raster-only fallback path is planned.
- **Initial performance goal:** 60 fps at 1440p output, with upscaling, on current mid/high-end RT GPUs. We refine this with real measurements (Tracy traces, [0011](0011-profiling-tracy.md)) once a renderer exists.
- **CI:** builds and tests on macOS (arm64) and Windows runners. CI runs headless with the null RHI, because hosted runners have no RT GPUs.

## Consequences

- Renderer features (Candela GI, RT reflections, area-light shadows) can assume hardware RT and bindless resources.
- Fewer users can run Veach. That is accepted.
- Platform code has exactly two OS backends. SDL3 ([0008](0008-platform-sdl3.md)) keeps the door open for others without committing to them.
- GPU work can't be verified on CI. Golden-image and GPU-timing tests will need self-hosted RT runners (backlog).

## Alternatives considered

- **Include Linux (Vulkan) now.** A third backend before the first one exists. Deferred, not ruled out; SDL3 and Slang both support it.
- **Raster fallback for non-RT GPUs.** Doubles the lighting stack and caps quality decisions. Rejected as a product goal.
- **Vulkan everywhere (MoltenVK on Mac).** Loses native Metal RT and tooling on the primary dev platform. See [0009](0009-rhi-design.md).
- **Consoles, mobile, web.** Out of scope until the PC/Mac renderer is real.
