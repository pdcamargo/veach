#pragma once

#include <veach/runtime/mcp_tools.hpp>

namespace veach {

/// Adds the engine's physics MCP tools, physics_scenes, physics_bodies and physics_contacts
/// (docs/specs/physics/spec.md). They read the PhysicsModule's world when the app imported it.
void register_physics_tools(McpTools& tools);

}  // namespace veach
