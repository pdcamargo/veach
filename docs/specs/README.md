# Specs

Capabilities are specified before they are built (spec-driven development). How to write one:
[writing specs](../guides/writing-specs.md). Create one with `/spec <name>`.

Workflow: spec (Draft → Approved) → ADR if the change is architectural → implement → tests named after the
acceptance criteria (`[area.ACn] ...`) → status Implemented.

| Spec | Area | Status |
|---|---|---|
| [Engine lifecycle](engine-lifecycle/spec.md) | runtime | Implemented |
| [ECS and phases](ecs-and-phases/spec.md) | ecs | Implemented |
| [Scripting: Luau API v0](scripting-luau-api-v0/spec.md) | script | Implemented |
| [Asset system core](asset-system-core/spec.md) | asset | Implemented |
| [Platform and windows](platform-and-windows/spec.md) | platform | Implemented |
| [First light: RHI v0, render graph v0, golden images](first-light/spec.md) | render | Implemented |
| [MCP server v0](mcp-server/spec.md) | runtime | Implemented |
| [DX12 backend](dx12-backend/spec.md) | rhi | Implemented (WARP in CI) |
| [Views and cameras](render-views/spec.md) | render | Implemented |
| [Transforms and hierarchy](transforms/spec.md) | ecs | Implemented |
| [Input actions in Luau](input/spec.md) | script | Implemented |
| [Hot reload](hot-reload/spec.md) | asset | Implemented |
| [First real scene](first-real-scene/spec.md) | render, asset | Implemented |
| [Record and replay](record-replay/spec.md) | runtime | Implemented |
| [Physical sky and atmosphere](physical-sky/spec.md) | render | Implemented |
| [Real-time renderer (interim visibility-buffer path)](realtime-renderer/spec.md) | render, asset | Implemented; its path replaced by the GPU-driven renderer |
| [GPU-driven real-time renderer: the core](gpu-driven-renderer/spec.md) | render, rhi | Implemented |
| [Photo mode: the reference path tracer](photo-mode/spec.md) | render | Implemented |
| [Runtime meshes](runtime-meshes/spec.md) | render | Implemented |
| [Job system](job-system/spec.md) | core | Implemented |
| [Procgen: stage runtime, field graphs, generator sandbox](procgen/spec.md) | procgen | Implemented |
| [Space backdrop](space-backdrop/spec.md) | render, space | Implemented |
| [Planet atmosphere: aerial perspective from orbit to a cave](planet-atmosphere/spec.md) | render, physics, space | Implemented |
| [Ambient light and occlusion in the real-time renderer](ambient-light/spec.md) | render | Implemented |
| [Reference frames and double positions](frames/spec.md) | ecs, render, script | Implemented |
| [Physics: Jolt behind veach::physics, one scene per frame](physics/spec.md) | physics, runtime | Implemented |
| [Voxel terrain: streaming, edits and collider meshes](voxel-terrain/spec.md) | voxel, runtime | Implemented |
| [The first slice: the realization layer and the slice app](first-slice/spec.md) | space, apps | Implemented |
| [GPU-driven RHI: indirect work and non-blocking readback](gpu-driven-rhi/spec.md) | rhi, render | Implemented |
| [RHI textures and raster state: texture kinds, updates, blending, depth bias, samplers](rhi-textures/spec.md) | rhi, render | Implemented |
| [Sun shadows in the real-time renderer](sun-shadows/spec.md) | render | Implemented |
| [Temporal anti-aliasing](temporal-aa/spec.md) | render | Implemented |

Specs of the raytracing-first era (the real-time renderer v0, temporal direct lighting, soft shadows, real-time GI)
were removed at the reset ([ADR 0001](../adr/0001-reset.md)); they live in the `pathtracer-era` tag.
