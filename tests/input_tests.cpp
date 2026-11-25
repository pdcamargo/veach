// Input actions (docs/specs/input/spec.md).

#include "support/test_support.hpp"

#include <veach/platform/platform.hpp>

#include <doctest/doctest.h>

#include <algorithm>

using namespace veach;

namespace {

Result<ProjectConfig> load_input(std::string_view input_json) {
    test::TempProject project("input-config");
    project.write("veach.project.json", std::format(R"({{"name": "t", "input": {}}})", input_json));
    ProjectConfig config;
    if (auto loaded = load_project_file(project.path() / "veach.project.json", config); !loaded) {
        return std::unexpected(loaded.error());
    }
    return config;
}

/// An engine with `jump` on space and `left`/`right` on a/d, running `source` as its startup script.
struct InputRun {
    explicit InputRun(std::string_view source) : project("input") {
        project.write("veach.project.json",
                      R"({"name": "input", "input": {"jump": ["space"], "left": ["a"], "right": ["d", "right"]}})");
        project.write("main.luau", source);
        auto config = resolve_config({.name = "t", .default_project = {}},
                                     std::vector<std::string_view>{"--project", project.path().string(), "--headless",
                                                                   "--script", "main.luau", "--log-level", "debug"});
        REQUIRE(config.has_value());
        config->log_to_console = false;
        engine = std::make_unique<Engine>(std::move(*config));
        REQUIRE(engine->init().has_value());
        REQUIRE(engine->start().has_value());
    }
    void key(Key k, bool down) {
        engine->services().get<Platform>().inject(
            {.type = down ? PlatformEvent::Type::key_down : PlatformEvent::Type::key_up, .key = k});
    }
    test::LogCapture logs;
    test::TempProject project;
    std::unique_ptr<Engine> engine;
};

constexpr std::string_view report_jump = R"(--!strict
local veach = require("@veach")
local input = veach.input
veach.system("Report", "Input", veach.query(veach.components.Window), function(_e, _w)
	veach.log.info(`frame {veach.time.frame()}: down={input.down("jump")} pressed={input.pressed("jump")} released={input.released("jump")} axis={input.axis("left", "right")}`)
end)
)";

}  // namespace

TEST_CASE("[input.AC1] the project file binds actions, with actionable errors") {
    auto config = load_input(R"({"jump": ["space", "w"], "fire": ["mouse_left"]})");
    REQUIRE(config.has_value());
    REQUIRE(config->input.size() == 2);
    const auto jump = std::ranges::find(config->input, std::string("jump"), &ActionBinding::action);
    REQUIRE(jump != config->input.end());
    CHECK(jump->primary == Key::space);
    CHECK(jump->secondary == Key::w);

    auto typo = load_input(R"({"jump": ["spcae"]})");
    REQUIRE_FALSE(typo.has_value());
    CHECK(typo.error().code == ErrorCode::input_unknown_key);
    CHECK(typo.error().hint.find("Did you mean 'space'?") != std::string::npos);

    auto too_many = load_input(R"({"jump": ["space", "w", "up"]})");
    REQUIRE_FALSE(too_many.has_value());
    CHECK(too_many.error().code == ErrorCode::data_type_mismatch);

    InputRun run("");
    CHECK(run.engine->world().lookup("veach::actions::right").get<InputAction>().secondary == Key::right);
}

TEST_CASE("[input.AC2] injected keys drive actions that scripts read") {
    InputRun run(report_jump);
    run.key(Key::space, true);
    run.key(Key::d, true);
    REQUIRE(run.engine->frame());
    REQUIRE(run.engine->frame());
    run.key(Key::space, false);
    REQUIRE(run.engine->frame());
    run.engine->shutdown();
    CHECK(run.logs.contains("frame 1: down=true pressed=true released=false axis=1"));
    CHECK(run.logs.contains("frame 2: down=true pressed=false released=false axis=1"));
    CHECK(run.logs.contains("frame 3: down=false pressed=false released=true axis=1"));
    CHECK_MESSAGE(run.logs.errors().empty(), run.logs.errors());
}

TEST_CASE("[input.AC3] an unknown action is an error with a suggestion") {
    InputRun run(R"(--!strict
local veach = require("@veach")
veach.system("Typo", "Input", veach.query(veach.components.Window), function(_e, _w)
	local _ = veach.input.down("jmp")
end)
)");
    REQUIRE(run.engine->frame());
    run.engine->shutdown();
    CHECK(run.logs.contains("no input action 'jmp'"));
    CHECK(run.logs.contains("Did you mean 'jump'?"));
    CHECK(run.logs.contains("[input.unknown_action]"));
}

TEST_CASE("[input.AC4] an untranslated key never trips actions") {
    InputRun run(report_jump);
    run.key(Key::unknown, true);  // `jump` has no second key: its unused slot is Key::unknown
    REQUIRE(run.engine->frame());
    run.engine->shutdown();
    CHECK(run.logs.contains("frame 1: down=false pressed=false released=false axis=0"));
}

TEST_CASE("[input.AC6] dev scripts run after the startup script, in order") {
    test::TempProject project("dev-scripts");
    project.write("veach.project.json", R"({"name": "t", "dev_scripts": ["dev/a.luau", "dev/b.luau"]})");
    project.write("main.luau", "require(\"@veach\").log.info(\"ran main\")\n");
    project.write("dev/a.luau", "require(\"@veach\").log.info(\"ran a\")\n");
    project.write("dev/b.luau", "require(\"@veach\").log.info(\"ran b\")\n");
    auto config = resolve_config(
        {.name = "t", .default_project = {}},
        std::vector<std::string_view>{"--project", project.path().string(), "--headless", "--script", "main.luau"});
    REQUIRE(config.has_value());
    CHECK(config->project.dev_scripts == std::vector<std::string>{"dev/a.luau", "dev/b.luau"});
    config->log_to_console = false;
    test::LogCapture logs;
    Engine engine(std::move(*config));
    REQUIRE(engine.init().has_value());
    REQUIRE(engine.start().has_value());
    engine.shutdown();
    CHECK(logs.messages("script") == std::vector<std::string>{"ran main", "ran a", "ran b"});

    test::TempProject bad("dev-scripts-bad");
    bad.write("veach.project.json", R"({"name": "t", "dev_scripts": "dev/a.luau"})");
    ProjectConfig loaded;
    auto result = load_project_file(bad.path() / "veach.project.json", loaded);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::data_type_mismatch);
}
