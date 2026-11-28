// Hot reload (docs/specs/hot-reload/spec.md).

#include "support/test_support.hpp"

#include <doctest/doctest.h>

using namespace veach;

namespace {

/// A headless engine on a temp project whose startup script tests rewrite between frames.
struct ReloadRun {
    explicit ReloadRun(std::string_view source) : project("reload") {
        project.write("main.luau", source);
        engine = std::make_unique<Engine>(test::headless_config(project.path(), "main.luau", 0));
        REQUIRE(engine->init().has_value());
        REQUIRE(engine->start().has_value());
    }
    void edit(std::string_view source) { project.write("main.luau", source); }
    [[nodiscard]] Entity lookup(const char* path) const { return engine->world().lookup(path); }
    test::LogCapture logs;
    test::TempProject project;
    std::unique_ptr<Engine> engine;
};

/// v1 and v2 differ in what the system logs and in their source length (the change stamp includes the size).
std::string ticker(std::string_view version) {
    return std::format(R"(--!strict
local veach = require("@veach")
local Marker = veach.component("Marker", {{ n = 0 }})
veach.spawn("thing"):set(Marker, {{ n = 1 }})
veach.system("Tick", "Simulation", veach.query(Marker), function(e, m)
	veach.log.info(`{} frame {{veach.time.frame()}} n={{m.n}}`)
	if veach.time.frame() == 1 then
		veach.spawn("made_by_system"):set(Marker, {{ n = 100 }})
	end
end)
)",
                       version);
}

}  // namespace

TEST_CASE("[reload.AC1] assets reload behind the same handle, a broken version keeps the old data") {
    test::TempProject project("reload-asset");
    project.write("a.luau", "return 1\n");
    World world;
    world.import<CoreModule>();
    world.import<AssetModule>();
    AssetServer assets(world, project.path());
    assets.add_loader(std::make_unique<ScriptLoader>());
    const Handle<ScriptAsset> handle = assets.load<ScriptAsset>("a.luau");
    const std::string v1 = assets.get(handle)->bytecode;

    CHECK(assets.reload_changed().empty());  // nothing changed yet
    project.write("a.luau", "return 1 + 2 + 3\n");
    const std::vector<ReloadResult> changed = assets.reload_changed();
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].reloaded);
    CHECK(changed[0].id == handle.id);
    CHECK(assets.get(handle)->bytecode != v1);
    const std::string v2 = assets.get(handle)->bytecode;

    test::LogCapture logs;
    project.write("a.luau", "return = 1\n");
    const std::vector<ReloadResult> broken = assets.reload_changed();
    REQUIRE(broken.size() == 1);
    CHECK_FALSE(broken[0].reloaded);
    REQUIRE(broken[0].error.has_value());
    CHECK(broken[0].error->code == ErrorCode::script_compile_failed);
    CHECK(assets.get(handle)->bytecode == v2);  // still the last good version
    const std::vector<AssetInfo> listed = assets.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].reloads == 1);
    CHECK(listed[0].reload_error.has_value());

    const ReloadResult unknown = assets.reload("b.luau");
    REQUIRE(unknown.error.has_value());
    CHECK(unknown.error->code == ErrorCode::asset_not_found);
    CHECK(unknown.error->hint.find("Did you mean 'a.luau'?") != std::string::npos);
}

TEST_CASE("[reload.AC2] a changed script replaces its systems and respawns its top-level entities") {
    ReloadRun run(ticker("v1"));
    REQUIRE(run.engine->frame());
    const Entity first = run.lookup("thing");
    REQUIRE(first.is_valid());
    REQUIRE(run.lookup("made_by_system").is_valid());
    const std::size_t systems = run.engine->services().get<ScriptRuntime>().system_count();

    run.edit(ticker("v2 (edited)"));
    const std::vector<ReloadResult> results = run.engine->hot_reload();
    REQUIRE(results.size() == 1);
    CHECK(results[0].reloaded);
    CHECK_FALSE(results[0].error.has_value());
    REQUIRE(run.engine->frame());

    CHECK(run.logs.contains("v1 frame 1 n=1"));
    CHECK(run.logs.contains("v2 (edited) frame 2 n=1"));
    CHECK_FALSE(run.logs.contains("v1 frame 2"));
    CHECK(run.engine->services().get<ScriptRuntime>().system_count() == systems);
    const Entity second = run.lookup("thing");
    REQUIRE(second.is_valid());
    CHECK(second.id() != first.id());  // fresh, as if the script had just started
    CHECK_FALSE(first.is_alive());
    CHECK(run.lookup("made_by_system").is_valid());  // spawned by a system, not the top level: kept
    run.engine->shutdown();
    CHECK_MESSAGE(run.logs.errors().empty(), run.logs.errors());
}

TEST_CASE("[reload.AC3] a script reload that fails changes nothing") {
    ReloadRun run(ticker("v1"));
    REQUIRE(run.engine->frame());
    const Entity thing = run.lookup("thing");
    run.edit(ticker("v2") + "error(\"broken edit\")\n");
    const std::vector<ReloadResult> results = run.engine->hot_reload();
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].error.has_value());
    CHECK(results[0].error->code == ErrorCode::script_runtime_error);
    REQUIRE(run.engine->frame());
    CHECK(run.logs.contains("v1 frame 2 n=1"));  // the old system still runs
    CHECK_FALSE(run.logs.contains("v2 frame"));
    CHECK(run.lookup("thing").id() == thing.id());
    run.engine->shutdown();
}

TEST_CASE("[reload.AC4] a headless run does not reload by itself") {
    ReloadRun run(ticker("v1"));
    REQUIRE(run.engine->frame());
    run.edit(ticker("v2 (edited)"));
    REQUIRE(run.engine->frame());
    REQUIRE(run.engine->frame());
    run.engine->shutdown();
    CHECK(run.logs.contains("v1 frame 3 n=1"));
    CHECK_FALSE(run.logs.contains("v2"));
}
