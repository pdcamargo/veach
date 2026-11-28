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
| 0040 | [The interim real-time path: the visibility buffer with one basic shading pass](0040-interim-realtime-path-visibility-buffer.md) | Superseded by 0060 |
| 0041 | [Photo mode: the reference path tracer, accumulated while the camera is still](0041-photo-mode.md) | Accepted |
| 0042 | [The space toolbox is a layer above the engine, in `space/`](0042-space-toolbox-layering.md) | Accepted |
| 0043 | [Determinism tiers, strict floating point and veach::dmath](0043-determinism-tiers-and-strict-floating-point.md) | Accepted |
| 0044 | [Counter-based hash randomness for generation and simulation](0044-counter-based-hash-randomness.md) | Accepted |
| 0045 | [Large-world positions: doubles in a reference-frame tree, float32 only on the GPU, no floating origin](0045-large-world-positions-and-reference-frames.md) | Accepted |
| 0046 | [64-bit values and positions across Luau, JSON and MCP](0046-64-bit-values-across-luau-json-mcp.md) | Accepted |
| 0047 | [The universe clock: int64 ticks, exact orbital phase, time-warp policy](0047-universe-clock.md) | Accepted |
| 0048 | [Bodies on rails: a barycenter tree of registered motion models; suns and eclipses as queries](0048-bodies-on-rails.md) | Accepted |
| 0049 | [The scale model: body, orbit and atmosphere scale factors; what never scales; derived quantities](0049-scale-model.md) | Accepted |
| 0050 | [Universe addresses and hashed seeds](0050-universe-addresses-and-seeds.md) | Accepted |
| 0051 | [Generation is a staged pipeline of typed physical records, registered by stable id, that never reads scale](0051-staged-generation-pipeline.md) | Accepted |
| 0052 | [Procgen runtime: code-first stages and JSON field graphs, CPU-authoritative, GPU visual-only, a Luau procgen sandbox](0052-procgen-runtime.md) | Accepted |
| 0053 | [Terrain is a signed density field in chunked body-local octrees, meshed on the CPU](0053-terrain-density-field.md) | Accepted |
| 0054 | [Terrain edits: brush operations on the wire, absolute override samples at rest; water as levels](0054-terrain-edits-and-water.md) | Accepted |
| 0055 | [Universe identity and generator versioning: genesis, epochs, stability classes, body pinning, save layout](0055-universe-identity-and-versioning.md) | Accepted |
| 0056 | [Multiplayer: server-authoritative replication, one world per process with an authority role](0056-server-authoritative-multiplayer.md) | Accepted |
| 0057 | [Simulation LOD: four tiers per observer, baseline plus deltas plus clock functions, verified catch-up](0057-simulation-lod.md) | Accepted |
| 0058 | [Physics: Jolt in double precision, one scene per frame, per-body gravity, deterministic collider installs](0058-physics-jolt.md) | Accepted |
| 0059 | [Gravity, flight and travel: sphere-of-influence gravity, flight-model plug-ins, the travel governor](0059-gravity-flight-travel.md) | Accepted |
| 0060 | [The real-time renderer: a GPU-driven visibility buffer with clustered lights and forward transparency](0060-gpu-driven-visibility-buffer.md) | Accepted |
| 0061 | [Temporal accumulation at native resolution; no upscaling](0061-temporal-accumulation-no-upscaling.md) | Accepted |
| 0062 | [Shadows, ambient light and reflections without ray tracing](0062-shadows-ambient-reflections.md) | Accepted |
| 0063 | [Astronomical rendering: per-body atmospheres, N suns, distant bodies, starfield, depth and exposure](0063-astronomical-rendering.md) | Accepted |
| 0064 | [Quality presets never change simulation outcomes](0064-quality-presets.md) | Accepted |
| 0065 | [Engine areas for the toolbox: frames, physics, procgen, voxel; what stays in space/](0065-engine-areas-for-the-toolbox.md) | Accepted |
| 0066 | [The owner's product decisions after the research phase](0066-owner-decisions-research-phase.md) | Accepted |
| 0067 | [veach::dmath on LLVM libc's header-only math, one no-FMA path](0067-dmath-on-llvm-libc.md) | Accepted |
| 0068 | [Amendments from SP-05: orbit scale at least body scale, radius in the system stage, habitability bias](0068-amendments-from-sp-05.md) | Accepted |
| 0069 | [Amendments from SP-04: the phase formula, the Kepler starter, binary pairs, pausing the clock](0069-amendments-from-sp-04.md) | Accepted |
| 0070 | [Rules for Luau generators, from SP-07](0070-generator-sandbox-rules.md) | Accepted |
| 0071 | [Field-graph precision rules, from SP-06](0071-field-graph-precision-rules.md) | Accepted |
| 0072 | [Runtime meshes live in a renderer-owned MeshStore of immutable snapshots](0072-runtime-meshes.md) | Accepted |
