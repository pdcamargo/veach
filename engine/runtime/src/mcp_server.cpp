#include <veach/runtime/mcp_server.hpp>

#include <veach/core/log.hpp>
#include <veach/core/suggest.hpp>
#include <veach/ecs/command_buffer.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/json.hpp>
#include <veach/ecs/reflection.hpp>
#include <veach/render/png.hpp>
#include <veach/render/renderer.hpp>
#include <veach/rhi/rhi.hpp>
#include <veach/runtime/engine.hpp>
#include <veach/script/script_runtime.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace veach {

namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 4> protocol_versions{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

/// A tool error: reported to the client as a tool result with isError, never as a protocol error.
struct ToolError {
    Error error;
};

[[noreturn]] void fail(ErrorCode code, std::string message, std::string hint = {}) {
    throw ToolError{Error{code, std::move(message), std::move(hint)}};
}

std::string base64(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t chunk = (std::uint32_t{bytes[i]} << 16) |
                                    (i + 1 < bytes.size() ? std::uint32_t{bytes[i + 1]} << 8 : 0) |
                                    (i + 2 < bytes.size() ? std::uint32_t{bytes[i + 2]} : 0);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return out;
}

json text_result(std::string text) {
    return {{"content", json::array({{{"type", "text"}, {"text", std::move(text)}}})}};
}

json json_result(const json& value) {
    return text_result(value.dump(2));
}

/// Reflected data components, by name. Short names (Window) and full paths (veach::platform::Window) both work.
class ComponentIndex {
public:
    explicit ComponentIndex(const World& world) {
        world.each([&](Entity e, const flecs::Component& c) {
            const std::string path = e.path("::", "").c_str();
            if (c.size == 0 || path.starts_with("flecs::") || e.has(flecs::Module) ||
                value_kind(world, e.id()) != ValueKind::structure) {
                return;
            }
            const std::string name = e.name().c_str();
            by_name_.emplace(path, e.id());
            if (std::ranges::find(short_names_, name) == short_names_.end()) {
                by_name_.emplace(name, e.id());  // first one wins if two modules share a short name
                short_names_.push_back(name);
            }
            names_[e.id()] = name;
        });
        std::ranges::sort(short_names_);
    }

    [[nodiscard]] ecs::entity_t resolve(const std::string& name) const {
        if (const auto it = by_name_.find(name); it != by_name_.end()) {
            return it->second;
        }
        fail(ErrorCode::ecs_unknown_component, std::format("unknown component '{}'", name),
             did_you_mean(name, short_names_));
    }

    [[nodiscard]] const std::vector<std::string>& names() const { return short_names_; }
    [[nodiscard]] const std::map<ecs::entity_t, std::string>& by_id() const { return names_; }

private:
    std::map<std::string, ecs::entity_t> by_name_;
    std::map<ecs::entity_t, std::string> names_;
    std::vector<std::string> short_names_;
};

const json& require(const json& arguments, std::string_view name) {
    if (!arguments.is_object() || !arguments.contains(name)) {
        fail(ErrorCode::mcp_invalid_arguments, std::format("missing argument '{}'", name),
             "See the tool's inputSchema in tools/list.");
    }
    return arguments.at(std::string(name));
}

std::string require_string(const json& arguments, std::string_view name) {
    const json& value = require(arguments, name);
    if (!value.is_string()) {
        fail(ErrorCode::mcp_invalid_arguments, std::format("argument '{}' must be a string", name));
    }
    return value.get<std::string>();
}

Entity resolve_entity(const World& world, const json& ref) {
    if (ref.is_number_unsigned() || ref.is_number_integer()) {
        const auto id = ref.get<std::uint64_t>();
        if (id != 0 && ecs_is_alive(world.c_ptr(), id)) {
            return world.entity(id);
        }
        fail(ErrorCode::ecs_unknown_entity, std::format("no live entity with id {}", id),
             "Ids come from query_entities or spawn_entity; the entity may have been despawned.");
    }
    if (ref.is_string()) {
        const std::string path = ref.get<std::string>();
        if (const Entity e = world.lookup(path.c_str())) {
            return e;
        }
        fail(ErrorCode::ecs_unknown_entity, std::format("no entity named '{}'", path),
             "Use the entity's path (e.g. \"primary_window\"), or its id from query_entities.");
    }
    fail(ErrorCode::mcp_invalid_arguments, "'entity' must be an entity id (number) or path (string)");
}

std::vector<std::string> components_of(const World& world, Entity e, const ComponentIndex& index) {
    std::vector<std::string> names;
    ecs_type_t const* type = ecs_get_type(world.c_ptr(), e.id());
    if (!type) {
        return names;
    }
    for (std::int32_t i = 0; i < type->count; ++i) {
        if (const auto it = index.by_id().find(type->array[i]); it != index.by_id().end()) {
            names.push_back(it->second);
        }
    }
    return names;
}

/// Reads a JSON value into a temporary component value, starting from `start` (or defaults).
struct TempValue {
    TempValue(const World& w, ecs::entity_t t) : world(w), type(t), ptr(ecs_ptr_new(w.c_ptr(), t)) {}
    ~TempValue() { ecs_ptr_free(world.c_ptr(), type, ptr); }
    TempValue(const TempValue&) = delete;
    TempValue& operator=(const TempValue&) = delete;
    TempValue(TempValue&&) = delete;
    TempValue& operator=(TempValue&&) = delete;
    const World& world;
    ecs::entity_t type;
    void* ptr;
};

json entity_schema() {
    return {{"description",
             "Entity id (number, from query_entities/spawn_entity) or path (string, e.g. \"primary_window\")"},
            {"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})}};
}

}  // namespace

json McpServer::tools() const {
    World& world = engine_.world();
    const ComponentIndex index(world);
    const json component_names = index.names();
    const json component = {{"type", "string"}, {"enum", component_names}, {"description", "Component name"}};

    // set_component's value schema depends on the component: one if/then branch per component.
    json value_branches = json::array();
    json spawn_properties = json::object();
    for (const std::string& name : index.names()) {
        const ecs::entity_t id = index.resolve(name);
        json schema = json_schema(world, id);
        schema.erase("additionalProperties");  // set_component merges: any subset of fields is fine
        value_branches.push_back({{"if", {{"properties", {{"component", {{"const", name}}}}}}},
                                  {"then", {{"properties", {{"value", schema}}}}}});
        spawn_properties[name] = schema;
    }

    const auto tool = [](std::string name, std::string description, json schema) {
        return json{
            {"name", std::move(name)}, {"description", std::move(description)}, {"inputSchema", std::move(schema)}};
    };
    json list = json::array();
    list.push_back(tool("query_entities",
                        "List entities, optionally only those having all the given components and/or whose name "
                        "contains a string. Returns id, name and reflected components of each.",
                        {{"type", "object"},
                         {"properties",
                          {{"with", {{"type", "array"}, {"items", component}}},
                           {"name_contains", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"default", 100}}}}}}));
    list.push_back(tool("get_component", "Read one component of an entity as JSON.",
                        {{"type", "object"},
                         {"properties", {{"entity", entity_schema()}, {"component", component}}},
                         {"required", {"entity", "component"}}}));
    list.push_back(
        tool("set_component",
             "Write a component (fields you leave out keep their current or default value). Applied "
             "through a command buffer: all or nothing.",
             {{"type", "object"},
              {"properties", {{"entity", entity_schema()}, {"component", component}, {"value", {{"type", "object"}}}}},
              {"required", {"entity", "component", "value"}},
              {"allOf", value_branches}}));
    list.push_back(tool(
        "spawn_entity", "Spawn an entity with an optional name and components (all or nothing).",
        {{"type", "object"},
         {"properties",
          {{"name", {{"type", "string"}}},
           {"components", {{"type", "object"}, {"properties", spawn_properties}, {"additionalProperties", false}}}}}}));
    list.push_back(tool(
        "step_frames",
        "Run N frames (simulation, extraction, rendering). Headless sessions only advance through "
        "this tool, one fixed 1/60 s step per frame, so they are deterministic.",
        {{"type", "object"},
         {"properties", {{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}, {"default", 1}}}}}}));
    list.push_back(tool("screenshot",
                        "Render the current state (without advancing simulation) and return it as a PNG. Needs a "
                        "GPU backend (--rhi metal). Optionally also write it to 'path'.",
                        {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}}));
    list.push_back(tool("render_graph_plan",
                        "The last compiled render graph as text: pass order, culled passes and barriers.",
                        {{"type", "object"}, {"properties", json::object()}}));
    list.push_back(tool("gpu_timings", "GPU backend, device, frames in flight, and the last frame's GPU time.",
                        {{"type", "object"}, {"properties", json::object()}}));
    list.push_back(
        tool("eval",
             "Run Luau in the live world, sandboxed and transactional like any script invocation "
             "(require(\"@veach\"); writes apply only if it returns without error). Returns its "
             "return values and the log lines it produced.",
             {{"type", "object"}, {"properties", {{"source", {{"type", "string"}}}}}, {"required", {"source"}}}));
    return list;
}

json McpServer::call_tool(std::string_view name, const json& arguments) {
    World& world = engine_.world();

    if (name == "query_entities") {
        const ComponentIndex index(world);
        std::vector<ecs::entity_t> with;
        if (arguments.contains("with")) {
            for (const json& n : arguments.at("with")) {
                with.push_back(index.resolve(n.get<std::string>()));
            }
        }
        const std::string contains = arguments.value("name_contains", "");
        const std::size_t limit = arguments.value("limit", std::size_t{100});

        std::set<ecs::entity_t> found;
        const auto collect = [&](const std::vector<ecs::entity_t>& terms) {
            auto builder = world.query_builder<>();
            for (const ecs::entity_t id : terms) {
                builder.with(id);
            }
            // run(), not each(): a term on a Singleton-trait component (Time, Input) is
            // sourced from the component itself, so the iterator has no $this entities.
            builder.build().run([&](flecs::iter& it) {
                while (it.next()) {
                    const ecs_iter_t* raw = it.c_ptr();
                    for (int32_t i = 0; raw->entities != nullptr && i < raw->count; ++i) {
                        found.insert(raw->entities[i]);
                    }
                }
            });
        };
        if (!with.empty()) {
            collect(with);
        } else {
            for (const auto& [id, component_name] : index.by_id()) {
                collect({id});
            }
        }
        json out = json::array();
        for (const ecs::entity_t id : found) {
            const Entity e = world.entity(id);
            if (e.has<flecs::Component>() || e.has(flecs::Module) || e.has(flecs::System)) {
                continue;
            }
            const std::string path = e.name().length() ? std::string(e.path("::", "").c_str()) : std::string();
            if (!contains.empty() && path.find(contains) == std::string::npos) {
                continue;
            }
            out.push_back({{"id", id}, {"name", path}, {"components", components_of(world, e, index)}});
            if (out.size() >= limit) {
                break;
            }
        }
        return json_result(out);
    }

    if (name == "get_component") {
        const ComponentIndex index(world);
        const Entity e = resolve_entity(world, require(arguments, "entity"));
        const std::string component_name = require_string(arguments, "component");
        const ecs::entity_t component = index.resolve(component_name);
        const void* ptr = ecs_get_id(world.c_ptr(), e.id(), component);
        if (!ptr) {
            std::string has;
            for (const std::string& c : components_of(world, e, index)) {
                has += has.empty() ? c : ", " + c;
            }
            fail(ErrorCode::mcp_invalid_arguments, std::format("entity {} has no '{}'", e.id(), component_name),
                 std::format("It has: {}.", has.empty() ? "no reflected components" : has));
        }
        return json_result(to_json(world, component, ptr));
    }

    if (name == "set_component" || name == "spawn_entity") {
        const ComponentIndex index(world);
        struct Write {
            ecs::entity_t component;
            std::unique_ptr<TempValue> value;
        };
        std::vector<Write> writes;
        Entity target;
        const auto prepare = [&](const std::string& component_name, const json& value, const void* start) {
            const ecs::entity_t component = index.resolve(component_name);
            auto temp = std::make_unique<TempValue>(world, component);
            if (start) {
                ecs_ptr_copy(world.c_ptr(), component, temp->ptr, start);
            }
            if (!value.is_object()) {
                fail(ErrorCode::mcp_invalid_arguments,
                     std::format("the value for '{}' must be an object", component_name));
            }
            if (auto r = from_json(world, component, temp->ptr, value, component_name); !r) {
                throw ToolError{r.error()};
            }
            writes.push_back({component, std::move(temp)});
        };
        if (name == "set_component") {
            target = resolve_entity(world, require(arguments, "entity"));
            const std::string component_name = require_string(arguments, "component");
            const ecs::entity_t component = index.resolve(component_name);
            prepare(component_name, require(arguments, "value"), ecs_get_id(world.c_ptr(), target.id(), component));
        } else if (arguments.contains("components")) {
            for (const auto& [component_name, value] : arguments.at("components").items()) {
                prepare(component_name, value, nullptr);
            }
        }
        // Everything validated: now apply as one transaction.
        CommandBuffer cmd(world);
        if (name == "spawn_entity") {
            target = cmd.spawn(arguments.value("name", ""));
        }
        for (const Write& w : writes) {
            cmd.set(target, w.component, w.value->ptr);
        }
        cmd.apply();
        if (name == "spawn_entity") {
            return json_result({{"id", target.id()},
                                {"name", arguments.value("name", "")},
                                {"components", components_of(world, target, index)}});
        }
        return json_result(
            to_json(world, writes.front().component, ecs_get_id(world.c_ptr(), target.id(), writes.front().component)));
    }

    if (name == "step_frames") {
        const auto count = arguments.value("count", std::uint64_t{1});
        if (count < 1 || count > 100000) {
            fail(ErrorCode::mcp_invalid_arguments, "'count' must be between 1 and 100000");
        }
        const bool running = engine_.step(count);
        const Time& time = world.get<Time>();
        return json_result(
            {{"frame", time.frame}, {"fixed_step", time.fixed_step}, {"elapsed", time.elapsed}, {"running", running}});
    }

    if (name == "screenshot") {
        auto& renderer = engine_.services().get<render::Renderer>();
        std::optional<Result<render::Image>> captured;
        renderer.request_capture([&](Result<render::Image> image) { captured = std::move(image); });
        engine_.render_frame();
        if (!captured) {
            renderer.request_capture({});
            fail(ErrorCode::render_screenshot_failed, "the frame did not render",
                 "Check the log (stderr) for render errors.");
        }
        if (!*captured) {
            throw ToolError{captured->error()};
        }
        const render::Image& image = **captured;
        std::string note =
            std::format("{}x{} scene color, frame {}", image.width, image.height, world.get<Time>().frame);
        if (arguments.contains("path")) {
            const std::string path = require_string(arguments, "path");
            if (auto written = render::write_png(path, image.width, image.height, image.rgba); !written) {
                throw ToolError{written.error()};
            }
            note += std::format(", written to {}", path);
        }
        const std::vector<std::uint8_t> png = render::encode_png(image);
        return {{"content", json::array({{{"type", "image"}, {"data", base64(png)}, {"mimeType", "image/png"}},
                                         {{"type", "text"}, {"text", note}}})}};
    }

    if (name == "render_graph_plan") {
        auto& renderer = engine_.services().get<render::Renderer>();
        if (renderer.last_graph().empty()) {
            engine_.render_frame();
        }
        return text_result(renderer.last_graph());
    }

    if (name == "gpu_timings") {
        auto& device = engine_.services().get<rhi::Device>();
        return json_result({{"backend", rhi::to_string(device.backend())},
                            {"device", device.capabilities().device_name},
                            {"gpu", device.capabilities().gpu},
                            {"frames_in_flight", device.frames_in_flight()},
                            {"last_frame_gpu_ms", device.last_frame_gpu_ms()}});
    }

    if (name == "eval") {
        const std::string source = require_string(arguments, "source");
        std::vector<std::string> lines;
        const log::SinkId sink = log::add_sink([&](const log::Record& r) {
            if (r.category == "script") {
                lines.push_back(std::format("[{}] {}", log::to_string(r.level), r.message));
            }
        });
        auto result = engine_.services().get<ScriptRuntime>().evaluate(source, "mcp/eval");
        log::remove_sink(sink);
        json out = {{"ok", result.has_value()}, {"log", lines}};
        if (!result) {
            out["error"] = result.error().to_string();
            json r = json_result(out);
            r["isError"] = true;
            return r;
        }
        out["returns"] = *result;
        return json_result(out);
    }

    std::vector<std::string> names;
    for (const json& t : tools()) {
        names.push_back(t.at("name").get<std::string>());
    }
    fail(ErrorCode::mcp_unknown_tool, std::format("unknown tool '{}'", name), did_you_mean(name, names));
}

json McpServer::handle(const json& message) {
    const bool is_request = message.contains("id");
    const json id = is_request ? message.at("id") : json(nullptr);
    const auto respond = [&](json result) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    };
    const auto protocol_error = [&](int code, std::string text) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::move(text)}}}};
    };

    const std::string method = message.value("method", "");
    const json params = message.value("params", json::object());
    if (!is_request) {
        return nullptr;  // notifications (initialized, cancelled, ...) need no answer
    }
    if (method == "initialize") {
        std::string version(protocol_versions.front());
        const std::string requested = params.value("protocolVersion", "");
        if (std::ranges::find(protocol_versions, requested) != protocol_versions.end()) {
            version = requested;
        }
        return respond({{"protocolVersion", version},
                        {"capabilities", {{"tools", {{"listChanged", false}}}}},
                        {"serverInfo", {{"name", "veach"}, {"version", "0.1.0"}}},
                        {"instructions",
                         "Veach engine. Inspect and change the live ECS world (query_entities, get/set_component, "
                         "spawn_entity), advance it (step_frames), look at it (screenshot, render_graph_plan, "
                         "gpu_timings) and run Luau in it (eval). Errors carry [codes] documented in "
                         "docs/reference/errors.md."}});
    }
    if (method == "ping") {
        return respond(json::object());
    }
    if (method == "tools/list") {
        return respond({{"tools", tools()}});
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        try {
            return respond(call_tool(name, params.value("arguments", json::object())));
        } catch (const ToolError& e) {
            json result = text_result(e.error.to_string());
            result["isError"] = true;
            return respond(result);
        } catch (const json::exception& e) {
            json result = text_result(
                Error{ErrorCode::mcp_invalid_arguments, e.what(), "See the tool's inputSchema in tools/list."}
                    .to_string());
            result["isError"] = true;
            return respond(result);
        }
    }
    return protocol_error(-32601, std::format("method '{}' not found", method));
}

int McpServer::run(std::istream& in, std::ostream& out) {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> lines;
    bool eof = false;
    // stdin is read on its own thread; the world is only touched on this one.
    std::thread reader([&] {
        std::string line;
        while (std::getline(in, line)) {
            const std::scoped_lock lock(mutex);
            lines.push_back(std::move(line));
            ready.notify_one();
        }
        const std::scoped_lock lock(mutex);
        eof = true;
        ready.notify_one();
    });
    reader.detach();  // it may be blocked in getline when the window closes

    const bool headless = engine_.config().project.headless;
    log::info("mcp", "serving MCP on stdio ({}; frames advance {})", headless ? "headless" : "windowed",
              headless ? "only through step_frames" : "continuously");
    while (true) {
        std::optional<std::string> line;
        {
            std::unique_lock lock(mutex);
            if (headless) {
                ready.wait(lock, [&] { return !lines.empty() || eof; });
            }
            if (!lines.empty()) {
                line = std::move(lines.front());
                lines.pop_front();
            } else if (eof) {
                break;
            }
        }
        if (!line) {
            if (!engine_.frame()) {
                break;  // the window closed or the app quit
            }
            continue;
        }
        if (line->empty()) {
            continue;
        }
        json response;
        const json message = json::parse(*line, nullptr, false);
        if (message.is_discarded()) {
            response = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}};
        } else {
            response = handle(message);
        }
        if (!response.is_null()) {
            out << response.dump() << '\n' << std::flush;
        }
    }
    log::info("mcp", "client disconnected");
    return 0;
}

}  // namespace veach
