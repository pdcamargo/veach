#pragma once

#include <veach/runtime/mcp_tools.hpp>

namespace veach {

/// Adds the engine's `frames` MCP tool (docs/specs/frames/spec.md).
void register_frames_tool(McpTools& tools);

}  // namespace veach
