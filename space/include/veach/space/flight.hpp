#pragma once

// veach: canonical
// Flight policy (ADR 0059, 0080; docs/specs/first-slice/spec.md): the travel governor, the handoff bands between
// frames and physics scenes, and the core flight model (assisted Newtonian with an in-system cruise drive). Pure
// functions of their inputs in + − × ÷ √ and veach::dmath, so a craft flown by the same controls flies the same bits
// everywhere. Meters and seconds, never scaled: the same at every s (ADR 0049).
//
// Every vector here is in the body-fixed frame of the body flown over (ADR 0080: the assist works on the
// surface-relative velocity in every frame, so its reference never changes at a handoff). Horizontal axes come from
// the craft's own heading, parallel-transported, never from the spin axis: east and north are 0/0 at a pole (the
// seams sweep's finding).

#include <veach/core/math.hpp>
#include <veach/space/realization.hpp>

#include <cstdint>

namespace veach::space {

/// ADR 0059's cap law: |v| ≤ max(v_floor, d / τ), d the height above the ground.
struct Governor {
    double tau_near = 5.0;    ///< s: descent and atmospheric flight
    double tau_cruise = 7.0;  ///< s: the cruise drive
    double v_floor = 100.0;   ///< m/s: below d = v_floor · τ the floor rules
    [[nodiscard]] double cap(double height) const;
    [[nodiscard]] double cruise_cap(double height) const;
};

/// Where a craft changes frame or physics scene (ADR 0080 decision 3). Altitudes are above the body's radius; the
/// surface bands are above the ground. Gaps between each pair are hysteresis.
struct HandoffBands {
    double body_fixed_below = 0.0;  ///< inertial → body-fixed: the atmosphere top (100 km × s_a for airless bodies)
    double inertial_above = 0.0;    ///< body-fixed → inertial: 1.1 × that
    double surface_below = 500.0;   ///< free flight → the surface physics scene
    double surface_above = 750.0;   ///< and back
    /// Until continuous collision detection exists, nothing enters the surface scene faster than this; a faster craft
    /// is slowed to it at the band (ADR 0080's consequence: SP-13 lost a body entering at 425 m/s).
    double surface_entry_speed = 150.0;

    [[nodiscard]] static HandoffBands of(const RealizedBody& body, double s_a);
};

enum class FlightFrame : std::uint8_t { inertial, body_fixed };
enum class FlightScene : std::uint8_t { free_flight, surface };

/// The move a craft makes at this tick boundary, if any.
enum class Handoff : std::uint8_t { none, to_body_fixed, to_inertial, to_surface, to_free_flight };

/// Altitude above the radius, height above the ground.
[[nodiscard]] Handoff decide_handoff(const HandoffBands& bands, FlightFrame frame, FlightScene scene, double altitude,
                                     double height);

/// A craft's heading: its up (follows −ĝ with a time constant) and its forward, kept tangent by projection after
/// every change of up (parallel transport, no north). Body-fixed axes.
struct Heading {
    math::dvec3 up{0.0, 1.0, 0.0};
    math::dvec3 forward{0.0, 0.0, -1.0};
    [[nodiscard]] math::dvec3 right() const;
    /// The rotation whose +Y is up and whose −Z is forward.
    [[nodiscard]] math::dquat rotation() const;
};
/// Moves up toward `target_up` by `alpha` (renormalized, no trigonometry), re-projects forward, then turns it about up
/// by `yaw` radians (positive turns left).
[[nodiscard]] Heading transport(const Heading& h, const math::dvec3& target_up, double alpha, double yaw);
/// 1 − exp(−dt / τ): the per-step fraction of a first-order filter.
[[nodiscard]] double filter_alpha(double dt, double tau);

/// Stick input, already de-quantized: lift, right and forward in [-1, 1] (fractions of the cap), yaw in [-1, 1]
/// (fraction of the yaw rate), engine in [0, 1] (on above 0.5).
struct FlightControls {
    double lift = 0.0;
    double right = 0.0;
    double forward = 0.0;
    double yaw = 0.0;
    double engine = 0.0;
};

struct CraftParams {
    double max_acceleration = 40.0;  ///< m/s²: the assist's thruster authority (about 4 g)
    double assist_tau = 0.5;         ///< s: how fast the assist closes on its target velocity
    double yaw_rate = 0.6;           ///< rad/s at full stick
    /// The cruise drive hands over to the thrusters at the atmosphere top at the speed they can brake from with this
    /// deceleration over the atmosphere's depth: v = √(2 a h_top).
    double entry_braking = 20.0;
};

struct FlightState {
    math::dvec3 position{0.0};  ///< body-fixed, from the body's center
    math::dvec3 velocity{0.0};  ///< relative to the surface (body-fixed)
    Heading heading;
    double altitude = 0.0;  ///< above the radius
    double height = 0.0;    ///< above the ground
    math::dvec3 gravity{0.0};
};

struct FlightCommand {
    bool engine = false;
    bool cruise = false;            ///< the kinematic cruise drive: `velocity` replaces the craft's velocity
    math::dvec3 velocity{0.0};      ///< cruise: the surface-relative velocity to fly at
    math::dvec3 acceleration{0.0};  ///< assisted: thrust per unit mass, body-fixed (gravity comes from the hook)
    double cap = 0.0;               ///< the governor's cap this step
};

/// The cruise drive's speed limit at `altitude`: under the governor's cruise cap, and falling to the thrusters'
/// entry speed at the atmosphere top (`top`, above the radius).
[[nodiscard]] double cruise_limit(const Governor& g, const CraftParams& p, double altitude, double height, double top);

/// ADR 0059's assisted Newtonian model with the cruise drive above the atmosphere top (`top`): stick input becomes
/// a target velocity within the cap; cruise flies at it, the thrusters close on it within their authority and
/// counter gravity. Engine off: nothing.
[[nodiscard]] FlightCommand fly(const Governor& g, const CraftParams& p, const FlightControls& c, const FlightState& s,
                                double top);

}  // namespace veach::space
