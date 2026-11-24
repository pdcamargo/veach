#include "support/test_support.hpp"

#include <doctest/doctest.h>

using namespace veach;

namespace {

std::vector<std::string> lifecycle(const test::LogCapture& logs) {
    std::vector<std::string> steps;
    for (const std::string& m : logs.messages("engine")) {
        if (m.starts_with("init ") || m.starts_with("shutdown ")) {
            steps.push_back(m.substr(0, m.find(" (")));
        }
    }
    return steps;
}

struct ShutdownProbe {
    explicit ShutdownProbe(World& world) {
        world.module<ShutdownProbe>();
        world.system("ProbeStartup").kind<phase::Startup>().run([](Iter&) { log::info("probe", "startup"); });
        world.system("ProbeShutdown").kind<phase::Shutdown>().run([](Iter&) { log::info("probe", "shutdown"); });
    }
};

}  // namespace

TEST_CASE("[lifecycle.AC1] init runs in order and shutdown in exact reverse") {
    test::LogCapture logs;
    test::TempProject project("lifecycle");
    {
        Engine engine(test::headless_config(project.path(), {}, 2));
        engine.import<ShutdownProbe>();
        CHECK(engine.run() == 0);
        CHECK(engine.frame_count() == 2);
    }
    const std::vector<std::string> expected{
        "init core",       "init platform", "init ecs",          "init assets",     "init script",  "init rhi",
        "init render",     "init modules",  "shutdown modules",  "shutdown render", "shutdown rhi", "shutdown script",
        "shutdown assets", "shutdown ecs",  "shutdown platform", "shutdown core",
    };
    CHECK(lifecycle(logs) == expected);
    CHECK(logs.messages("probe") == std::vector<std::string>{"startup", "shutdown"});
}

TEST_CASE("[lifecycle.AC2] a failed init step unwinds the steps that completed") {
    test::LogCapture logs;
    test::TempProject project("lifecycle-fail");
    EngineConfig config = test::headless_config(project.path(), {}, 1);
    config.project.rhi = rhi::Backend::dx12;  // unavailable on macOS, a stub on Windows: fails either way
    Engine engine(std::move(config));
    CHECK(engine.run() == 1);
    CHECK(logs.has_code("engine.init_failed"));
    const std::vector<std::string> expected{"init core",         "init platform",   "init ecs",        "init assets",
                                            "init script",       "shutdown script", "shutdown assets", "shutdown ecs",
                                            "shutdown platform", "shutdown core"};
    CHECK(lifecycle(logs) == expected);
}

TEST_CASE("[lifecycle.AC3] headless runs are deterministic: one fixed step per frame") {
    test::TempProject project("lifecycle-time");
    Engine engine(test::headless_config(project.path(), {}, 5));
    REQUIRE(engine.init().has_value());
    REQUIRE(engine.start().has_value());
    while (engine.frame()) {
    }
    const Time& time = engine.world().get<Time>();
    CHECK(time.frame == 5);
    CHECK(time.fixed_step == 5);
    CHECK(time.elapsed == doctest::Approx(5.0 / 60.0));
    engine.shutdown();
}

TEST_CASE("[lifecycle.AC4] CLI flags override the project file") {
    test::TempProject project("config");
    project.write("veach.project.json", R"({"name": "demo", "frames": 10, "seed": 1, "window": {"title": "Demo"}})");
    const std::string dir = project.path().string();
    const std::vector<std::string_view> args{"--project", dir,          "--frames",     "2",
                                             "--seed=7",  "--headless", "--log-format", "json"};
    const auto config = resolve_config({.name = "t"}, args);
    REQUIRE(config.has_value());
    CHECK(config->project.name == "demo");
    CHECK(config->project.window.title == "Demo");
    CHECK(config->project.frames == 2);
    CHECK(config->project.seed == 7);
    CHECK(config->project.headless);
    CHECK(config->project.log_format == log::Format::json);
}

TEST_CASE("[lifecycle.AC5] bad flags and bad project files explain the fix") {
    const std::vector<std::string_view> typo{"--frmes", "3"};
    auto r = resolve_config({.name = "t"}, typo);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::cli_unknown_flag);
    CHECK(r.error().hint.starts_with("Did you mean '--frames'?"));

    const std::vector<std::string_view> both{"--headless", "--window"};
    CHECK(resolve_config({.name = "t"}, both).error().code == ErrorCode::cli_conflicting_flags);

    const std::vector<std::string_view> bad_number{"--frames", "three"};
    CHECK(resolve_config({.name = "t"}, bad_number).error().code == ErrorCode::cli_invalid_value);

    test::TempProject project("config-bad");
    project.write("veach.project.json", R"({"window": {"tittle": "x"}})");
    const std::string dir = project.path().string();
    const std::vector<std::string_view> args{"--project", dir};
    auto bad = resolve_config({.name = "t"}, args);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::data_unknown_field);
    CHECK(bad.error().message.find("window.tittle") != std::string::npos);
    CHECK(bad.error().hint == "Did you mean 'title'?");
}
