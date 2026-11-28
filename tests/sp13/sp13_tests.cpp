// Spike SP-13 (VCH-089): walking, hovering and landing on a rotating planet at s = 0.01 and s = 1, through frame and
// scene handoffs, recorded and replayed through the engine's input path (docs/research/spikes/sp-13-walk-hover-land.md,
// ADR 0045, 0058, 0059). The hashes below were committed from macOS arm64; Windows x64 CI checks them.

#include "sp13/sp13_module.hpp"
#include "sp13/sp13_sim.hpp"
#include "support/test_support.hpp"

#include <veach/platform/platform.hpp>
#include <veach/runtime/replay.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace veach;
using namespace veach::sp13;

namespace {

struct Expected {
    double scale;
    std::uint64_t walk;     ///< [sp13.AC1]: after 2 minutes of walking
    std::uint64_t landing;  ///< [sp13.AC3]: when the landed craft's engine has been off for 2 s
};

// Regenerate only on purpose: a different value means Jolt, its build flags or the scene changed. The INFO lines of
// a failing run print the new values.
constexpr Expected expected[] = {
    {0.01, 0x16e318bdcca0b65bULL, 0x9e25df28fd4d4a02ULL},
    {1.0, 0xd9d5140a4fbe07bdULL, 0x5fff33f2b5008adaULL},
};

std::string hex(std::uint64_t v) {
    return std::format("0x{:016x}", v);
}

/// A landing flown by the autopilot directly on a Sim: the reference for the engine-driven run.
struct Flight {
    std::uint64_t hash = 0;
    std::int64_t ticks = 0;
    std::vector<Transfer> transfers;
    double hover_min = 0.0, hover_max = 0.0, touchdown = 0.0, offset = 0.0;
};

Flight fly(double scale, bool climb, bool walkers) {
    Sim sim(Config{.scale = scale, .walkers = walkers});
    Autopilot pilot(sim.planet(), climb);
    Controls c;
    for (int i = 0; i < 60 * 600 && pilot.phase() != Autopilot::Phase::done; ++i) {
        sim.step(c);
        c = pilot.next(sim.craft());
    }
    REQUIRE(pilot.phase() == Autopilot::Phase::done);
    return {sim.hash(),
            sim.tick(),
            sim.transfers(),
            pilot.hover_min(),
            pilot.hover_max(),
            pilot.touchdown_speed(),
            pilot.landing_offset()};
}

}  // namespace

TEST_CASE("[sp13.AC1] a base stays put and characters walk on a rotating planet, through surface re-anchors") {
    for (const Expected& e : expected) {
        INFO("s = " << e.scale);
        const Controls walk = walk_controls(1.0, 0.025);
        Sim sim(Config{.scale = e.scale, .craft = false});
        Sim control(Config{.scale = e.scale, .craft = false, .reanchor = false});
        for (int i = 0; i < 300; ++i) {  // characters start 2 cm, crates 1 cm above the ground
            sim.step(walk);
            control.step(walk);
        }
        const dvec3 base = sim.position_body("base");
        const dvec3 idler = sim.position_body("idler");
        const dvec3 crate_sleepy = sim.position_body("crate_sleepy");
        const dvec3 crate_awake = sim.position_body("crate_awake");
        double reanchor_diff = 0.0;
        double min_height = 1e300, max_height = -1e300;
        bool always_supported = true;
        for (int i = 0; i < 60 * 120; ++i) {
            sim.step(walk);
            control.step(walk);
            reanchor_diff =
                std::max(reanchor_diff, math::length(sim.position_body("walker") - control.position_body("walker")));
            const double height = math::length(sim.position_body("walker")) - sim.planet().radius;
            min_height = std::min(min_height, height);
            max_height = std::max(max_height, height);
            always_supported = always_supported && sim.walker_supported();
        }
        // Nothing placed in the body-fixed frame moves: not the static base, not a sleeping or an always-awake
        // crate, not a character standing still (2 minutes here; 24 hours in the findings).
        CHECK(math::length(sim.position_body("base") - base) == 0.0);
        CHECK(math::length(sim.position_body("idler") - idler) < 1e-9);
        CHECK(math::length(sim.position_body("crate_sleepy") - crate_sleepy) < 1e-9);
        CHECK(math::length(sim.position_body("crate_awake") - crate_awake) < 1e-6);
        // The walker walks on the curved surface with up = −ĝ, on the ground every step, 600 m around the pad.
        CHECK(always_supported);
        CHECK(min_height > -0.01);
        CHECK(max_height < 0.05);
        CHECK(math::length(sim.position_body("walker") - sim.planet().site) == doctest::Approx(200.0).epsilon(0.01));
        // Re-anchoring the surface scene changes nothing: the walker follows the same path as without it.
        const auto reanchors = std::count_if(sim.transfers().begin(), sim.transfers().end(), [](const Transfer& t) {
            return t.kind == Transfer::Kind::surface_reanchor && t.who == "walker";
        });
        CHECK(reanchors >= 3);
        CHECK(reanchor_diff < 1e-9);
        for (const Transfer& t : sim.transfers()) {
            CHECK(t.dv < 1e-9);
            CHECK(t.dx < 1e-9);
        }
        INFO("walk hash " << hex(sim.hash()));
        CHECK(sim.hash() == e.walk);
    }
}

TEST_CASE("[sp13.AC2] |dv| stays under 1 mm/s through every frame and scene handoff, down and back up") {
    for (const Expected& e : expected) {
        INFO("s = " << e.scale);
        const Flight f = fly(e.scale, true, false);
        int kinds[6] = {};
        double max_dv = 0.0;
        for (const Transfer& t : f.transfers) {
            ++kinds[static_cast<int>(t.kind)];
            max_dv = std::max(max_dv, t.dv);
            CHECK(t.dx < 1e-6);
        }
        INFO("max |dv| " << max_dv);
        CHECK(max_dv < 1e-3);
        CHECK(kinds[static_cast<int>(Transfer::Kind::inertial_to_body_fixed)] == 1);
        CHECK(kinds[static_cast<int>(Transfer::Kind::flight_to_surface)] == 1);
        CHECK(kinds[static_cast<int>(Transfer::Kind::surface_to_flight)] == 1);
        CHECK(kinds[static_cast<int>(Transfer::Kind::body_fixed_to_inertial)] == 1);
        CHECK(kinds[static_cast<int>(Transfer::Kind::flight_reorigin)] > 10);
        // It hovered at 50 m and landed softly on the pad.
        CHECK(f.hover_min > 48.0);
        CHECK(f.hover_max < 52.0);
        CHECK(f.touchdown < 1.0);
        CHECK(f.offset < 1.0);
    }

    // An unpowered fall through the handoff altitude follows the same inertial trajectory as a control that never
    // leaves the inertial frame, once body-fixed free flight applies Coriolis and centrifugal forces. Without them
    // (ADR 0058's rule, right for the surface scene) the trajectory bends by metres.
    for (const Expected& e : expected) {
        INFO("s = " << e.scale);
        const int steps = e.scale < 0.1 ? 40 * 60 : 90 * 60;  // stays well above the ground
        Config with{.scale = e.scale, .craft_descent = 0.0, .craft_east = 0.0, .walkers = false, .fictitious = true};
        Config without = with;
        without.fictitious = false;
        Config inertial = with;
        inertial.always_inertial = true;
        Sim a(with), b(without), control(inertial);
        double dv_with = 0.0, dv_without = 0.0;
        for (int i = 0; i < steps; ++i) {
            a.step({});
            b.step({});
            control.step({});
            dv_with = std::max(dv_with, math::length(a.craft_velocity_inertial() - control.craft_velocity_inertial()));
            dv_without =
                std::max(dv_without, math::length(b.craft_velocity_inertial() - control.craft_velocity_inertial()));
        }
        REQUIRE(a.craft().frame == static_cast<std::int32_t>(Frame::body_fixed));
        INFO("with " << dv_with << " m/s, without " << dv_without << " m/s");
        CHECK(dv_with < 1e-3);
        CHECK(dv_without > 0.1);
    }
}

namespace {

/// An engine running the SP-13 module on a startup script that builds the scene at `scale`.
struct EngineRun {
    EngineRun(double scale, const std::vector<std::string>& extra) : project("sp13") {
        project.write("veach.project.json", R"({"name": "sp13"})");
        project.write("main.luau", std::format(R"(--!strict
local veach = require("@veach")
veach.spawn("sp13"):set(veach.components.Sp13Run, {{ scale = {}, craft = true, walkers = true }})
)",
                                               scale));
        std::vector<std::string> args{"--project", project.path().string(), "--headless", "--script", "main.luau"};
        args.insert(args.end(), extra.begin(), extra.end());
        const std::vector<std::string_view> views(args.begin(), args.end());
        auto config = resolve_config({.name = "t", .default_project = {}}, views);
        REQUIRE(config.has_value());
        config->log_to_console = false;
        engine = std::make_unique<Engine>(std::move(*config));
        engine->import<Sp13Module>();
        REQUIRE(engine->init().has_value());
        REQUIRE(engine->start().has_value());
    }
    [[nodiscard]] const Sp13Craft* craft() const { return engine->world().lookup("sp13").try_get<Sp13Craft>(); }
    void inject(const Controls& c) const {
        const auto values = control_values(c);
        for (std::size_t i = 0; i < values.size(); ++i) {
            engine->services().get<Platform>().inject(
                {.type = PlatformEvent::Type::axis_moved, .axis = action_axes[i], .value = values[i]});
        }
    }
    test::LogCapture logs;
    test::TempProject project;
    std::unique_ptr<Engine> engine;
};

}  // namespace

namespace {

/// Flies the landing through the engine with --record, injecting the autopilot's controls as gamepad axes, then
/// replays the recording headless: every frame's state hash must match (replay.diverged otherwise).
void record_and_replay(const Expected& e) {
    {
        INFO("s = " << e.scale);
        const Flight direct = fly(e.scale, false, true);

        const test::TempProject out("sp13-replay");
        const std::string recording = (out.path() / "landing.vreplay").string();
        const std::string frames = std::to_string(direct.ticks + 10);
        std::uint64_t recorded = 0;
        std::int64_t ticks = 0;
        {
            EngineRun run(e.scale, {"--record", recording, "--frames", frames});
            std::optional<Autopilot> pilot;
            Controls c;  // the first step runs before the craft exists to read
            while (pilot == std::nullopt || pilot->phase() != Autopilot::Phase::done) {
                run.inject(c);
                REQUIRE_MESSAGE(run.engine->frame(), "stopped at tick " << (run.craft() ? run.craft()->tick : -1));
                const Sp13Craft* craft = run.craft();
                REQUIRE(craft != nullptr);
                if (!pilot) {
                    pilot.emplace(make_planet(e.scale), false);
                }
                c = pilot->next(view_of(*craft));
            }
            recorded = hash_of(*run.craft());
            ticks = run.craft()->tick;
            run.engine->shutdown();
            CHECK_MESSAGE(run.logs.errors().empty(), run.logs.errors());
        }
        // The engine flew exactly what the direct run flew: same controls, same steps, same bits.
        CHECK(ticks == direct.ticks);
        CHECK(recorded == direct.hash);

        auto log = read_replay(recording);
        REQUIRE(log.has_value());
        CHECK(log->frames.size() == static_cast<std::size_t>(ticks));
        {
            EngineRun replay(e.scale, {"--replay", recording});
            for (std::int64_t i = 0; i < ticks; ++i) {
                static_cast<void>(replay.engine->frame());
            }
            const Sp13Craft* craft = replay.craft();
            REQUIRE(craft != nullptr);
            CHECK(craft->tick == ticks);
            CHECK(hash_of(*craft) == recorded);
            replay.engine->shutdown();
            CHECK_FALSE(replay.logs.contains("replay.diverged"));
            CHECK_MESSAGE(replay.logs.errors().empty(), replay.logs.errors());
        }
        INFO("landing hash " << hex(recorded));
        CHECK(recorded == e.landing);
    }
}

}  // namespace

TEST_CASE("[sp13.AC3] a landing at s = 0.01 recorded through the input path replays to the same state hash") {
    record_and_replay(expected[0]);
}

TEST_CASE("[sp13.AC3] a landing at s = 1 recorded through the input path replays to the same state hash") {
    record_and_replay(expected[1]);
}
