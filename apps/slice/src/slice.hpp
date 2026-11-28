#pragma once

// The first slice (docs/specs/first-slice/spec.md, VCH-115 closing VCH-091): spawn in orbit of a seed-generated
// planet, fly a craft down through the atmosphere-top handoff into the surface physics scene without a loading
// screen, land, get out as a character and dig a hole, with the same code at any scale. The universe (frames on
// rails, terrain, gravity, sky) is space::UniverseModule's; this module is the game on top of it: the craft, the
// character, the camera, the dig, the logged readout.
//
// A startup script sets the Universe (seed, system, scale) on an entity, and optionally Slice on another:
//
//   veach.spawn("universe"):set(c.Universe, { seed = "42", system = "S:1000400100008000", scale = 0.01 })
//
// Controls are analog actions on the gamepad (and keys), read each fixed step, so a run records and replays through
// --record/--replay. The Autopilot below stands in for a player: the tests inject its controls as gamepad events,
// and VEACH_SLICE_AUTOPILOT=1 flies it inside the app (with VEACH_SLICE_SHOTS=dir it takes the milestone
// screenshots and logs frame times).

#include <veach/veach.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace slice {

/// Per-run options. Scriptable.
struct Slice {
    double orbit_altitude = 0.0;  ///< the craft starts this high over the landing site, m; 0: one radius
    bool autopilot = false;       ///< fly itself (the demo); VEACH_SLICE_AUTOPILOT=1 turns it on too
};

/// The logged readout, published every fixed step on the "slice" entity (Scriptable): what the player would see on
/// a HUD, and what the autopilot reads. Body-fixed quantities are relative to the surface.
struct SliceReadout {
    std::int64_t tick = 0;                              ///< universe tick, end of the step
    std::int64_t step = 0;                              ///< fixed steps since the craft spawned
    std::string mode;                                   ///< coasting, cruise, assisted, landed, on_foot
    std::string frame;                                  ///< the subject's: inertial or body_fixed
    std::string scene;                                  ///< free_flight or surface
    double scale = 0.0;                                 ///< s_p
    double altitude = 0.0;                              ///< above the body's radius, m
    double height = 0.0;                                ///< above the ground (the field), m
    double speed = 0.0;                                 ///< surface-relative, m/s
    double cap = 0.0;                                   ///< the governor's cap (cruise: the cruise limit), m/s
    double top = 0.0;                                   ///< the atmosphere top above the radius, m
    double v_up = 0.0, v_right = 0.0, v_forward = 0.0;  ///< surface-relative velocity in the subject's heading
    double to_right = 0.0, to_forward = 0.0;            ///< the landing site from the subject, in its tangent plane
    double latitude = 0.0, longitude = 0.0;             ///< degrees
    double local_solar_time = 0.0;                      ///< hours
    double sun_elevation = 0.0;                         ///< degrees
    bool engine = false;
    bool landed = false;
    double walked = 0.0;     ///< on foot: m from where the character got out
    double depth = 0.0;      ///< on foot: the feet below the undug ground there, m
    std::uint32_t digs = 0;  ///< brush ops applied
    std::uint32_t colliders = 0;
    std::int64_t hash_hi = 0, hash_lo = 0;        ///< the canonical state hash, split for JSON and Luau
    std::int64_t landing_hi = 0, landing_lo = 0;  ///< the hash 2 s after touchdown (0 before)
};

/// Controls in 1/65536 fixed point, as the input path delivers them.
struct Controls {
    std::int32_t lift = 0;     ///< [-1, 1]: up, fraction of the cap
    std::int32_t right = 0;    ///< [-1, 1]: strafe right (on foot: unused)
    std::int32_t forward = 0;  ///< [-1, 1]: ahead (on foot: walk)
    std::int32_t yaw = 0;      ///< [-1, 1]: turn left
    std::int32_t engine = 0;   ///< [0, 1]: thrusters on above 0.5
    bool exit = false;         ///< get out of the landed craft
    bool dig = false;          ///< dig in front of the character
    bool operator==(const Controls&) const = default;
};

/// The analog actions in Controls order and their gamepad axes and keys, with dead zones.
struct AxisAction {
    std::string_view name;
    veach::Axis axis;
    veach::Key negative, positive;
    float dead_zone;
};
inline constexpr std::array<AxisAction, 5> axis_actions{{
    {"slice_lift", veach::Axis::gamepad_right_y, veach::Key::f, veach::Key::r, 0.1f},
    {"slice_right", veach::Axis::gamepad_left_x, veach::Key::a, veach::Key::d, 0.1f},
    {"slice_forward", veach::Axis::gamepad_left_y, veach::Key::s, veach::Key::w, 0.1f},
    {"slice_yaw", veach::Axis::gamepad_right_x, veach::Key::e, veach::Key::q, 0.1f},
    {"slice_engine", veach::Axis::gamepad_right_trigger, veach::Key::unknown, veach::Key::space, 0.05f},
}};
struct ButtonAction {
    std::string_view name;
    veach::Key pad, key;
};
inline constexpr ButtonAction exit_action{"slice_exit", veach::Key::gamepad_south, veach::Key::x};
inline constexpr ButtonAction dig_action{"slice_dig", veach::Key::gamepad_west, veach::Key::g};

/// The controls as raw gamepad values: the inverse of each axis's dead zone, so the axis reads `c` back.
[[nodiscard]] std::array<std::int32_t, 5> raw_axes(const Controls& c);

/// A stand-in for the player (canonical: + − × ÷ √ only): descend from orbit over the landing site, hover 5 s at
/// 50 m, land, wait 2 s, get out, walk 8 m, dig three times and step into the hole. Reads the readout only.
class Autopilot {
public:
    enum class Phase : std::uint8_t { coast, descend, hover, land, landed, exit, walk, dig, step_in, done };
    [[nodiscard]] Controls next(const SliceReadout& r);
    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] static std::string_view name(Phase p);
    double hover_min = 1e300, hover_max = -1e300, touchdown_speed = 0.0, landing_offset = 0.0;
    double walked_at_dig = 0.0;

private:
    Phase phase_ = Phase::coast;
    int timer_ = 0;
};

struct SliceModule {
    explicit SliceModule(veach::World& world);
};

/// Imports what the slice runs on, in order: PhysicsModule, TerrainModule, space's UniverseModule and SpaceMcpModule,
/// then SliceModule (veach-slice and the tests both use it).
void import_modules(veach::Engine& engine);

}  // namespace slice
