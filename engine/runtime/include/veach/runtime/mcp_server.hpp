#pragma once

// The MCP server (docs/specs/mcp-server/spec.md). Any Veach app started with
// --mcp speaks the Model Context Protocol over stdio (JSON-RPC 2.0, one
// message per line). Logs go to stderr. Headless, frames advance only through
// `step_frames`, so an agent's session is deterministic. Windowed, the app
// keeps rendering between requests.
//
// Tools: query_entities, get_component, set_component, spawn_entity,
// step_frames, screenshot, render_graph_plan, gpu_timings, eval. Their input
// schemas are generated from reflection (component names, component value
// schemas). Errors carry Veach error codes and did-you-mean hints.

#include <nlohmann/json_fwd.hpp>

#include <iosfwd>
#include <string>
#include <string_view>

namespace veach {

class Engine;

class McpServer {
public:
    explicit McpServer(Engine& engine) : engine_(engine) {}

    /// Serves requests from `in` until EOF (or the app quits). Returns the process exit code.
    int run(std::istream& in, std::ostream& out);

    /// Handles one JSON-RPC message. Returns the response, or null for notifications.
    [[nodiscard]] nlohmann::json handle(const nlohmann::json& message);

private:
    [[nodiscard]] nlohmann::json tools() const;
    [[nodiscard]] nlohmann::json call_tool(std::string_view name, const nlohmann::json& arguments);

    Engine& engine_;
};

}  // namespace veach
