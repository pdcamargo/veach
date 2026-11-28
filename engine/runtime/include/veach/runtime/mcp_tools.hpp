#pragma once

// MCP tools registered by modules (docs/specs/mcp-server/spec.md, VCH-100). The engine provides one `McpTools`
// registry per world as a service, before any module is imported, so a module (engine, toolbox or game) adds its
// own tools next to the built-in ones:
//
//   auto added = veach::services(world).get<veach::McpTools>().add({
//       .name = "health_of",
//       .description = "The health of every entity with a Health component.",
//       .input_schema = {{"type", "object"}, {"properties", {{"limit", {{"type", "integer"}}}}}},
//       .handler = [](veach::World& w, const nlohmann::json& args) -> veach::Result<veach::McpToolOutput> {
//           return nlohmann::json{{"count", 3}};   // sent as text content, pretty-printed JSON
//       },
//   });
//   if (!added) veach::log::error("game", added.error());   // a duplicate name, with a hint
//
// `veach-lab --mcp` lists module tools in tools/list after the built-in ones, and runs their handlers on the main
// thread between frames, like the built-in tools. A handler's Error becomes an MCP tool result with isError and the
// error's [code]. Module tools are not recorded by --record: keep them read-only, or document that they aren't.

#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace veach {

/// MCP content blocks, sent as the tool result's `content` as they are: `[{"type": "image", ...}, ...]`.
struct McpContent {
    nlohmann::json blocks = nlohmann::json::array();
};

/// What a handler returns: a JSON value (sent as one text block holding the value as indented JSON, like the
/// built-in tools' results) or content blocks.
using McpToolOutput = std::variant<nlohmann::json, McpContent>;

using McpToolHandler = std::function<Result<McpToolOutput>(World& world, const nlohmann::json& arguments)>;

struct McpTool {
    std::string name;  ///< unique, 1-64 of [A-Za-z0-9_.-]; snake_case like the built-in tools
    std::string description;
    nlohmann::json input_schema = {{"type", "object"}};  ///< JSON Schema of `arguments`: an object schema
    McpToolHandler handler;
};

/// The built-in tools of McpServer (mcp_server.cpp). A module can't register these names.
[[nodiscard]] std::span<const std::string_view> builtin_mcp_tools();

/// The registry of module tools. A service: `veach::services(world).get<veach::McpTools>()`.
class McpTools {
public:
    /// Adds a tool. Fails with mcp.invalid_tool (bad name, schema not an object schema, no handler) or
    /// mcp.duplicate_tool (a built-in or already registered name); nothing is added then.
    [[nodiscard]] Result<> add(McpTool tool);
    /// Removes a tool; returns whether it was there.
    bool remove(std::string_view name);

    [[nodiscard]] const McpTool* find(std::string_view name) const;
    /// In registration order.
    [[nodiscard]] std::span<const McpTool> tools() const { return tools_; }
    /// tools/list entries: `[{name, description, inputSchema}]`, in registration order.
    [[nodiscard]] nlohmann::json list() const;

    /// Runs a tool's handler. mcp.unknown_tool (with a did-you-mean over every tool, built-in included) if `name`
    /// is not registered. A handler that throws nlohmann::json::exception fails with mcp.invalid_arguments.
    [[nodiscard]] Result<McpToolOutput> call(std::string_view name, World& world,
                                             const nlohmann::json& arguments) const;

    void clear() { tools_.clear(); }

private:
    std::vector<McpTool> tools_;
};

}  // namespace veach
