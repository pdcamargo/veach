#include <veach/runtime/mcp_tools.hpp>

#include <veach/core/suggest.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace veach {

namespace {

// Keep in sync with McpServer::tools(); tests/mcp_tools_tests.cpp checks that tools/list starts with exactly these.
constexpr std::array<std::string_view, 13> builtin_names{
    "query_entities", "get_component", "set_component", "spawn_entity",      "step_frames", "send_input", "reload",
    "list_assets",    "screenshot",    "photo_mode",    "render_graph_plan", "gpu_timings", "eval",
};

bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.';
    });
}

}  // namespace

std::span<const std::string_view> builtin_mcp_tools() {
    return builtin_names;
}

Result<> McpTools::add(McpTool tool) {
    if (!valid_name(tool.name)) {
        return make_error(ErrorCode::mcp_invalid_tool, std::format("invalid MCP tool name '{}'", tool.name),
                          "Tool names are 1-64 characters of A-Z, a-z, 0-9, '_', '-' and '.'; Veach uses snake_case "
                          "(\"physics_raycast\").");
    }
    if (!tool.handler) {
        return make_error(ErrorCode::mcp_invalid_tool, std::format("MCP tool '{}' has no handler", tool.name),
                          "Set McpTool::handler to a function (World&, const json& arguments) -> "
                          "Result<McpToolOutput>.");
    }
    if (!tool.input_schema.is_object() || tool.input_schema.value("type", "") != "object") {
        return make_error(
            ErrorCode::mcp_invalid_tool,
            std::format("MCP tool '{}': the input schema must be a JSON Schema of type \"object\"", tool.name),
            "MCP passes arguments as an object: {\"type\": \"object\", \"properties\": {...}}.");
    }
    if (std::ranges::find(builtin_names, tool.name) != builtin_names.end()) {
        return make_error(
            ErrorCode::mcp_duplicate_tool,
            std::format("MCP tool '{}' is a built-in tool and can't be registered", tool.name),
            "Pick another name; prefix it with the module's area (\"physics_raycast\", \"terrain_query\").");
    }
    if (find(tool.name) != nullptr) {
        return make_error(ErrorCode::mcp_duplicate_tool, std::format("MCP tool '{}' is already registered", tool.name),
                          "Tool names are unique across modules: rename one, or register it from one module only "
                          "(two imports of a module register once: Flecs runs a module's constructor once).");
    }
    tools_.push_back(std::move(tool));
    return {};
}

bool McpTools::remove(std::string_view name) {
    return std::erase_if(tools_, [&](const McpTool& t) { return t.name == name; }) > 0;
}

const McpTool* McpTools::find(std::string_view name) const {
    const auto it = std::ranges::find(tools_, name, &McpTool::name);
    return it == tools_.end() ? nullptr : &*it;
}

nlohmann::json McpTools::list() const {
    nlohmann::json out = nlohmann::json::array();
    for (const McpTool& t : tools_) {
        out.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
    }
    return out;
}

Result<McpToolOutput> McpTools::call(std::string_view name, World& world, const nlohmann::json& arguments) const {
    const McpTool* tool = find(name);
    if (tool == nullptr) {
        std::vector<std::string> names(builtin_names.begin(), builtin_names.end());
        for (const McpTool& t : tools_) {
            names.push_back(t.name);
        }
        return make_error(ErrorCode::mcp_unknown_tool, std::format("unknown tool '{}'", name),
                          did_you_mean(name, names));
    }
    try {
        return tool->handler(world, arguments.is_null() ? nlohmann::json::object() : arguments);
    } catch (const nlohmann::json::exception& e) {
        return make_error(ErrorCode::mcp_invalid_arguments, std::format("{}: {}", name, e.what()),
                          "See the tool's inputSchema in tools/list.");
    }
}

}  // namespace veach
