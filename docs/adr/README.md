# Architecture Decision Records

An ADR records one decision: the context, what we chose, what it costs, and what we rejected.

- **When:** write one for anything that changes how the engine is structured, a dependency, a convention, or a process rule. Code-local choices (naming inside a module, a helper's shape) don't need one.
- **How:** run `/adr <title>`, or copy [0000-template.md](0000-template.md) to the next free number (`NNNN-kebab-title.md`) and add it to the table below.
- **Immutable once Accepted:** to change a decision, write a new ADR and mark the old one `Superseded by NNNN`. Don't edit the old decision.
- **Product changes:** an ADR that contradicts [VISION.md](../VISION.md) or a non-negotiable in [ARCHITECTURE.md](../ARCHITECTURE.md) needs sign-off from the product owner first.
- **Series:** the series restarted at [0001](0001-reset.md) when the raytracing-first era ended. The old ADRs live in the `pathtracer-era` git tag (`git show pathtracer-era:docs/adr/<file>`); their numbers mean nothing in this series.
- **Same commit:** an ADR lands together with the code that implements it, or before it.

| #    | Title | Status |
|------|-------|--------|
| 0001 | [Reset: from a raytracing-first engine to the foundation of a space-game toolbox](0001-reset.md) | Accepted |
| 0002 | [Language and build system](0002-language-and-build-system.md) | Accepted |
| 0003 | [Platforms and hardware target: bindless GPUs, ray tracing optional](0003-platforms-and-hardware-target.md) | Accepted |
| 0004 | [Keep our own thin RHI; do not adopt SDL_GPU or a third-party renderer](0004-own-rhi-not-sdl-gpu.md) | Accepted |
| 0005 | [ECS: Flecs v4](0005-ecs-flecs-v4.md) | Accepted |
| 0006 | [Flecs barrel, not a wrapper](0006-flecs-barrel-not-wrapper.md) | Accepted |
| 0007 | [Scripting with Luau](0007-scripting-luau.md) | Accepted |
| 0008 | [Shaders in Slang](0008-shaders-slang.md) | Accepted |
| 0009 | [Shader compilation](0009-shader-compilation.md) | Accepted |
| 0010 | [Asset formats and identity](0010-asset-formats-and-identity.md) | Accepted |
| 0011 | [glTF import and cooked runtime formats](0011-gltf-import-and-cooking.md) | Accepted |
| 0012 | [Platform layer on SDL3](0012-platform-sdl3.md) | Accepted |
| 0013 | [Conventions and math](0013-conventions-and-math.md) | Accepted |
| 0014 | [Profiling with Tracy](0014-profiling-tracy.md) | Accepted |
| 0015 | [Code quality tooling](0015-code-quality-tooling.md) | Accepted |
| 0016 | [Leak checking with macOS `leaks`](0016-leak-checking-with-macos-leaks.md) | Accepted |
| 0017 | [Dependency set](0017-dependency-set.md) | Accepted |
| 0018 | [Engine lifecycle and phases](0018-engine-lifecycle-and-phases.md) | Accepted |
| 0019 | [Veach modules and services](0019-veach-modules-and-services.md) | Accepted |
| 0020 | [Renderer data and the extraction boundary](0020-renderer-data-and-extraction.md) | Accepted |
| 0021 | [Git LFS for binaries](0021-git-lfs-for-binaries.md) | Accepted |
| 0022 | [Process: SDD, ADRs, backlog](0022-process-sdd-adrs-backlog.md) | Accepted |
| 0023 | [License: MIT](0023-license-mit.md) | Accepted |
| 0024 | [Repository layout](0024-repository-layout.md) | Accepted |
| 0025 | [Structured logging and errors](0025-structured-logging-and-errors.md) | Accepted |
| 0026 | [Reflection everywhere](0026-reflection-everywhere.md) | Accepted |
| 0027 | [Resource binding model: bindless handles, null descriptors, validated in debug](0027-resource-binding-model.md) | Accepted |
| 0028 | [Metal residency through one residency set; passes name only what the GPU writes](0028-metal-residency-set.md) | Accepted |
| 0029 | [Render graph synchronization and barrier model](0029-render-graph-sync-and-barriers.md) | Accepted |
| 0030 | [Frame and synchronization model](0030-frame-and-sync-model.md) | Accepted |
| 0031 | [Image comparison with FLIP, and one shared set of golden images](0031-image-comparison-and-goldens.md) | Accepted |
| 0032 | [Windows is verified only through GitHub-hosted CI](0032-windows-verified-in-hosted-ci.md) | Accepted |
| 0033 | [DX12 is verified on WARP in hosted CI, on a pinned Agility SDK](0033-dx12-on-warp-in-ci.md) | Accepted |
| 0034 | [No self-hosted GPU runner; real-GPU tests run in the pre-push gate](0034-pre-push-gate-no-gpu-runner.md) | Accepted |
| 0035 | [Material model: Slang interfaces with overridable defaults](0035-material-model.md) | Accepted |
| 0036 | [Lights are components in photometric units](0036-lights-in-photometric-units.md) | Accepted |
| 0037 | [Physical camera exposure and the AgX tonemapper](0037-exposure-and-agx.md) | Accepted |
| 0038 | [The physical sky: Hillaire LUTs shared by both renderers, one Sky component](0038-physical-sky.md) | Accepted |
| 0039 | [The real-time renderer is a PBR rasterizer: no real-time GI or path tracing](0039-realtime-renderer-is-raster-pbr.md) | Accepted |
| 0040 | [The interim real-time path: the visibility buffer with one basic shading pass](0040-interim-realtime-path-visibility-buffer.md) | Accepted |
| 0041 | [Photo mode: the reference path tracer, accumulated while the camera is still](0041-photo-mode.md) | Accepted |
| 0042 | [The space toolbox is a layer above the engine, in `space/`](0042-space-toolbox-layering.md) | Accepted |
