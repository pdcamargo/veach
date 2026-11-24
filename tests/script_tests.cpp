#include "support/test_support.hpp"

#include <veach/script/luau_types.hpp>

#include <doctest/doctest.h>

#include <algorithm>

using namespace veach;

namespace {

/// Runs `source` as the startup script of a headless engine for `frames` frames.
struct ScriptRun {
    ScriptRun(std::string_view source, std::uint64_t frames = 3) : project("script") {
        project.write("main.luau", source);
        Engine engine(test::headless_config(project.path(), "main.luau", frames));
        exit_code = engine.run();
    }
    test::LogCapture logs;
    test::TempProject project;
    int exit_code = -1;
};

}  // namespace

TEST_CASE("[script.AC1] the hello world logs once per frame and exits cleanly") {
    const auto source = fs::read_file(test::source_dir() / "apps/lab/scripts/hello.luau");
    REQUIRE(source.has_value());
    ScriptRun run(*source, 3);
    CHECK(run.exit_code == 0);
    CHECK(run.logs.messages("script") == std::vector<std::string>{
                                             "Hello from Luau — frame 1",
                                             "Hello from Luau — frame 2",
                                             "Hello from Luau — frame 3",
                                         });
}

TEST_CASE("[script.AC2] scripts load through the asset system") {
    ScriptRun run("--!strict\nlocal veach = require(\"@veach\")\nveach.log.info(\"loaded\")\n", 1);
    CHECK(run.exit_code == 0);
    CHECK(run.logs.contains("loaded"));
    CHECK(fs::exists(run.project.path() / "main.luau.meta"));

    ScriptRun broken("local x = = 1\n", 1);
    CHECK(broken.exit_code == 1);
    CHECK(broken.logs.has_code("script.compile_failed"));
}

TEST_CASE("[script.AC3] a failing top level applies nothing") {
    ScriptRun run(R"(--!strict
local veach = require("@veach")
local Mark = veach.component("Mark", { n = 0 })
veach.system("Count", "Simulation", veach.query(Mark), function(_e, _m)
	veach.log.info("system ran")
end)
veach.spawn("marked"):set(Mark, { n = 1 })
error("boom")
)",
                  2);
    CHECK(run.exit_code == 1);
    CHECK(run.logs.has_code("script.runtime_error"));
    CHECK_FALSE(run.logs.contains("system ran"));
}

TEST_CASE("[script.AC4] a failing system run discards only that run's writes") {
    ScriptRun run(R"(--!strict
local veach = require("@veach")
local Counter = veach.component("Counter", { value = 0 })
veach.system("Bump", "Simulation", veach.query(Counter), function(e, c)
	e:set(Counter, { value = c.value + 1 })
	if veach.time.frame() == 2 then
		error("fail on frame 2")
	end
end)
veach.system("Report", "PreExtract", veach.query(Counter), function(_e, c)
	veach.log.info(`counter {c.value}`)
end)
veach.spawn():set(Counter, { value = 0 })
)",
                  3);
    CHECK(run.exit_code == 1);  // an error was logged
    CHECK(run.logs.messages("script") ==
          std::vector<std::string>{"counter 1", "counter 1", "counter 2"});  // frame 2's bump was discarded
}

TEST_CASE("[script.AC5] systems get the whole query result in one batch") {
    ScriptRun run(R"(--!strict
local veach = require("@veach")
local Pos = veach.component("Pos", { p = vector.create(0, 0, 0), tag = "" })
local Vel = veach.component("Vel", { v = vector.create(1, 2, 3) })
veach.system("Move", "Simulation", veach.query(Pos, Vel), function(e, pos, vel)
	e:set(Pos, { p = pos.p + vel.v, tag = pos.tag })
end)
veach.system("Report", "Render", veach.query(Pos), function(_e, pos)
	if veach.time.frame() == 2 then
		veach.log.info(`{pos.tag} {pos.p.x} {pos.p.y} {pos.p.z}`)
	end
end)
for i = 1, 100 do
	local e = veach.spawn()
	e:set(Pos, { p = vector.create(i, 0, 0), tag = `e{i}` })
	if i % 2 == 0 then
		e:set(Vel, { v = vector.create(1, 2, 3) })
	end
end
)",
                  2);
    CHECK(run.exit_code == 0);
    const auto lines = run.logs.messages("script");
    CHECK(lines.size() == 100);
    CHECK(std::ranges::count(lines, std::string("e2 4 4 6")) == 1);  // moved twice
    CHECK(std::ranges::count(lines, std::string("e1 1 0 0")) == 1);  // no Vel, never moved
}

TEST_CASE("[script.AC6] mistakes produce actionable errors") {
    ScriptRun unknown(R"(--!strict
local veach = require("@veach")
local Greeting = veach.component("Greeting", { text = "" })
veach.spawn():set({ name = "Greting", defaults = { text = "" } }, { text = "hi" })
)",
                      1);
    CHECK(unknown.logs.contains("unknown component 'Greting' — Did you mean 'Greeting'?"));

    ScriptRun phase(R"(local veach = require("@veach")
local C = veach.component("C", { x = 0 })
veach.system("S", "Simulaton", veach.query(C), function() end)
)",
                    1);
    CHECK(phase.logs.contains("unknown phase 'Simulaton' — Did you mean 'Simulation'?"));

    ScriptRun field(R"(local veach = require("@veach")
local C = veach.component("C", { text = "" })
veach.spawn():set(C, { txt = "x" })
)",
                    1);
    CHECK(field.logs.contains("unknown field 'txt' in C — Did you mean 'text'?"));

    ScriptRun module(R"(local x = require("@veech"))", 1);
    CHECK(module.logs.has_code("script.runtime_error"));
    CHECK(module.logs.contains("unknown module '@veech'"));
}

TEST_CASE("[script.AC7] C++ components tagged Scriptable are visible to scripts") {
    ScriptRun run(R"(--!strict
local veach = require("@veach")
local Window = veach.components.Window
veach.log.info(`default title {Window.defaults.title}`)
veach.system("ReadWindow", "Input", veach.query(Window), function(e, w)
	veach.log.info(`{w.title} {w.width}x{w.height} {w.mode}`)
	if veach.time.frame() == 1 then
		e:set(Window, { title = "Renamed", width = 640, height = w.height, mode = "fullscreen", vsync = w.vsync })
	end
end)
)",
                  2);
    CHECK(run.exit_code == 0);
    CHECK(run.logs.contains("default title Veach"));
    CHECK(run.logs.contains("Veach 1280x720 windowed"));
    CHECK(run.logs.contains("Renamed 640x720 fullscreen"));  // written through the command buffer
}

TEST_CASE("[script.AC8] generated types cover every binding and every phase") {
    Engine engine(test::headless_config(test::source_dir(), {}, 1));
    REQUIRE(engine.init().has_value());
    const std::string types = generate_luau_types(engine.world());
    for (const std::string_view name : ScriptRuntime::api_names()) {
        CHECK_MESSAGE(types.find(std::format("read {}:", name)) != std::string::npos, "binding not typed: ", name);
    }
    for (const PhaseInfo& info : phases()) {
        CHECK(types.find(std::format("\"{}\"", info.name)) != std::string::npos);
    }
    CHECK(types.find("export type Window = {") != std::string::npos);
    CHECK(types.find("read Window: Component<Window>,") != std::string::npos);
    CHECK(types.find("export type WindowMode = \"windowed\" | \"fullscreen\"") != std::string::npos);

    const auto committed = fs::read_file(test::source_dir() / "types/veach.d.luau");
    REQUIRE(committed.has_value());
    CHECK_MESSAGE(*committed == types, "types/veach.d.luau is stale: run `veach types --out types/veach.d.luau`");
    engine.shutdown();
}
