#pragma once

// veach: canonical
// Bodies on rails (docs/adr/0048-bodies-on-rails.md): a tree of nodes, each moved relative to its parent by a
// registered motion model, evaluated as a pure function of the universe tick in double. Spike SP-04 (VCH-080).
//
// The system frame follows the engine's conventions: right-handed, +Y is the reference plane's north (the ecliptic
// pole for Sol), +X the reference direction (the J2000 equinox for Sol), so ecliptic (x, y, z) is system (x, z, −y).
// Prograde motion turns +X toward −Z. Meters and seconds.
//
// Canonical: only veach::dmath and IEEE + − × ÷ √ (ADR 0043, 0067), fixed iteration counts.

#include <veach/core/math.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach::space {

/// Keplerian elements relative to the parent, at the node's epoch. Meters and radians.
struct Elements {
    double a = 0.0;              ///< semi-major axis
    double e = 0.0;              ///< eccentricity, [0, 0.9999]
    double i = 0.0;              ///< inclination to the reference plane
    double node = 0.0;           ///< longitude of the ascending node, Ω
    double arg_periapsis = 0.0;  ///< argument of periapsis, ω
    double mean_anomaly = 0.0;   ///< M at the epoch (kepler_elliptic); mean longitude L (kepler_secular)
};

/// Linear drift per Julian century of a, e, i, Ω and the longitude of periapsis ϖ = Ω + ω: JPL's approximate
/// ephemeris ("Approximate Positions of the Planets", Standish). The mean longitude advances with period_q.
/// Outside [t_min, t_max] centuries from the epoch the elements stop drifting (the table's validity interval).
struct SecularRates {
    double a = 0.0;
    double e = 0.0;
    double i = 0.0;
    double node = 0.0;
    double longitude_periapsis = 0.0;
    double t_min = 0.0;
    double t_max = 0.0;
    double century_seconds = 36525.0 * 86400.0;  ///< × √s when the system is scaled (ADR 0049)
};

/// Rotation about the body's own +Y (its north pole) (ADR 0048 `Spin`). No precession.
struct Spin {
    std::int64_t period_q = 0;  ///< sidereal period; 0: no rotation
    std::int64_t epoch_tick = 0;
    double phase0 = 0.0;          ///< radians: the prime meridian's angle at the epoch
    double obliquity = 0.0;       ///< radians: the pole's tilt from the reference plane's north
    double pole_longitude = 0.0;  ///< radians: the direction the pole tilts toward, measured like orbital longitudes
};

/// Position and velocity, in the parent's frame (from a model) or the system frame (from System::evaluate).
struct Pose {
    math::dvec3 position{0.0};
    math::dvec3 velocity{0.0};
};

struct OrbitNode;
using MotionFn = Pose (*)(const OrbitNode& node, std::int64_t tick);

/// A motion model, registered by stable string id (`space:kepler_elliptic`). Fixed cost per evaluation.
struct MotionModel {
    std::string_view id;
    MotionFn evaluate = nullptr;
};

/// The built-in models: `space:fixed`, `space:kepler_elliptic`, `space:kepler_secular`.
[[nodiscard]] std::span<const MotionModel> builtin_motion_models();
[[nodiscard]] const MotionModel* find_motion_model(std::string_view id);

struct OrbitNode {
    std::string name;
    std::int32_t parent = -1;  ///< index of an earlier node; −1 for the root
    const MotionModel* model = nullptr;
    Elements elements;
    SecularRates rates;           ///< kepler_secular only
    std::int64_t period_q = 0;    ///< orbital period (mean longitude for kepler_secular), stored, not derived
    std::int64_t epoch_tick = 0;  ///< the elements' epoch
    /// Barycentric split (ADR 0048's binary_pair): the model gives the secondary's orbit relative to the primary;
    /// the primary carries −m₂/(m₁+m₂), the secondary m₁/(m₁+m₂). 1 for an ordinary child.
    double mass_fraction = 1.0;
    double gm = 0.0;      ///< gameplay GM, m³/s² (s² × physical, ADR 0049)
    double radius = 0.0;  ///< m
    Spin spin;
    // Derived at System::add from fixed elements: the perifocal axes in the parent's frame.
    math::dvec3 p_axis{1.0, 0.0, 0.0};
    math::dvec3 q_axis{0.0, 0.0, -1.0};
};

/// Solves M = E − e sin E for E (radians), M in [−π, π], with a fixed iteration count: the starter is M + e sin M
/// for e ≤ 0.8 and the cubic min(π, ∛(6|M|)) above, then 6 Newton steps. Converges to the double's conditioning
/// limit for every e ≤ 0.9999 (tests/space_tests.cpp).
[[nodiscard]] double solve_kepler(double mean_anomaly, double e);
inline constexpr int kepler_iterations = 6;

/// The perifocal axes P (toward periapsis) and Q (90° ahead) in the reference frame, from i, Ω, ω.
void perifocal_axes(double i, double node, double arg_periapsis, math::dvec3& p, math::dvec3& q);

/// A tree of nodes in parent-before-child order.
class System {
public:
    System() = default;
    explicit System(std::string name) : name_(std::move(name)) {}

    /// Appends a node; its parent must already be in the system. Returns its index. `model` must be registered.
    std::int32_t add(OrbitNode node, std::string_view model);

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] std::span<const OrbitNode> nodes() const { return nodes_; }
    [[nodiscard]] const OrbitNode& node(std::int32_t index) const { return nodes_[static_cast<std::size_t>(index)]; }
    /// The node's index, or −1.
    [[nodiscard]] std::int32_t find(std::string_view name) const;

    /// Every node's pose in the system frame at `tick`; `out` has one entry per node.
    void evaluate(std::int64_t tick, std::span<Pose> out) const;
    /// One node's pose in the system frame (walks its ancestors).
    [[nodiscard]] Pose evaluate(std::int32_t index, std::int64_t tick) const;

private:
    std::string name_;
    std::vector<OrbitNode> nodes_;
};

/// The body's orientation: its own axes (X: prime meridian, Y: north pole) expressed in the system frame.
struct Orientation {
    math::dvec3 x{1.0, 0.0, 0.0};
    math::dvec3 y{0.0, 1.0, 0.0};
    math::dvec3 z{0.0, 0.0, 1.0};
    [[nodiscard]] math::dvec3 to_system(math::dvec3 v) const { return x * v.x + y * v.y + z * v.z; }
    [[nodiscard]] math::dvec3 to_body(math::dvec3 v) const {
        return {math::dot(x, v), math::dot(y, v), math::dot(z, v)};
    }
};

/// Pole rotation × R_y(phase0 + 2π × spin phase(tick)), ADR 0048.
[[nodiscard]] Orientation orientation(const Spin& spin, std::int64_t tick);

/// The sidereal period from a solar day and the orbital period, prograde: 1/P_sid = 1/P_sol + 1/P_orb.
[[nodiscard]] double sidereal_from_solar(double solar_day_seconds, double orbit_seconds);

}  // namespace veach::space
