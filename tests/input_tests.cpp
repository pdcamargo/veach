// Input actions (docs/specs/input/spec.md).

#include "support/test_support.hpp"

#include <veach/platform/platform.hpp>
#include <veach/platform/platform_module.hpp>
#include <veach/runtime/replay.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

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

// ---- analog axes (VCH-101) and gamepads (VCH-036) ----

namespace {

constexpr std::string_view axes_project = R"({"name": "axes",
  "input": {"look": ["mouse_right"], "fire": ["gamepad_south"]},
  "axes": {
    "look_x": {"source": "mouse_x", "sensitivity": 0.5},
    "move_x": {"source": "gamepad_left_x", "negative": "a", "positive": "d", "dead_zone": 0.2},
    "pitch": {"source": "mouse_y", "invert": true}
  }})";

/// An engine on `axes_project`, running `source`, with extra flags (--record, --replay).
struct AxisRun {
    explicit AxisRun(std::string_view source, const std::vector<std::string>& extra = {}) : project("axes") {
        project.write("veach.project.json", axes_project);
        project.write("main.luau", source);
        std::vector<std::string> args{"--project", project.path().string(), "--headless", "--script", "main.luau"};
        args.insert(args.end(), extra.begin(), extra.end());
        const std::vector<std::string_view> views(args.begin(), args.end());
        auto config = resolve_config({.name = "t", .default_project = {}}, views);
        REQUIRE(config.has_value());
        config->log_to_console = false;
        engine = std::make_unique<Engine>(std::move(*config));
        REQUIRE(engine->init().has_value());
        REQUIRE(engine->start().has_value());
    }
    void inject(const PlatformEvent& e) const { engine->services().get<Platform>().inject(e); }
    void axis(Axis a, double value) const {
        inject({.type = PlatformEvent::Type::axis_moved, .axis = a, .value = quantize_axis(value)});
    }
    void key(Key k, bool down) const {
        inject({.type = down ? PlatformEvent::Type::key_down : PlatformEvent::Type::key_up, .key = k});
    }
    [[nodiscard]] float value(std::string_view name) const { return axis_value(engine->world(), name); }
    test::LogCapture logs;
    test::TempProject project;
    std::unique_ptr<Engine> engine;
};

Result<ProjectConfig> load_axes(std::string_view axes_json) {
    test::TempProject project("axes-config");
    project.write("veach.project.json", std::format(R"({{"name": "t", "axes": {}}})", axes_json));
    ProjectConfig config;
    if (auto loaded = load_project_file(project.path() / "veach.project.json", config); !loaded) {
        return std::unexpected(loaded.error());
    }
    return config;
}

constexpr std::string_view report_axes = R"(--!strict
local veach = require("@veach")
local input = veach.input
veach.system("ReportInput", "Input", veach.query(veach.components.Window), function(_e, _w)
	veach.log.info(`input {veach.time.frame()}: look_x={input.axis("look_x")} move_x={input.axis("move_x")} pads={input.gamepads()} fire={input.down("fire")}`)
end)
veach.system("ReportSim", "Simulation", veach.query(veach.components.Window), function(_e, _w)
	veach.log.info(`sim {veach.time.frame()}: look_x={input.axis("look_x")}`)
end)
)";

}  // namespace

TEST_CASE("[input.AC7] the project file binds analog axes, with actionable errors") {
    auto config = load_axes(R"({"look_x": {"source": "mouse_x", "sensitivity": 0.003, "invert": true},
                               "move": {"negative": "a", "positive": "d"}})");
    REQUIRE(config.has_value());
    REQUIRE(config->axes.size() == 2);
    const auto look = std::ranges::find(config->axes, std::string("look_x"), &AxisActionBinding::action);
    REQUIRE(look != config->axes.end());
    CHECK(look->binding.source == Axis::mouse_x);
    CHECK(look->binding.sensitivity == doctest::Approx(0.003));
    CHECK(look->binding.invert);
    CHECK(look->binding.dead_zone == 0.0f);

    auto axis_typo = load_axes(R"({"look_x": {"source": "mouse_xx"}})");
    REQUIRE_FALSE(axis_typo.has_value());
    CHECK(axis_typo.error().code == ErrorCode::input_unknown_axis);
    CHECK(axis_typo.error().hint.find("Did you mean 'mouse_x'?") != std::string::npos);

    auto key_typo = load_axes(R"({"move": {"positive": "spcae"}})");
    REQUIRE_FALSE(key_typo.has_value());
    CHECK(key_typo.error().code == ErrorCode::input_unknown_key);
    CHECK(key_typo.error().hint.find("Did you mean 'space'?") != std::string::npos);

    auto field_typo = load_axes(R"({"move": {"sensitivty": 2}})");
    REQUIRE_FALSE(field_typo.has_value());
    CHECK(field_typo.error().code == ErrorCode::data_unknown_field);
    CHECK(field_typo.error().hint.find("Did you mean 'sensitivity'?") != std::string::npos);

    CHECK(load_axes(R"({"move": {"dead_zone": 1.0}})").error().code == ErrorCode::data_type_mismatch);
    CHECK(load_axes(R"({"move": ["a", "d"]})").error().code == ErrorCode::data_type_mismatch);

    const AxisRun run("");
    const InputAxis& move = run.engine->world().lookup("veach::actions::move_x").get<InputAxis>();
    CHECK(move.binding.source == Axis::gamepad_left_x);
    CHECK(move.binding.positive == Key::d);
}

TEST_CASE("[input.AC8] dead zones, sensitivity, inversion and the key fallback") {
    Input input;
    const AxisBinding stick{.source = Axis::gamepad_left_x, .negative = Key::a, .positive = Key::d, .dead_zone = 0.2f};
    const auto at = [&](double v) {
        input.move_axis(Axis::gamepad_left_x, quantize_axis(v));
        return evaluate_axis(stick, input);
    };
    CHECK(at(0.0) == 0.0f);
    CHECK(at(0.19) == 0.0f);                               // inside the dead zone
    CHECK(at(-0.2) == 0.0f);                               // its edge reads 0 too
    CHECK(at(0.6) == doctest::Approx(0.5).epsilon(1e-4));  // the rest is rescaled: (0.6 - 0.2) / 0.8
    CHECK(at(-1.0) == -1.0f);                              // full deflection still reaches 1
    input.move_axis(Axis::gamepad_left_x, 0);
    input.set_key(Key::d, true);
    CHECK(evaluate_axis(stick, input) == 1.0f);  // keys are the digital fallback, exactly 1
    input.set_key(Key::a, true);
    CHECK(evaluate_axis(stick, input) == 0.0f);

    Input mouse;
    mouse.move_axis(Axis::mouse_x, quantize_axis(10.0));
    mouse.move_axis(Axis::mouse_x, quantize_axis(6.0));  // relative: motion adds up
    CHECK(mouse.axis(Axis::mouse_x) == 16 * axis_one);
    CHECK(evaluate_axis({.source = Axis::mouse_x, .sensitivity = 0.25f}, mouse) == 4.0f);
    CHECK(evaluate_axis({.source = Axis::mouse_x, .sensitivity = 0.25f, .invert = true}, mouse) == -4.0f);
    CHECK(evaluate_axis({.source = Axis::mouse_x, .dead_zone = 0.5f}, mouse) == 15.5f);  // unbounded: no rescale
}

TEST_CASE("[input.AC9] analog values are quantized to 1/65536, deterministically") {
    CHECK(axis_one == 65536);
    CHECK(quantize_axis(1.0) == 65536);
    CHECK(quantize_axis(0.1) == 6554);       // 6553.6 rounds to nearest
    CHECK(quantize_axis(0.5 / 65536) == 1);  // halves round away from zero
    CHECK(quantize_axis(-0.5 / 65536) == -1);
    CHECK(quantize_axis(1e12) == std::numeric_limits<std::int32_t>::max());  // saturates
    CHECK(quantize_axis(-1e12) == std::numeric_limits<std::int32_t>::min());
    CHECK(quantize_axis(std::nan("")) == 0);
    // Every value a script or tool reads is on the grid, whatever the sensitivity.
    Input input;
    input.move_axis(Axis::mouse_x, quantize_axis(3.0));
    const float v = evaluate_axis({.source = Axis::mouse_x, .sensitivity = 0.0025f}, input);
    const double scaled = static_cast<double>(v) * axis_one;
    CHECK(scaled == std::round(scaled));
    CHECK(v == doctest::Approx(0.0075).epsilon(1e-3));
}

TEST_CASE("[input.AC10] mouse motion counts once, in the frame's first fixed step, sticks hold") {
    AxisRun run(report_axes);
    run.axis(Axis::mouse_x, 10.0);
    run.axis(Axis::mouse_x, 6.0);
    run.axis(Axis::gamepad_left_x, 0.6);
    REQUIRE(run.engine->frame());
    CHECK(run.value("look_x") == 0.0f);  // consumed by the step that read it
    const float move = run.value("move_x");
    CHECK(move == doctest::Approx(0.5).epsilon(1e-4));
    REQUIRE(run.engine->frame());
    CHECK(run.value("move_x") == move);  // an absolute axis holds until it moves
    run.axis(Axis::mouse_y, 2.0);
    REQUIRE(run.engine->frame());
    CHECK(run.engine->world().get<Input>().axis(Axis::mouse_y) == 0);
    run.engine->shutdown();
    CHECK(run.logs.contains("input 1: look_x=8 "));
    CHECK(run.logs.contains("sim 1: look_x=8"));
    CHECK(run.logs.contains("input 2: look_x=0 "));
    CHECK(run.logs.contains("sim 2: look_x=0"));
    CHECK_MESSAGE(run.logs.errors().empty(), run.logs.errors());

    // A frame without fixed steps keeps the motion for the next one; consume_relative_axes is what a step does.
    World world;
    world.import<CoreModule>();
    world.import<PlatformModule>();
    bind_axis(world, "pitch", {.source = Axis::mouse_y, .invert = true});
    world.get_mut<Input>().move_axis(Axis::mouse_y, 3 * axis_one);
    world.lookup("veach::actions::pitch").get_mut<InputAxis>().value = -3.0f;
    consume_relative_axes(world);
    CHECK(axis_value(world, "pitch") == 0.0f);
    CHECK(world.get<Input>().axis(Axis::mouse_y) == 0);
}

TEST_CASE("[input.AC11] gamepads connect and disconnect, their buttons bind like keys") {
    AxisRun run(report_axes);
    run.inject({.type = PlatformEvent::Type::gamepad_added});
    run.key(Key::gamepad_south, true);
    run.axis(Axis::gamepad_left_x, -1.0);
    REQUIRE(run.engine->frame());
    CHECK(run.engine->world().get<Input>().gamepads == 1);
    CHECK(action_down(run.engine->world(), "fire"));
    CHECK(run.value("move_x") == -1.0f);
    run.inject({.type = PlatformEvent::Type::gamepad_removed});
    REQUIRE(run.engine->frame());
    CHECK(run.engine->world().get<Input>().gamepads == 0);
    CHECK(action_released(run.engine->world(), "fire"));  // the last pad left: its buttons let go
    CHECK(run.value("move_x") == 0.0f);                   // and its sticks center
    run.engine->shutdown();
    CHECK(run.logs.contains("input 1: look_x=0 move_x=-1 pads=1 fire=true"));
    CHECK(run.logs.contains("input 2: look_x=0 move_x=0 pads=0 fire=false"));
}

TEST_CASE("[input.AC12] axis(name) on an unknown or key-only action is an error with a suggestion") {
    AxisRun run(R"(--!strict
local veach = require("@veach")
veach.system("Typo", "Input", veach.query(veach.components.Window), function(_e, _w)
	local _ = veach.input.axis("lookx")
end)
veach.system("KeyAction", "Input", veach.query(veach.components.Window), function(_e, _w)
	local _ = veach.input.axis("look")
end)
)");
    REQUIRE(run.engine->frame());
    run.engine->shutdown();
    CHECK(run.logs.contains("no analog input action 'lookx'"));
    CHECK(run.logs.contains("Did you mean 'look_x'?"));
    CHECK(run.logs.contains("'look' is a key action"));
    CHECK(run.logs.contains("[input.unknown_action]"));
}

TEST_CASE("[input.AC13] RelativeMouse on a window turns relative mouse mode on and off") {
    const AxisRun run("");
    Entity window = run.engine->world().lookup("primary_window");
    window.set<RelativeMouse>({});
    REQUIRE(run.engine->frame());
    CHECK(window.get<WindowPlatformState>().relative_mouse);
    window.set<RelativeMouse>({.enabled = false});
    REQUIRE(run.engine->frame());
    CHECK_FALSE(window.get<WindowPlatformState>().relative_mouse);
    window.set<RelativeMouse>({});
    REQUIRE(run.engine->frame());
    window.remove<RelativeMouse>();
    REQUIRE(run.engine->frame());
    CHECK_FALSE(window.get<WindowPlatformState>().relative_mouse);
}

TEST_CASE("[input.AC14] recorded axes replay bit for bit") {
    const test::TempProject out("axes-replay");
    const std::string recording = (out.path() / "axes.vreplay").string();
    std::vector<float> recorded;
    {
        AxisRun run(report_axes, {"--record", recording});
        run.axis(Axis::gamepad_left_x, 0.123456789);
        run.axis(Axis::mouse_x, 1.0 / 3.0);
        REQUIRE(run.engine->frame());
        recorded.push_back(run.value("move_x"));
        run.axis(Axis::mouse_x, -7.77);
        run.key(Key::mouse_right, true);
        REQUIRE(run.engine->frame());
        recorded.push_back(run.value("move_x"));
        run.engine->shutdown();
    }
    auto log = read_replay(recording);
    REQUIRE(log.has_value());
    REQUIRE(log->frames.size() == 2);
    REQUIRE(log->frames[0].events.size() == 2);
    const PlatformEvent& stick = log->frames[0].events[0];
    CHECK(stick.type == PlatformEvent::Type::axis_moved);
    CHECK(stick.axis == Axis::gamepad_left_x);
    CHECK(stick.value == quantize_axis(0.123456789));
    CHECK(log->frames[0].events[1].value == quantize_axis(1.0 / 3.0));

    AxisRun replay(report_axes, {"--replay", recording});
    REQUIRE(replay.engine->frame());
    CHECK(replay.value("move_x") == recorded[0]);
    static_cast<void>(replay.engine->frame());  // the recording's last frame: the replay ends here
    CHECK(replay.value("move_x") == recorded[1]);
    replay.engine->shutdown();
    CHECK_FALSE(replay.logs.contains("replay.diverged"));
    CHECK_MESSAGE(replay.logs.errors().empty(), replay.logs.errors());
}

namespace {
/// Binds an action and an axis from its constructor, inside its own module scope.
struct BindingModule {
    explicit BindingModule(World& world) {
        world.module<BindingModule>();
        bind_action(world, "module_fire", Key::space);
        bind_axis(world, "module_throttle", {.source = Axis::gamepad_right_trigger});
    }
};
}  // namespace

TEST_CASE("[input.AC17] actions and axes bound inside a module's scope live at the root") {
    World world;
    world.import<CoreModule>();
    world.import<PlatformModule>();
    world.import<BindingModule>();
    CHECK(world.lookup("veach::actions::module_fire").is_valid());
    CHECK(world.lookup("veach::actions::module_throttle").is_valid());
    Input& input = world.get_mut<Input>();
    input.move_axis(Axis::gamepad_right_trigger, axis_one / 2);
    Entity throttle = world.lookup("veach::actions::module_throttle");
    throttle.get_mut<InputAxis>().value = evaluate_axis(throttle.get<InputAxis>().binding, input);
    CHECK(axis_value(world, "module_throttle") == 0.5f);
}
