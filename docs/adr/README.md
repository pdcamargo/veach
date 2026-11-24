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
| 0002 | [Target platforms and hardware](0002-target-platforms-and-hardware.md)   | Accepted |
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
