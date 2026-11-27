# 0008: Shaders in Slang

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0003).

## Context

We need one shader language that compiles to MSL (Metal) and DXIL (DX12), covers raster, compute and ray queries
(the latter for photo mode), and allows real modularity for extensible materials: a user overrides just the parts of
a material they care about, the way Bevy's `ExtendedMaterial` works.

## Decision

- Shaders are written in **Slang**, compiled to MSL for Metal and DXIL for DX12. There is no custom shader language
  and no shading abstraction on top of Slang.
- **Materials are Slang types implementing interfaces with default implementations**; a user material overrides
  only the functions it needs. Composition uses Slang interfaces and generics, not text substitution. The model is
  [0035](0035-material-model.md).
- Render passes are extensible in the same spirit: they consume interfaces, not concrete material types.
- Compilation (offline, every target) is [0009](0009-shader-compilation.md).

## Consequences

- One source of truth for both backends, with bindless access, ray queries and generics in one language.
- We depend on the Slang compiler's MSL and DXIL quality; backend bugs land on us, and we pin its version.
- Material extension is type-checked by the shader compiler.
- Agents write Slang directly; there is no Veach shader dialect.

## Alternatives considered

- **HLSL + DXC + SPIRV-Cross to MSL.** Mature on DX12, but MSL goes through cross-compilation and HLSL lacks Slang's
  interfaces and modules for material composition.
- **MSL and HLSL written twice.** Doubles shader work and drifts.
- **A custom shading language or graph.** A large ongoing cost agents would have to learn. A node graph (a future
  nature or material editor) can generate Slang.
- **WGSL.** No ray queries and no bindless model at the level we need.
