# 0003: Shaders in Slang

Date: 2026-11-24

## Status

Accepted

## Context

We need one shader language that compiles to MSL (Metal) and DXIL (DX12), supports ray tracing stages, and allows real modularity for extensible materials. Users should be able to override just the parts of a material they care about, the way Bevy's `ExtendedMaterial` works ([VISION.md](../VISION.md)).

## Decision

- Shaders are written in **Slang**, compiled to MSL for Metal and DXIL for DX12. There is no custom shader language and no shading abstraction on top of Slang ([ARCHITECTURE.md](../ARCHITECTURE.md): "not abstracted").
- **Materials are Slang modules implementing interfaces with default implementations.** A base material interface declares functions such as `evalAlbedo`, `evalEmission` and `displaceVertex`, each with a default. A user material implements the interface and overrides only the functions it needs. The engine composes it with Slang's interface and generics features, not with text substitution.
- Render passes are extensible in the same spirit: passes consume interfaces, not concrete material types.
- **Not integrated yet.** Slang is not a dependency in v0. Integrating the compiler and offline shader compilation, the material interface, and hot reload are backlog items ([BACKLOG.md](../backlog/BACKLOG.md)).

## Consequences

- One source of truth for both backends. Ray tracing pipelines, bindless access and generics are available in one language.
- We depend on the Slang compiler's quality for MSL output. Backend bugs land on us, and we pin the version like any other dependency.
- Material extension is type-checked by the shader compiler instead of being string-spliced.
- Agents write Slang directly. There is no Veach shader dialect to learn or document.

## Alternatives considered

- **HLSL + DXC + SPIRV-Cross to MSL.** Mature for DX12, but the MSL path goes through cross-compilation, and HLSL lacks Slang's interfaces and modules for material composition.
- **MSL + HLSL written twice.** Doubles shader work and drifts.
- **A custom shading language or graph.** A large ongoing cost, and agents would have to learn it. A node graph can come later as a generator of Slang.
- **WGSL.** No hardware RT. Rejected.
