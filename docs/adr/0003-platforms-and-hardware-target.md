# 0003: Platforms and hardware target: bindless GPUs, ray tracing optional

Date: 2026-11-27

## Status

Accepted. Replaces the pathtracer-era hardware stance (old ADR 0002: hardware ray tracing as the baseline).

## Context

The real-time renderer is now a PBR rasterizer (ADR 0039); ray tracing serves only photo mode (ADR 0041). A space
game in the spirit of No Man's Sky should run on ordinary gaming hardware, not only on recent RT GPUs. The engine's
foundation, though, is bindless ([0027](0027-resource-binding-model.md)): shaders reach every resource through
descriptor handles, and the render graph, materials and scene upload are built on that. Development happens on
Apple silicon Macs; Windows is verified only in hosted CI ([0032](0032-windows-verified-in-hosted-ci.md)).

## Decision

**Platforms:** macOS on Apple silicon, and Windows x64. Nothing else for now.

**GPU floor (required):**

- **Metal:** Metal 3 with tier-2 argument buffers: every Apple silicon Mac (M1 and later), macOS 14 or later (shaders
  build with `-mmacosx-version-min=14.0`).
- **DX12:** Shader Model 6.6 (`ResourceDescriptorHeap`), resource binding tier 3 and enhanced barriers, through the
  pinned Agility SDK ([0033](0033-dx12-on-warp-in-ci.md)).

A device below the floor fails with `rhi.backend_unavailable`, and its hint says what is missing.

**Optional capabilities**, reported in `rhi::Capabilities`, never required by the real-time path:

- **Hardware ray tracing** (`hardware_ray_tracing`: Metal ray tracing, which every Apple silicon Mac supports and M3
  and later accelerate in hardware; DXR 1.1 on DX12). Used only by photo mode. `VEACH_DISABLE_RAY_TRACING=1` makes a
  device report none, and a golden test renders the real-time path that way (rt.AC2), so a dependency on ray tracing
  cannot creep in.
- **Mesh shaders** (`mesh_shaders`): not used; meshlets are rasterized by vertex pulling.

**Performance goal:** the real-time renderer is designed for 60 fps at 1080p on a base Apple M1-class GPU, and 60 fps
at 1440p on the reference machine (Apple M5 Max, the only GPU we can measure on). These are design targets until the
research phase sets per-feature budgets from measurements (open question 7 in ARCHITECTURE.md).

## Consequences

- Every Apple silicon Mac and most DX12 GPUs from the last several years can run the engine and its real-time path.
  Photo mode runs where ray tracing exists, and reports `rhi.backend_unavailable` elsewhere while the camera keeps
  rendering in real time.
- No legacy binding path: GPUs without SM 6.6 descriptor heaps or Metal 3 are out. That excludes very old hardware
  and keeps one binding model.
- Real-time rendering features can't assume ray queries (shadows, GI, reflections must be raster or compute
  techniques). Any future real-time use of ray tracing would be an optional enhancement with a fallback, and needs an
  ADR.
- The performance goal can only be checked on the reference machine and by scaling estimates; lower-end Macs are not
  measured until someone has one.

## Alternatives considered

- **Keep hardware ray tracing as the baseline.** Excludes a large part of the audience for features the product no
  longer has.
- **A lower floor (D3D12 feature level 11 binding tiers, Metal 2).** Needs a second, slot-based binding model next to
  the bindless one, for hardware that is now rare.
- **Linux with Vulkan now.** A third backend before the research phase knows what the renderer needs. SDL3 and Slang
  keep the door open.
