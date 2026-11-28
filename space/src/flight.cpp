// veach: canonical (flight.hpp).

#include <veach/space/flight.hpp>

#include <veach/core/dmath.hpp>

#include <algorithm>
#include <cmath>

namespace veach::space {

namespace {

math::dvec3 unit(const math::dvec3& v) {
    return v / std::sqrt(math::dot(v, v));
}

double length(const math::dvec3& v) {
    return std::sqrt(math::dot(v, v));
}

}  // namespace

double Governor::cap(double height) const {
    return std::max(v_floor, std::max(height, 0.0) / tau_near);
}

double Governor::cruise_cap(double height) const {
    return std::max(v_floor, std::max(height, 0.0) / tau_cruise);
}

HandoffBands HandoffBands::of(const RealizedBody& body, double s_a) {
    HandoffBands b;
    b.body_fixed_below = body.atmosphere_top > 0.0 ? body.atmosphere_top : 100'000.0 * s_a;
    b.inertial_above = 1.1 * b.body_fixed_below;
    return b;
}

Handoff decide_handoff(const HandoffBands& b, FlightFrame frame, FlightScene scene, double altitude, double height) {
    if (scene == FlightScene::surface) {
        return height > b.surface_above ? Handoff::to_free_flight : Handoff::none;
    }
    if (frame == FlightFrame::inertial) {
        return altitude < b.body_fixed_below ? Handoff::to_body_fixed : Handoff::none;
    }
    if (altitude > b.inertial_above) {
        return Handoff::to_inertial;
    }
    return height < b.surface_below ? Handoff::to_surface : Handoff::none;
}

math::dvec3 Heading::right() const {
    return unit(math::cross(forward, up));
}

math::dquat Heading::rotation() const {
    const math::dvec3 x = right();
    const math::dvec3 z = -forward;
    return math::normalize(math::quat_cast(math::dmat3(x, math::cross(z, x), z)));
}

Heading transport(const Heading& h, const math::dvec3& target_up, double alpha, double yaw) {
    Heading out;
    out.up = unit(h.up + (target_up - h.up) * alpha);
    out.forward = unit(h.forward - out.up * math::dot(h.forward, out.up));
    if (yaw != 0.0) {
        out.forward = unit(out.forward * dmath::cos(yaw) + math::cross(out.up, out.forward) * dmath::sin(yaw));
    }
    return out;
}

double filter_alpha(double dt, double tau) {
    return 1.0 - dmath::exp(-dt / tau);
}

double cruise_limit(const Governor& g, const CraftParams& p, double altitude, double height, double top) {
    const double entry = std::sqrt(2.0 * p.entry_braking * top);
    return std::min(g.cruise_cap(height), entry + std::max(altitude - top, 0.0) / g.tau_cruise);
}

FlightCommand fly(const Governor& g, const CraftParams& p, const FlightControls& c, const FlightState& s, double top) {
    FlightCommand out;
    out.engine = c.engine > 0.5;
    out.cruise = s.altitude > top;
    out.cap = out.cruise ? cruise_limit(g, p, s.altitude, s.height, top) : g.cap(s.height);
    if (!out.engine) {
        return out;
    }
    const Heading& h = s.heading;
    math::dvec3 target = (h.up * c.lift + h.right() * c.right + h.forward * c.forward) * out.cap;
    const double speed = length(target);
    if (speed > out.cap) {
        target *= out.cap / speed;
    }
    if (out.cruise) {
        out.velocity = target;
        return out;
    }
    math::dvec3 a = (target - s.velocity) / p.assist_tau - s.gravity;
    const double magnitude = length(a);
    if (magnitude > p.max_acceleration) {
        a *= p.max_acceleration / magnitude;
    }
    out.acceleration = a;
    return out;
}

}  // namespace veach::space
