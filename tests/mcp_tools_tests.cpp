// MCP tools registered by modules (docs/specs/mcp-server/spec.md, VCH-100): the McpTools registry, its use through
// McpServer, and the space toolbox's orrery_state and generate_system.

#include "support/test_support.hpp"

#include <veach/runtime/mcp_server.hpp>
#include <veach/runtime/mcp_tools.hpp>
#include <veach/space/gen/system.hpp>
#include <veach/space/mcp_tools.hpp>
#include <veach/space/orrery.hpp>
#include <veach/space/presets.hpp>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace veach;
using nlohmann::json;

namespace {

McpTool echo_tool(std::string name = "echo") {
    return {.name = std::move(name),
            .description = "Returns its arguments.",
            .input_schema = {{"type", "object"}},
            .handler = [](World&, const json& args) -> Result<McpToolOutput> { return McpToolOutput{args}; }};
}

/// A module that registers two tools: one reads the world, one returns image-like content blocks.
struct ProbeToolsModule {
    explicit ProbeToolsModule(World& world) {
        world.module<ProbeToolsModule>();
        auto& tools = services(world).get<McpTools>();
        auto added = tools.add({.name = "probe_frame",
                                .description = "The current frame, or an error if 'fail' is set.",
                                .input_schema = {{"type", "object"}, {"properties", {{"fail", {{"type", "boolean"}}}}}},
                                .handler = [](World& w, const json& args) -> Result<McpToolOutput> {
                                    if (args.value("fail", false)) {
                                        return make_error(ErrorCode::mcp_invalid_arguments, "asked to fail",
                                                          "Leave 'fail' out.");
                                    }
                                    return McpToolOutput{json{{"frame", w.get<Time>().frame}}};
                                }});
        VEACH_ASSERT(added.has_value(), "probe_frame: {}", added.error());
        added = tools.add({.name = "probe_content",
                           .description = "Content blocks as they are.",
                           .handler = [](World&, const json&) -> Result<McpToolOutput> {
                               return McpContent{json::array({{{"type", "text"}, {"text", "raw"}}})};
                           }});
        VEACH_ASSERT(added.has_value(), "probe_content: {}", added.error());
        // A second registration of the same name fails, and the module can tell.
        const auto again = tools.add(
            {.name = "probe_frame", .handler = [](World&, const json&) -> Result<McpToolOutput> { return {}; }});
        VEACH_ASSERT(!again.has_value() && again.error().code == ErrorCode::mcp_duplicate_tool, "duplicate accepted");
    }
};

json call(McpServer& server, const std::string& tool, json arguments = json::object()) {
    return server.handle({{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "tools/call"},
                          {"params", {{"name", tool}, {"arguments", arguments}}}});
}

json call_json(McpServer& server, const std::string& tool, json arguments = json::object()) {
    const json r = call(server, tool, std::move(arguments));
    REQUIRE_FALSE(r["result"].value("isError", false));
    return json::parse(r["result"]["content"][0]["text"].get<std::string>());
}

/// A world with services, as an Engine has, but without the engine: for the space tools.
struct ToolWorld {
    ToolWorld() {
        services.provide<McpTools>(tools);
        attach_services(world, &services);
        world.import<CoreModule>();
        world.import<veach::space::SpaceMcpModule>();
    }
    ~ToolWorld() { attach_services(world, nullptr); }
    ToolWorld(const ToolWorld&) = delete;
    ToolWorld& operator=(const ToolWorld&) = delete;
    ToolWorld(ToolWorld&&) = delete;
    ToolWorld& operator=(ToolWorld&&) = delete;

    json call(std::string_view name, const json& args) {
        auto out = tools.call(name, world, args);
        REQUIRE_MESSAGE(out.has_value(), (out ? std::string() : out.error().to_string()));
        REQUIRE(std::holds_alternative<json>(*out));
        return std::get<json>(*out);
    }
    Error fail(std::string_view name, const json& args) {
        auto out = tools.call(name, world, args);
        REQUIRE_FALSE(out.has_value());
        return out.error();
    }

    McpTools tools;
    Services services;
    World world;
};

}  // namespace

TEST_CASE("[mcp.AC11] McpTools: names are unique, validated, and errors say how to fix them") {
    McpTools tools;
    REQUIRE(tools.add(echo_tool()).has_value());
    CHECK(tools.find("echo") != nullptr);
    CHECK(tools.tools().size() == 1);

    const auto duplicate = tools.add(echo_tool());
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == ErrorCode::mcp_duplicate_tool);
    CHECK_FALSE(duplicate.error().hint.empty());
    CHECK(tools.tools().size() == 1);

    for (const std::string_view builtin : builtin_mcp_tools()) {
        const auto taken = tools.add(echo_tool(std::string(builtin)));
        REQUIRE_FALSE(taken.has_value());
        CHECK(taken.error().code == ErrorCode::mcp_duplicate_tool);
        CHECK(taken.error().message.find("built-in") != std::string::npos);
    }

    CHECK(tools.add(echo_tool("")).error().code == ErrorCode::mcp_invalid_tool);
    CHECK(tools.add(echo_tool("has space")).error().code == ErrorCode::mcp_invalid_tool);
    CHECK(tools.add(echo_tool(std::string(65, 'a'))).error().code == ErrorCode::mcp_invalid_tool);
    McpTool no_handler = echo_tool("no_handler");
    no_handler.handler = nullptr;
    CHECK(tools.add(no_handler).error().code == ErrorCode::mcp_invalid_tool);
    McpTool bad_schema = echo_tool("bad_schema");
    bad_schema.input_schema = {{"type", "string"}};
    CHECK(tools.add(bad_schema).error().code == ErrorCode::mcp_invalid_tool);
    CHECK(tools.tools().size() == 1);

    const json list = tools.list();
    REQUIRE(list.size() == 1);
    CHECK(list[0]["name"] == "echo");
    CHECK(list[0]["description"] == "Returns its arguments.");
    CHECK(list[0]["inputSchema"] == json{{"type", "object"}});

    World world;
    auto echoed = tools.call("echo", world, {{"x", 1}});
    REQUIRE(echoed.has_value());
    CHECK(std::get<json>(*echoed) == json{{"x", 1}});

    const auto unknown = tools.call("ecoh", world, json::object());
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::mcp_unknown_tool);
    CHECK(unknown.error().hint == "Did you mean 'echo'?");

    REQUIRE(tools
                .add({.name = "strict",
                      .handler = [](World&, const json& args) -> Result<McpToolOutput> {
                          return McpToolOutput{json{{"n", args.at("n").get<int>()}}};  // throws if n is missing
                      }})
                .has_value());
    const auto thrown = tools.call("strict", world, json::object());
    REQUIRE_FALSE(thrown.has_value());
    CHECK(thrown.error().code == ErrorCode::mcp_invalid_arguments);

    CHECK(tools.remove("strict"));
    CHECK_FALSE(tools.remove("strict"));
}

TEST_CASE("[mcp.AC12] a module's tools are listed after the built-in ones and run on the live world") {
    test::LogCapture logs;
    test::TempProject project("mcp-tools");
    Engine engine(test::headless_config(project.path(), {}, 100));
    engine.import<ProbeToolsModule>();
    REQUIRE(engine.init().has_value());
    REQUIRE(engine.start().has_value());
    McpServer server(engine);

    const json init = server.handle({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    CHECK(init["result"]["instructions"].get<std::string>().find("probe_frame, probe_content") != std::string::npos);

    const json list = server.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})["result"]["tools"];
    std::vector<std::string> names;
    for (const json& t : list) {
        names.push_back(t["name"].get<std::string>());
    }
    std::vector<std::string> expected(builtin_mcp_tools().begin(), builtin_mcp_tools().end());
    expected.emplace_back("frames");  // the engine's own module tools (docs/specs/frames/spec.md, physics/spec.md)
    expected.emplace_back("physics_scenes");
    expected.emplace_back("physics_bodies");
    expected.emplace_back("physics_contacts");
    expected.emplace_back("probe_frame");
    expected.emplace_back("probe_content");
    CHECK(names == expected);  // also keeps builtin_mcp_tools() in sync with McpServer::tools()
    CHECK(list[17]["inputSchema"]["properties"].contains("fail"));

    CHECK(call_json(server, "probe_frame")["frame"] == 0);
    call_json(server, "step_frames", {{"count", 2}});
    CHECK(call_json(server, "probe_frame")["frame"] == 2);

    const json failed = call(server, "probe_frame", {{"fail", true}})["result"];
    CHECK(failed["isError"] == true);
    CHECK(failed["content"][0]["text"].get<std::string>().find("[mcp.invalid_arguments]") != std::string::npos);

    CHECK(call(server, "probe_content")["result"]["content"] == json::array({{{"type", "text"}, {"text", "raw"}}}));

    const json typo = call(server, "probe_fram")["result"];
    CHECK(typo["isError"] == true);
    CHECK(typo["content"][0]["text"].get<std::string>().find("Did you mean 'probe_frame'?") != std::string::npos);
    engine.shutdown();
    CHECK(logs.errors().empty());
}

TEST_CASE("[mcp.AC13] orrery_state answers where the bodies are at a tick in one call") {
    ToolWorld w;
    constexpr double au = 149'597'870'700.0;
    // SP-04's checks (docs/research/spikes/sp-04-orrery-clock.md): at J2000.0 the Earth is 0.98332 AU from the Sun
    // and the sun stands 18.1 degrees over Paris at 12:06 local solar time.
    const json earth = w.call("orrery_state", {{"system", "sol"},
                                               {"tick", std::to_string(veach::space::sol_j2000_tick)},
                                               {"body", "earth"},
                                               {"latitude", 48.8566},
                                               {"longitude", 2.3522}});
    CHECK(earth["tick"] == "378683424000");
    CHECK(earth["source"] == "preset");
    REQUIRE(earth["bodies"].size() == 1);
    CHECK(earth["bodies"][0]["parent"] == "emb");
    CHECK(earth["bodies"][0]["distance_to_star"].get<double>() / au == doctest::Approx(0.98332).epsilon(1e-5));
    CHECK(earth["sun"]["elevation"].get<double>() == doctest::Approx(18.09).epsilon(1e-3));
    CHECK(earth["sun"]["local_solar_time"].get<double>() == doctest::Approx(12.10).epsilon(1e-3));

    // Earth-like: at tick 0 the June solstice, the sun on the prime meridian, so noon there and 23.4° subsolar.
    const json earthlike = w.call("orrery_state", {{"system", "earthlike"}});
    REQUIRE(earthlike["bodies"].size() == 2);
    const json& planet = earthlike["bodies"][1];
    CHECK(planet["name"] == "planet");
    CHECK(planet["orbit"]["phase"] == 0.0);
    CHECK(planet["spin"]["local_solar_time_at_prime_meridian"].get<double>() == doctest::Approx(12.0));
    CHECK(planet["spin"]["subsolar_point"]["latitude"].get<double>() == doctest::Approx(23.4));
    // A quarter of a year later the orbit phase is a quarter.
    const auto quarter = static_cast<std::int64_t>(planet["orbit"]["period_s"].get<double>() * 60.0 / 4.0);
    const json later = w.call("orrery_state", {{"system", "earthlike"}, {"tick", std::to_string(quarter)}});
    CHECK(later["bodies"][1]["orbit"]["phase"].get<double>() == doctest::Approx(0.25).epsilon(1e-6));

    // Defaults follow the world's orrery and its clock.
    w.world.entity("orrery")
        .set<veach::space::Orrery>({.system = "earthlike", .scale = 0.1})
        .set<veach::space::UniverseClock>({.tick = 1234, .rate = 0});
    const json live = w.call("orrery_state", json::object());
    CHECK(live["system"] == "earthlike");
    CHECK(live["scale"] == 0.1);
    CHECK(live["tick"] == "1234");
    CHECK(live["source"] == "the orrery 'orrery'");

    const Error body = w.fail("orrery_state", {{"system", "sol"}, {"body", "eart"}});
    CHECK(body.code == ErrorCode::mcp_invalid_arguments);
    CHECK(body.hint == "Did you mean 'earth'?");
    CHECK(w.fail("orrery_state", {{"system", "sool"}}).hint == "Did you mean 'sol'?");
    CHECK(w.fail("orrery_state", {{"tick", "-5"}}).code == ErrorCode::mcp_invalid_arguments);
    CHECK(w.fail("orrery_state", {{"tick", "9007199254740992"}}).code == ErrorCode::mcp_invalid_arguments);
}

TEST_CASE("[mcp.AC14] generate_system returns SP-05's records with their stage hashes") {
    ToolWorld w;
    const json r = w.call("generate_system", {{"seed", "42"}, {"address", "S:3f2a91c07e5b0000"}});
    CHECK(r["address"] == "S:3f2a91c07e5b0000");
    CHECK(r["seed"] == "42");
    CHECK(r["coords"]["system"].is_string());

    // The same records as calling the stages directly, and committed so Windows CI checks them too.
    const ::space::gen::UniverseRecord universe{.seed = 42, .habitability_bias = 0.5};
    const ::space::gen::StageContext ctx{.universe = &universe,
                                         .address = *::space::gen::parse_system_address("S:3f2a91c07e5b0000")};
    const ::space::gen::SystemRecord record = ::space::gen::generate_system(ctx);
    const auto physics = ::space::gen::derive_all_body_physics(ctx, record);
    CHECK(r["stages"]["space:system"]["hash"] == std::to_string(::space::gen::stage_hash(record)));
    CHECK(r["stages"]["space:body.physics"]["hash"] == std::to_string(::space::gen::stage_hash(physics)));
    CHECK(r["stages"]["space:system"]["hash"] == "2733265849347982535");
    CHECK(r["stages"]["space:body.physics"]["hash"] == "9754287468050259825");

    CHECK(r["planet_count"] == record.planet_count);
    REQUIRE(r["bodies"].size() == record.bodies.size());
    CHECK(r["bodies"][0]["address"] == "S:3f2a91c07e5b0000/B:1");
    CHECK(r["bodies"][0]["mass_kg"] == record.bodies[0].mass_kg);  // doubles round-trip exactly
    CHECK(r["bodies"][0]["physics"].contains("equilibrium_temperature_k"));
    CHECK(r["star"]["kind"] == std::string(record.star.kind));
    CHECK(r["violations"] == json::array());

    CHECK(w.fail("generate_system", {{"seed", "42"}}).code == ErrorCode::mcp_invalid_arguments);
    CHECK(w.fail("generate_system", {{"seed", "x"}, {"address", "S:3f2a91c07e5b0000"}}).code ==
          ErrorCode::mcp_invalid_arguments);
    CHECK(w.fail("generate_system", {{"seed", "42"}, {"address", "S:3f2a91c07e5b0004"}}).code ==
          ErrorCode::mcp_invalid_arguments);  // body bits set
}
