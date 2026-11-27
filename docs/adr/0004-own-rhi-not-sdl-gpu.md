# 0004: Keep our own thin RHI; do not adopt SDL_GPU or a third-party renderer

Date: 2026-11-27

## Status

Accepted. Carries over the RHI design of the pathtracer-era series (old ADR 0009) and records the alternatives
weighed at the reset.

## Context

With ray tracing no longer the baseline (ADR 0003), the reset asked whether to keep `engine/rhi` or adopt something
else. What exists and works:

- Metal and DX12 backends plus a null backend, selected with `--rhi null|metal|dx12` or `"rhi"` in
  `veach.project.json`; null is the default, so headless runs and CI need no GPU.
- Bindless resources through `DescriptorHandle`s ([0027](0027-resource-binding-model.md)), validated in debug builds;
  Metal residency through one residency set ([0028](0028-metal-residency-set.md)).
- Explicit barriers derived by the render graph ([0029](0029-render-graph-sync-and-barriers.md)), frames in flight and
  deferred release ([0030](0030-frame-and-sync-model.md)).
- Per-pass GPU timestamps, GPU memory accounting, optional acceleration structures with inline ray queries.
- The DX12 backend verified on WARP in hosted CI ([0033](0033-dx12-on-warp-in-ci.md)), every golden shared across
  backends.

The toolbox will need more GPU compute than the renderer uses today (GPU-built terrain, procgen, readback to CPU
physics). The capability audit is [reference/rhi-capabilities.md](../reference/rhi-capabilities.md).

## Decision

- **Keep `engine/rhi`**: thin, modern-only, bindless, explicit synchronization, no legacy paths. Its interface is
  `rhi::Device` and `rhi::CommandList`; Slang shaders compiled offline for both targets
  ([0009](0009-shader-compilation.md)).
- **Every RHI feature is designed against Metal and DX12 before it lands**, with a null-backend implementation that
  does nothing and validates what it can.
- **Ray tracing stays as an optional capability** (BLAS/TLAS, inline ray queries), used by photo mode only.
- **Gaps the toolbox needs are added to this RHI**, each as its own change with tests on both backends. The audit
  lists them, and the backlog tracks them as research-phase inputs (VCH-071 to VCH-076).

## Consequences

- The render graph, extraction, materials, photo mode and the golden-image pipeline keep working unchanged.
- We own every backend line, and the gaps (indirect draws and dispatch, 3D textures, async compute, non-blocking
  readback) are ours to build.
- No third-party renderer decides our material model, scene format or frame structure.

## Alternatives considered

- **SDL_GPU (SDL3's GPU API).** Portable and already a dependency family (SDL3 runs our platform layer), but its
  resource model is slot-based with a strict layout, it has no bindless descriptors, and it has no ray tracing and no
  stated plans for it. Adopting it would throw away the working Metal and DX12 backends, the bindless material and
  scene model built on descriptor handles, photo mode, and the render graph's barrier model (SDL_GPU tracks hazards
  itself). Rejected.
- **Filament.** A complete PBR renderer with its own material system, scene and frame structure, and backends for
  Vulkan, Metal, OpenGL and WebGPU but no DX12. It would replace our renderer and extraction boundary rather than sit
  under them, and planet-scale features (terrain, atmosphere, oceans) would have to fit its design. No ray tracing for
  photo mode. Rejected.
- **Wicked Engine.** A whole engine (ECS, scene, editor, scripting, its own GPU abstraction over DX12, Vulkan and,
  since early 2026, Metal), with ray tracing. Using it means replacing Veach's engine, not its RHI, which contradicts
  the AI-native foundation that stays. Rejected.
- **OSPRay.** A ray-tracing renderer for scientific visualization, CPU-based (Embree) with GPU support through SYCL on
  Intel GPUs. Not a real-time game renderer, and as a photo-mode backend it would need a second scene representation
  and its own materials, so photo mode would stop matching the real-time materials ([0035](0035-material-model.md)).
  Rejected.
- **Vulkan everywhere (MoltenVK on Mac), NVRHI, Diligent, WebGPU.** Carried over from the original decision: a
  translation layer on the primary platform, no Metal backend (NVRHI), broad legacy support we don't want (Diligent),
  no bindless or ray tracing at the level we need (WebGPU).
