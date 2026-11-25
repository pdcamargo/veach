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
| [Real-time renderer v0](realtime-renderer-v0/spec.md) | render, asset, rhi | Implemented |
| [Record and replay](record-replay/spec.md) | runtime | Implemented |
| [Temporal foundation and direct lighting at speed](temporal-direct-lighting/spec.md) | render, rhi | Implemented |
| [Soft shadows from lights with a size](soft-shadows/spec.md) | render | Implemented |
| [Physical sky and atmosphere](physical-sky/spec.md) | render | Implemented |
