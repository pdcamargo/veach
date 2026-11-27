// veach: canonical (ADR 0043): veach::dmath and IEEE + − × ÷ √ only, fixed iteration counts.
#include <veach/space/clock.hpp>
#include <veach/space/orbit.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/dmath.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace veach::space {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double two_pi = 2.0 * pi;

/// Ecliptic-style (x, y, z), z north, to the system frame (x, z, −y).
math::dvec3 to_frame(double x, double y, double z) {
    return {x, z, -y};
}

/// M wrapped to [−π, π).
double wrap_pi(double angle) {
    return angle - two_pi * std::floor((angle + pi) / two_pi);
}

/// The node's orbit at mean anomaly M and mean motion n (rad/s) on the given perifocal axes.
Pose kepler_pose(double a, double e, double mean_anomaly, double n, const math::dvec3& p, const math::dvec3& q) {
    const double big_e = solve_kepler(wrap_pi(mean_anomaly), e);
    const double cos_e = dmath::cos(big_e);
    const double sin_e = dmath::sin(big_e);
    const double b = a * std::sqrt(1.0 - e * e);
    const double e_dot = n / (1.0 - e * cos_e);
    const double x = a * (cos_e - e);
    const double y = b * sin_e;
    const double vx = -a * sin_e * e_dot;
    const double vy = b * cos_e * e_dot;
    return {p * x + q * y, p * vx + q * vy};
}

double mean_motion(std::int64_t period_q) {
    return two_pi / seconds_from_period_q(period_q);
}

Pose model_fixed(const OrbitNode&, std::int64_t) {
    return {};
}

Pose model_kepler_elliptic(const OrbitNode& node, std::int64_t tick) {
    const Elements& el = node.elements;
    const double m = el.mean_anomaly + two_pi * phase(tick, node.epoch_tick, node.period_q);
    return kepler_pose(el.a, el.e, m, mean_motion(node.period_q), node.p_axis, node.q_axis);
}

Pose model_kepler_secular(const OrbitNode& node, std::int64_t tick) {
    const Elements& el = node.elements;
    const SecularRates& r = node.rates;
    const double t_exact =
        static_cast<double>(tick - node.epoch_tick) * (1.0 / static_cast<double>(ticks_per_second)) / r.century_seconds;
    const double t = std::clamp(t_exact, r.t_min, r.t_max);
    const double a = el.a + r.a * t;
    const double e = el.e + r.e * t;
    const double i = el.i + r.i * t;
    const double node_longitude = el.node + r.node * t;
    const double longitude_periapsis = el.node + el.arg_periapsis + r.longitude_periapsis * t;
    const double mean_longitude = el.mean_anomaly + two_pi * phase(tick, node.epoch_tick, node.period_q);
    math::dvec3 p, q;
    perifocal_axes(i, node_longitude, longitude_periapsis - node_longitude, p, q);
    return kepler_pose(a, e, mean_longitude - longitude_periapsis, mean_motion(node.period_q), p, q);
}

constexpr std::array<MotionModel, 3> builtin_models{{
    {"space:fixed", model_fixed},
    {"space:kepler_elliptic", model_kepler_elliptic},
    {"space:kepler_secular", model_kepler_secular},
}};

}  // namespace

std::span<const MotionModel> builtin_motion_models() {
    return builtin_models;
}

const MotionModel* find_motion_model(std::string_view id) {
    for (const MotionModel& m : builtin_models) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

double solve_kepler(double mean_anomaly, double e) {
    // Odd in M: solve for |M| in [0, π], where E − e sin E is increasing and convex.
    const double m = std::abs(mean_anomaly);
    // ADR 0048's starter (π for e > 0.8) needs 16 Newton steps at e = 0.9999 and small M; the cubic root of
    // E³/6 ≈ M (E − sin E for e → 1) needs 6 (sp-04-orrery-clock.md).
    double big_e = e > 0.8 ? std::min(pi, dmath::cbrt(6.0 * m)) : m + e * dmath::sin(m);
    for (int k = 0; k < kepler_iterations; ++k) {
        big_e -= (big_e - e * dmath::sin(big_e) - m) / (1.0 - e * dmath::cos(big_e));
    }
    return mean_anomaly < 0.0 ? -big_e : big_e;
}

void perifocal_axes(double i, double node, double arg_periapsis, math::dvec3& p, math::dvec3& q) {
    const double ci = dmath::cos(i), si = dmath::sin(i);
    const double co = dmath::cos(node), so = dmath::sin(node);
    const double cw = dmath::cos(arg_periapsis), sw = dmath::sin(arg_periapsis);
    // R_z(Ω) R_x(i) R_z(ω) applied to the perifocal x and y axes (JPL's formulae).
    p = to_frame(cw * co - sw * so * ci, cw * so + sw * co * ci, sw * si);
    q = to_frame(-sw * co - cw * so * ci, -sw * so + cw * co * ci, cw * si);
}

std::int32_t System::add(OrbitNode node, std::string_view model) {
    node.model = find_motion_model(model);
    VEACH_ASSERT(node.model != nullptr, "unknown motion model");
    VEACH_ASSERT(node.parent < static_cast<std::int32_t>(nodes_.size()), "a node's parent must come before it");
    VEACH_ASSERT(node.model->evaluate == model_fixed || node.period_q > 0, "an orbit needs a period");
    const Elements& el = node.elements;
    perifocal_axes(el.i, el.node, el.arg_periapsis, node.p_axis, node.q_axis);
    nodes_.push_back(std::move(node));
    return static_cast<std::int32_t>(nodes_.size() - 1);
}

std::int32_t System::find(std::string_view name) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].name == name) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

void System::evaluate(std::int64_t tick, std::span<Pose> out) const {
    VEACH_ASSERT(out.size() == nodes_.size(), "one pose per node");
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const OrbitNode& n = nodes_[i];
        Pose local = n.model->evaluate(n, tick);
        local.position *= n.mass_fraction;
        local.velocity *= n.mass_fraction;
        if (n.parent >= 0) {
            const Pose& parent = out[static_cast<std::size_t>(n.parent)];
            local.position += parent.position;
            local.velocity += parent.velocity;
        }
        out[i] = local;
    }
}

Pose System::evaluate(std::int32_t index, std::int64_t tick) const {
    Pose pose;
    for (std::int32_t i = index; i >= 0; i = node(i).parent) {
        const OrbitNode& n = node(i);
        const Pose local = n.model->evaluate(n, tick);
        pose.position += local.position * n.mass_fraction;
        pose.velocity += local.velocity * n.mass_fraction;
    }
    return pose;
}

Orientation orientation(const Spin& spin, std::int64_t tick) {
    const double theta = spin.phase0 + (spin.period_q > 0 ? two_pi * phase(tick, spin.epoch_tick, spin.period_q) : 0.0);
    const double ct = dmath::cos(theta), st = dmath::sin(theta);
    const double ce = dmath::cos(spin.obliquity), se = dmath::sin(spin.obliquity);
    const double cp = dmath::cos(spin.pole_longitude), sp = dmath::sin(spin.pole_longitude);
    // R = R_y(ψ) R_z(−ε) R_y(θ); R_y(a): (x, z) → (x cos a + z sin a, −x sin a + z cos a).
    const auto ry = [](double c, double s, math::dvec3 v) {
        return math::dvec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
    };
    const auto rz_minus = [&](math::dvec3 v) {  // R_z(−ε): (x, y) → (x cos ε + y sin ε, −x sin ε + y cos ε)
        return math::dvec3(v.x * ce + v.y * se, -v.x * se + v.y * ce, v.z);
    };
    const auto apply = [&](math::dvec3 v) {
        return ry(cp, sp, rz_minus(ry(ct, st, v)));
    };
    return {apply({1.0, 0.0, 0.0}), apply({0.0, 1.0, 0.0}), apply({0.0, 0.0, 1.0})};
}

double sidereal_from_solar(double solar_day_seconds, double orbit_seconds) {
    return 1.0 / (1.0 / solar_day_seconds + 1.0 / orbit_seconds);
}

}  // namespace veach::space
