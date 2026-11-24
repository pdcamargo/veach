#include "support/test_support.hpp"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

using namespace veach;

// A named namespace on purpose: Flecs derives enum constant names at compile
// time and cannot parse types inside an anonymous namespace.
namespace ecs_test {

struct Health {
    float value = 100.0f;
    std::string label = "hp";
};

struct Unreflected {
    int x = 0;
};

enum class Team : std::uint8_t { red, blue };

struct Settings {
    std::string name = "default";
    std::uint32_t count = 1;
    Team team = Team::red;
    math::vec3 offset{0.0f};
};

World make_world() {
    World world;
    world.import<CoreModule>();
    world.component<Health>().member(&Health::value, "value").member(&Health::label, "label");
    world.component<Team>();
    world.component<Settings>()
        .member(&Settings::name, "name")
        .member(&Settings::count, "count")
        .member(&Settings::team, "team")
        .member(&Settings::offset, "offset");
    return world;
}

}  // namespace ecs_test

using namespace ecs_test;

TEST_CASE("[ecs.AC3] command buffer applies writes in order") {
    World world = make_world();
    CommandBuffer cmd(world);
    const Entity e = cmd.spawn("player");
    cmd.set(e, Health{50.0f, "shield"});
    CHECK_FALSE(e.has<Health>());  // nothing applied yet
    CHECK(cmd.size() == 2);
    cmd.apply();
    CHECK(cmd.empty());
    REQUIRE(e.has<Health>());
    CHECK(e.get<Health>().value == 50.0f);
    CHECK(e.get<Health>().label == "shield");
    CHECK(std::string(e.name().c_str()) == "player");

    CommandBuffer second(world);
    second.remove<Health>(e);
    second.despawn(e);
    second.apply();
    CHECK_FALSE(e.is_alive());
}

TEST_CASE("[ecs.AC4] command buffer discard leaves the world untouched") {
    World world = make_world();
    const Entity existing = world.entity().set<Health>({});
    ecs::entity_t spawned = 0;
    {
        CommandBuffer cmd(world);
        const Entity e = cmd.spawn("ghost");
        spawned = e.id();
        cmd.set(e, Health{1.0f, "x"});
        cmd.set(existing, Health{0.0f, "dead"});
        cmd.despawn(existing);
        cmd.discard();
    }
    CHECK_FALSE(world.is_alive(spawned));
    REQUIRE(existing.is_alive());
    CHECK(existing.get<Health>().value == 100.0f);
    CHECK_FALSE(world.lookup("ghost"));
}

TEST_CASE("[ecs.AC4] a destroyed command buffer discards") {
    World world = make_world();
    ecs::entity_t spawned = 0;
    {
        CommandBuffer cmd(world);
        spawned = cmd.spawn().id();
    }
    CHECK_FALSE(world.is_alive(spawned));
}

TEST_CASE("[ecs.AC5] unreflected components are rejected with a fix") {
    World world = make_world();
    CHECK(check_reflection(world).has_value());
    world.component<Unreflected>();
    const auto result = check_reflection(world);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::ecs_unreflected_component);
    CHECK(result.error().message.find("Unreflected") != std::string::npos);
    CHECK(result.error().hint.find(".member(") != std::string::npos);
}

TEST_CASE("[ecs.AC6] phases run in the documented order") {
    const std::vector<std::string_view> expected{
        "Startup",    "Input",   "Simulation", "PostSimulation", "TransformPropagation",
        "PreExtract", "Extract", "Render",     "Shutdown"};
    std::vector<std::string_view> actual;
    for (const PhaseInfo& info : phases()) {
        actual.push_back(info.name);
    }
    CHECK(actual == expected);
    CHECK(find_phase("Simulation") == PhaseId::simulation);
    CHECK_FALSE(find_phase("Simulaton").has_value());

    World world = make_world();
    PhaseRunner runner;
    runner.register_phases(world);
    std::vector<std::string> ran;
    world.system("B").kind<phase::Simulation>().run([&](Iter&) { ran.emplace_back("sim-1"); });
    world.system("A").kind<phase::Simulation>().run([&](Iter&) { ran.emplace_back("sim-2"); });
    world.system("I").kind<phase::Input>().run([&](Iter&) { ran.emplace_back("input"); });
    runner.run(world, PhaseId::input, 0.0f);
    runner.run(world, PhaseId::simulation, 0.0f);
    runner.run(world, PhaseId::render, 0.0f);
    CHECK(ran == std::vector<std::string>{"input", "sim-1", "sim-2"});  // declaration order within a phase
}

TEST_CASE("[ecs.AC7] reflection-driven JSON reads data files and explains mistakes") {
    World world = make_world();
    Settings s;
    auto ok = from_json(world, s, nlohmann::json::parse(R"({"name":"x","count":3,"team":"blue","offset":[1,2,3]})"),
                        "test.json");
    REQUIRE(ok.has_value());
    CHECK(s.name == "x");
    CHECK(s.count == 3);
    CHECK(s.team == Team::blue);
    CHECK(s.offset.z == 3.0f);

    auto unknown = from_json(world, s, nlohmann::json::parse(R"({"nmae":"x"})"), "test.json");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::data_unknown_field);
    CHECK(unknown.error().hint == "Did you mean 'name'?");

    auto bad_enum = from_json(world, s, nlohmann::json::parse(R"({"team":"bleu"})"), "test.json");
    REQUIRE_FALSE(bad_enum.has_value());
    CHECK(bad_enum.error().hint.find("Did you mean 'blue'?") != std::string::npos);

    auto mismatch = from_json(world, s, nlohmann::json::parse(R"({"count":"three"})"), "test.json");
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error().code == ErrorCode::data_type_mismatch);
}

TEST_CASE("[ecs.AC8] JSON Schemas are generated from reflection") {
    World world = make_world();
    const Settings defaults;
    const nlohmann::json schema = json_schema(world, world.id<Settings>(), &defaults);
    CHECK(schema["type"] == "object");
    CHECK(schema["additionalProperties"] == false);
    CHECK(schema["properties"]["count"]["type"] == "integer");
    CHECK(schema["properties"]["team"]["enum"] == nlohmann::json::array({"red", "blue"}));
    CHECK(schema["properties"]["name"]["default"] == "default");
    CHECK(schema["properties"]["offset"]["maxItems"] == 3);
}
