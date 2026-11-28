// Spike SP-13 (VCH-089), throwaway: walking, hovering and landing in body-fixed frames on a rotating planet
// (docs/research/spikes/sp-13-walk-hover-land.md; ADR 0045, 0058, 0059). Jolt types stay in sp13_sim.cpp.
//
// Frames (ADR 0045): the planet's body-centered inertial frame I (system axes) and its body-fixed rotating frame B,
// whose orientation comes from the universe tick (veach::space::orientation, exact phase, ADR 0069). Physics scenes
// (ADR 0058): one surface scene in B, anchored near its occupants and re-anchored at tick boundaries; one co-moving
// free-flight scene for the craft, in I above the handoff altitude and in B below it, re-centered on the craft
// (position and velocity) whenever its local offsets grow. Every move between frames or scenes happens at a tick
// boundary, converts pose and velocity in double, and is logged as a Transfer with the jump it caused.
#pragma once

#include <veach/core/math.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace veach::sp13 {

using math::dvec3;

inline constexpr double step_seconds = 1.0 / 60.0;

/// An Earth-like planet at body scale s (ADR 0049): radius and GM scale, surface gravity and day length do not.
struct Planet {
    double scale = 1.0;
    double radius = 0.0;                ///< 6,371 km × s
    double surface_gravity = 9.80665;   ///< m/s², never scaled
    double gm = 0.0;                    ///< g R²
    double atmosphere_top = 0.0;        ///< 100 km × √s (s_a = √s_p)
    double handoff_down = 0.0;          ///< free flight moves from I to B below this altitude: the atmosphere top
    double handoff_up = 0.0;            ///< and back from B to I above this one (hysteresis): top × 1.1
    double surface_scene_down = 500.0;  ///< free flight (in B) joins the surface scene below this height
    double surface_scene_up = 750.0;    ///< and leaves it above this one
    std::int64_t spin_period_q = 0;     ///< sidereal day, 2^-16 tick units
    double obliquity = 0.0;             ///< radians
    double omega = 0.0;                 ///< rad/s, about the body's +Y
    dvec3 site{0.0};                    ///< the landing pad, on the sphere, body-fixed (latitude 30°, longitude 0)
    dvec3 site_east{0.0}, site_north{0.0}, site_up{0.0};  ///< the pad's tangent frame, body-fixed
};
[[nodiscard]] Planet make_planet(double scale);

enum class Frame : std::uint8_t { inertial, body_fixed };
enum class Scene : std::uint8_t { surface, flight };

/// Controls in 1/65536 fixed point, as the input path delivers them (veach.input.axis on the 1/65536 grid, VCH-101).
struct Controls {
    std::int32_t lift = 0;      ///< craft: vertical target, fraction of the governor cap, [-1, 1]
    std::int32_t strafe_x = 0;  ///< craft: east target, [-1, 1]
    std::int32_t strafe_z = 0;  ///< craft: north target, [-1, 1]
    std::int32_t engine = 0;    ///< craft: assist on above 0.5, [0, 1]
    std::int32_t walk = 0;      ///< walker: forward speed, fraction of 5 m/s, [0, 1]
    std::int32_t turn = 0;      ///< walker: turn rate, rad/s, [-1, 1] (positive turns left)
    bool operator==(const Controls&) const = default;
};
inline constexpr std::int32_t control_one = 65536;

struct Config {
    double scale = 0.01;
    bool craft = true;
    double craft_altitude = 0.0;  ///< start height above the pad; 0: 1.15 × the atmosphere top (in I)
    double craft_descent =
        -1.0;  ///< start descent speed, m/s; negative: 80 % of the autopilot's braking-curve speed (10 m/s²)
    double craft_east = 20.0;  ///< start surface-relative east speed, m/s
    bool walkers = true;       ///< a base, two crates, an idle character and one that walks
    bool reanchor = true;      ///< re-anchor the surface scene on the walker (false: the control run)
    double reanchor_distance = 128.0;
    bool fictitious = false;       ///< Coriolis and centrifugal forces in body-fixed free flight
    bool always_inertial = false;  ///< the craft never leaves inertial free flight (the free-fall control)
    bool time_steps = false;       ///< measure each step with a steady clock (the bench)
};

/// One move of an entity between frames or scenes, or of a scene's origin, at a tick boundary.
struct Transfer {
    enum class Kind : std::uint8_t {
        inertial_to_body_fixed,  ///< free flight: I -> B, descending through handoff_down
        body_fixed_to_inertial,  ///< free flight: B -> I, climbing through handoff_up
        flight_to_surface,       ///< the craft joins the surface scene
        surface_to_flight,       ///< the craft leaves it
        flight_reorigin,         ///< the free-flight scene re-centered on the craft
        surface_reanchor,        ///< the surface scene's anchor moved
    };
    std::int64_t tick = 0;
    Kind kind = Kind::flight_reorigin;
    std::string_view who;  ///< "craft", "walker", "idler", "crate_awake", "crate_sleepy"
    double dv = 0.0;       ///< |Δv|, m/s: the state read back from the old and the new scene, in the old frame
    double dx = 0.0;       ///< |Δx|, m
    double speed = 0.0;    ///< |v| before the move, in the old frame
    double altitude = 0.0;
};
[[nodiscard]] std::string_view kind_name(Transfer::Kind kind);

/// The craft as its pilot sees it: what the autopilot reads, and what the engine module publishes (Sp13Craft).
struct CraftView {
    std::int64_t tick = 0;
    double altitude = 0.0;                  ///< of the craft's bottom above the sphere
    double ve = 0.0, vn = 0.0, vu = 0.0;    ///< surface-relative velocity: local east, north, up
    double offset_e = 0.0, offset_n = 0.0;  ///< from the pad, in the pad's tangent plane
    double cap = 0.0;                       ///< the travel governor's cap max(v_floor, d/τ) (ADR 0059)
    std::int32_t frame = 0;                 ///< Frame
    std::int32_t scene = 0;                 ///< Scene
};

class Sim {
public:
    explicit Sim(const Config& config);
    ~Sim();
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;
    Sim(Sim&&) = delete;
    Sim& operator=(Sim&&) = delete;

    /// One fixed step from tick t to t + 1: transfers and re-origins at the boundary, controls, gravity, characters,
    /// then each scene's PhysicsSystem::Update.
    void step(const Controls& controls);

    [[nodiscard]] std::int64_t tick() const;
    [[nodiscard]] const Planet& planet() const;
    [[nodiscard]] const Config& config() const;
    [[nodiscard]] CraftView craft() const;
    [[nodiscard]] dvec3 craft_position_inertial() const;
    [[nodiscard]] dvec3 craft_velocity_inertial() const;
    [[nodiscard]] dvec3 craft_position_body() const;
    /// Body-fixed positions: "base", "walker", "idler", "crate_awake", "crate_sleepy".
    [[nodiscard]] dvec3 position_body(std::string_view who) const;
    [[nodiscard]] dvec3 velocity_body(std::string_view who) const;
    [[nodiscard]] bool walker_supported() const;
    [[nodiscard]] dvec3 surface_anchor() const;
    /// Every dynamic body, character and scene origin, as raw bits, in a fixed order.
    [[nodiscard]] std::uint64_t hash() const;
    [[nodiscard]] const std::vector<Transfer>& transfers() const;
    [[nodiscard]] int terrain_chunks() const;
    [[nodiscard]] double last_step_ms() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The landing autopilot: reads a CraftView, returns quantized controls. It stands in for a player with a gamepad;
/// a replay never runs it (the recorded axes replace it). Descend on the braking curve, hover 5 s at 50 m, land,
/// cut the engine; then, if asked, climb back through the handoff altitude.
class Autopilot {
public:
    enum class Phase : std::uint8_t { descend, hover, land, landed, climb, done };
    Autopilot(const Planet& planet, bool climb_after_landing, double walk = 1.0, double turn = 0.025);
    [[nodiscard]] Controls next(const CraftView& view);
    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] double hover_min() const { return hover_min_; }
    [[nodiscard]] double hover_max() const { return hover_max_; }
    [[nodiscard]] double touchdown_speed() const { return touchdown_speed_; }
    [[nodiscard]] double landing_offset() const { return landing_offset_; }

private:
    Planet planet_;
    bool climb_ = false;
    std::int32_t walk_ = 0, turn_ = 0;
    Phase phase_ = Phase::descend;
    int timer_ = 0;
    double hover_min_ = 1e300, hover_max_ = -1e300;
    double touchdown_speed_ = 0.0, landing_offset_ = 0.0;
};

/// The walker's controls with the craft idle: walk at `walk` × 5 m/s, turning at `turn` rad/s.
[[nodiscard]] Controls walk_controls(double walk, double turn);

}  // namespace veach::sp13
