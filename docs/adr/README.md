# Architecture Decision Records

An ADR records one decision: the context, what we chose, what it costs, and what we rejected.

- **When:** write one for anything that changes how the engine is structured, a dependency, a convention, or a process rule. Code-local choices (naming inside a module, a helper's shape) don't need one.
- **How:** run `/adr <title>`, or copy [0000-template.md](0000-template.md) to the next free number (`NNNN-kebab-title.md`) and add it to the table below.
- **Immutable once Accepted:** to change a decision, write a new ADR and mark the old one `Superseded by NNNN`. Don't edit the old decision.
- **Product changes:** an ADR that contradicts [VISION.md](../VISION.md) or a non-negotiable in [ARCHITECTURE.md](../ARCHITECTURE.md) needs sign-off from the product owner first.
- **Same commit:** an ADR lands together with the code that implements it, or before it.

| #    | Title                                                                    | Status   |
|------|--------------------------------------------------------------------------|----------|
| 0001 | [Language and build system](0001-language-and-build-system.md)           | Accepted |
| 0002 | [Target platforms and hardware](0002-target-platforms-and-hardware.md)   | Accepted (CI line superseded by 0030 and 0036) |
| 0003 | [Shaders in Slang](0003-shaders-slang.md)                                | Accepted |
| 0004 | [ECS: Flecs v4](0004-ecs-flecs-v4.md)                                    | Accepted |
| 0005 | [Flecs barrel, not a wrapper](0005-flecs-barrel-not-wrapper.md)          | Accepted |
| 0006 | [Scripting with Luau](0006-scripting-luau.md)                            | Accepted |
| 0007 | [Asset formats and identity](0007-asset-formats-and-identity.md)         | Accepted |
| 0008 | [Platform layer on SDL3](0008-platform-sdl3.md)                          | Accepted |
| 0009 | [RHI design](0009-rhi-design.md)                                         | Accepted |
| 0010 | [Conventions and math](0010-conventions-and-math.md)                     | Accepted |
| 0011 | [Profiling with Tracy](0011-profiling-tracy.md)                          | Accepted |
| 0012 | [Code quality tooling](0012-code-quality-tooling.md)                     | Accepted |
| 0013 | [Dependency set](0013-dependency-set.md)                                 | Accepted |
| 0014 | [Engine lifecycle and phases](0014-engine-lifecycle-and-phases.md)       | Accepted |
| 0015 | [Veach modules and services](0015-veach-modules-and-services.md)         | Accepted |
| 0016 | [Renderer data and extraction](0016-renderer-data-and-extraction.md)     | Accepted |
| 0017 | [Git LFS for binaries](0017-git-lfs-for-binaries.md)                     | Accepted |
| 0018 | [Process: SDD, ADRs, backlog](0018-process-sdd-adrs-backlog.md)          | Accepted |
| 0019 | [License: MIT](0019-license-mit.md)                                      | Accepted |
| 0020 | [Repository layout](0020-repository-layout.md)                           | Accepted |
| 0021 | [Structured logging and errors](0021-structured-logging-and-errors.md)   | Accepted |
| 0022 | [Reflection everywhere](0022-reflection-everywhere.md)                   | Accepted |
| 0023 | [Leak checking with macOS `leaks`](0023-leak-checking-with-macos-leaks.md) | Accepted |
| 0024 | [Resource binding model (bindless)](0024-resource-binding-model.md)      | Accepted (Metal residency superseded by 0044) |
| 0025 | [Synchronization and barrier model](0025-sync-and-barrier-model.md)      | Accepted (frames part superseded by 0029) |
| 0026 | [Shader compilation](0026-shader-compilation.md)                         | Accepted |
| 0027 | [Image I/O and perceptual comparison](0027-image-io-and-comparison.md)    | Accepted |
| 0028 | [One shared set of golden images across backends](0028-golden-images-across-backends.md) | Accepted (Windows runner part superseded by 0030) |
| 0029 | [Frame and synchronization model](0029-frame-and-sync-model.md)          | Accepted |
| 0030 | [Windows is verified only through GitHub-hosted CI](0030-windows-verified-in-hosted-ci.md) | Accepted (Metal runner superseded by 0036) |
| 0031 | [DX12 is verified on WARP in hosted CI, on a pinned Agility SDK](0031-dx12-on-warp-in-ci.md) | Accepted |
| 0032 | [glTF import and cooked runtime formats](0032-asset-import-and-cooking.md) | Accepted |
| 0033 | [Material model: Slang interfaces with overridable defaults](0033-material-model.md) | Accepted |
| 0034 | [Lights in physical units, and a reference path tracer](0034-lights-and-reference-path-tracer.md) | Accepted (alpha part superseded by 0035) |
| 0035 | [Alpha testing by continuing opaque rays](0035-alpha-by-continuing-opaque-rays.md) | Accepted |
| 0036 | [No self-hosted GPU runner; real-GPU tests are gated before push](0036-no-self-hosted-gpu-runner.md) | Accepted |
| 0037 | [The real-time renderer: a visibility buffer over cook-time meshlets](0037-realtime-renderer-visibility-buffer.md) | Accepted (one shading pass superseded by 0040, direct lighting by 0041) |
| 0038 | [Physical camera exposure and the AgX tonemapper](0038-exposure-and-tonemapping.md) | Accepted (where exposure applies superseded by 0043) |
| 0039 | [Null descriptors per kind, and validated bindless handles in debug builds](0039-null-descriptors-and-bindless-validation.md) | Accepted |
| 0040 | [Temporal foundation: render resolution, jitter, surfaces, motion, history](0040-temporal-foundation.md) | Accepted (history copies superseded by 0045) |
| 0041 | [Direct lighting with ReSTIR DI](0041-restir-di.md)                      | Accepted |
| 0042 | [The denoiser: our own, SVGF/ReLAX-style](0042-denoiser.md)              | Accepted (history feedback superseded by 0045) |
| 0043 | [Upscaling: MetalFX on Apple GPUs, our own TAAU everywhere](0043-upscaling.md) | Accepted |
| 0044 | [Metal residency through one residency set](0044-metal-residency-set.md) | Accepted |
| 0045 | [Temporal renderer: ping-ponged history, the denoiser's history](0045-temporal-implementation-amendments.md) | Accepted |
