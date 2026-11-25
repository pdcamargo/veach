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
| [First light: RHI v0, render graph v0, golden images](first-light/spec.md) | render | Implemented (GPU CI pending, VCH-044) |
| [MCP server v0](mcp-server/spec.md) | runtime | Implemented |
| [DX12 backend](dx12-backend/spec.md) | rhi | Implemented (WARP in CI) |
| [Views and cameras](render-views/spec.md) | render | Implemented |
| [Transforms and hierarchy](transforms/spec.md) | ecs | Implemented |
| [Input actions in Luau](input/spec.md) | script | Implemented |
