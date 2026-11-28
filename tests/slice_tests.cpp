// The first slice (docs/specs/first-slice/spec.md, VCH-115 closing VCH-091), end to end through the engine: the real
// slice project and its startup script at s = 0.01 and s = 1, flown from orbit to the ground, landed, left on foot
// and dug into, with every control a gamepad event recorded by --record and replayed by --replay on the null RHI.
// The hashes were committed from macOS arm64; Windows x64 CI checks them.

#include "slice.hpp"
#include "support/test_support.hpp"

#include <veach/platform/platform.hpp>
#include <veach/runtime/replay.hpp>

#include <doctest/doctest.h>

#include <format>
#include <ostream>  // doctest prints string_view and string operands (MSVC needs it declared)
#include <memory>
#include <string>
#include <vector>

using namespace veach;

namespace {

struct Expected {
    double scale;
    const char* script;
    std::uint64_t landing;  ///< [slice.AC5]: 2 s after touchdown
    std::uint64_t dig;      ///< [slice.AC6]: standing in the dug hole
};

// Regenerate only on purpose: a different value means the realization, the flight model, Jolt, the terrain or the
// scene changed. The INFO lines of a failing run print the new values.
constexpr Expected expected[] = {
    {0.01, "scripts/slice_s0.01.luau", 0x8163fa461afa85abULL, 0x5420285180e141c7ULL},
    {1.0, "scripts/slice_s1.luau", 0x1ee27804b67dadbfULL, 0x9acac2bc2caeedb8ULL},
};

std::string hex(std::uint64_t v) {
    return std::format("0x{:016x}", v);
}

std::uint64_t join(std::int64_t hi, std::int64_t lo) {
    return (static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo);
}

/// The slice app's own modules on its own project and script, headless on the null RHI.
struct SliceRun {
    SliceRun(const char* script, const std::vector<std::string>& extra) {
        std::vector<std::string> args{"--project", std::string(VEACH_SOURCE_DIR) + "/apps/slice", "--headless",
                                      "--script", script};
        args.insert(args.end(), extra.begin(), extra.end());
        const std::vector<std::string_view> views(args.begin(), args.end());
        auto config = resolve_config({.name = "t", .default_project = {}}, views);
        REQUIRE(config.has_value());
        config->log_to_console = false;
        engine = std::make_unique<Engine>(std::move(*config));
        slice::import_modules(*engine);
        REQUIRE(engine->init().has_value());
        REQUIRE(engine->start().has_value());
    }
    [[nodiscard]] const slice::SliceReadout* readout() const {
        return engine->world().lookup("slice").try_get<slice::SliceReadout>();
    }
    /// The controls as the gamepad would send them: sticks and triggers (through the dead zones), buttons on change.
    void inject(const slice::Controls& c) {
        Platform& platform = engine->services().get<Platform>();
        const auto raw = slice::raw_axes(c);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            platform.inject(
                {.type = PlatformEvent::Type::axis_moved, .axis = slice::axis_actions.at(i).axis, .value = raw.at(i)});
        }
        const auto button = [&](bool down, bool& was, Key key) {
            if (down != was) {
                platform.inject(
                    {.type = down ? PlatformEvent::Type::key_down : PlatformEvent::Type::key_up, .key = key});
                was = down;
            }
        };
        button(c.exit, exit_down, slice::exit_action.pad);
        button(c.dig, dig_down, slice::dig_action.pad);
    }
    test::LogCapture logs;
    std::unique_ptr<Engine> engine;
    bool exit_down = false, dig_down = false;
};

void fly_record_replay(const Expected& e) {
    INFO("s = " << e.scale);
    const test::TempProject out("slice-replay");
    const std::string recording = (out.path() / "slice.vreplay").string();
    std::uint64_t landing = 0, dig = 0;
    std::int64_t frames = 0;
    slice::Autopilot pilot;
    {
        SliceRun run(e.script, {"--record", recording, "--frames", "40000"});
        slice::Controls c;  // the first step runs before there is a readout
        bool landed_shown = false, cruise = false, assisted = false, surface = false;
        double min_depth = 0.0;
        while (pilot.phase() != slice::Autopilot::Phase::done) {
            run.inject(c);
            REQUIRE_MESSAGE(run.engine->frame(), "stopped in " << slice::Autopilot::name(pilot.phase()));
            const slice::SliceReadout* r = run.readout();
            REQUIRE(r != nullptr);
            cruise = cruise || r->mode == "cruise";
            assisted = assisted || (r->mode == "assisted" && r->frame == "body_fixed");
            surface = surface || r->scene == "surface";
            landed_shown = landed_shown || r->landed;
            min_depth = std::max(min_depth, r->depth);
            c = pilot.next(*r);
            ++frames;
        }
        const slice::SliceReadout& r = *run.readout();
        landing = join(r.landing_hi, r.landing_lo);
        dig = join(r.hash_hi, r.hash_lo);
        // [slice.AC5] Down from orbit through the cruise drive, the atmosphere-top handoff and the surface scene,
        // hovering at 50 m and landing softly near the site, with no loading screen: every frame ran one step.
        CHECK(cruise);
        CHECK(assisted);
        CHECK(surface);
        CHECK(landed_shown);
        CHECK(pilot.hover_min > 45.0);
        CHECK(pilot.hover_max < 55.0);
        CHECK(pilot.touchdown_speed < 1.5);
        CHECK(pilot.landing_offset < 10.0);
        CHECK(r.step == frames);
        // [slice.AC6] Out of the craft, three digs, standing in the hole: the colliders were swapped under the pilot.
        CHECK(r.mode == "on_foot");
        CHECK(r.digs == 3);
        CHECK(min_depth > 2.0);
        CHECK(r.depth > 2.0);
        // [slice.AC3] The readout's local solar time is a real clock time where the pilot stands.
        CHECK(r.local_solar_time >= 0.0);
        CHECK(r.local_solar_time < 24.0);
        run.engine->shutdown();
        CHECK_MESSAGE(run.logs.errors().empty(), run.logs.errors());
    }
    auto log = read_replay(recording);
    REQUIRE(log.has_value());
    CHECK(log->frames.size() == static_cast<std::size_t>(frames));
    {
        SliceRun replay(e.script, {"--replay", recording});
        for (std::int64_t i = 0; i < frames; ++i) {
            static_cast<void>(replay.engine->frame());
        }
        const slice::SliceReadout* r = replay.readout();
        REQUIRE(r != nullptr);
        CHECK(join(r->landing_hi, r->landing_lo) == landing);
        CHECK(join(r->hash_hi, r->hash_lo) == dig);
        replay.engine->shutdown();
        CHECK_FALSE(replay.logs.contains("replay.diverged"));
        CHECK_MESSAGE(replay.logs.errors().empty(), replay.logs.errors());
    }
    INFO("landing hash " << hex(landing) << ", dig hash " << hex(dig));
    CHECK(landing == e.landing);
    CHECK(dig == e.dig);
}

}  // namespace

TEST_CASE("[slice.AC5] [slice.AC6] the slice at s = 0.01: orbit to a dug hole, recorded and replayed") {
    fly_record_replay(expected[0]);
}

TEST_CASE("[slice.AC5] [slice.AC6] the slice at s = 1: orbit to a dug hole, recorded and replayed") {
    fly_record_replay(expected[1]);
}
