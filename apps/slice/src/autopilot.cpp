// The slice's autopilot (slice.hpp): a stand-in for a player with a gamepad. Canonical (+ − × ÷ √, nearbyint), so
// the tests' recorded controls are the same on every platform. It reads only the published readout.

#include "slice.hpp"

#include <algorithm>
#include <cmath>

namespace slice {

namespace {

constexpr double hover_height = 50.0;
constexpr double braking = 20.0;       ///< m/s²: the braking curve v = √(2 a h), the cruise drive's entry braking
constexpr double lag = braking * 0.5;  ///< the assist trails its target by a·τ (τ = 0.5 s)
constexpr double cruise_tau = 7.0;     ///< s: lateral correction toward the site during cruise
constexpr double walk_distance = 8.0;  ///< m from the craft before digging
constexpr double hole_ahead = 2.5;     ///< m: where the hole is, ahead of where the character dug
constexpr std::uint32_t digs_wanted = 3;

std::int32_t quantize(double x, double lo, double hi) {
    return static_cast<std::int32_t>(std::nearbyint(std::clamp(x, lo, hi) * veach::axis_one));
}

}  // namespace

std::string_view Autopilot::name(Phase p) {
    switch (p) {
        case Phase::coast: return "orbit";
        case Phase::descend: return "descend";
        case Phase::hover: return "hover";
        case Phase::land: return "land";
        case Phase::landed: return "landed";
        case Phase::exit: return "exit";
        case Phase::walk: return "walk";
        case Phase::dig: return "dig";
        case Phase::step_in: return "step_in";
        case Phase::done: return "done";
    }
    return "?";
}

Controls Autopilot::next(const SliceReadout& r) {
    Controls c;
    c.engine = veach::axis_one;
    const double cap = std::max(r.cap, 1.0);
    // Over the site: a lateral target speed proportional to the offset.
    const double reach = r.altitude > r.top ? cruise_tau : 10.0;
    const double lateral_max = r.altitude > r.top ? 0.4 * cap : std::min(30.0, 0.3 * cap);
    double tr = std::clamp(r.to_right / reach, -lateral_max, lateral_max);
    double tf = std::clamp(r.to_forward / reach, -lateral_max, lateral_max);
    double tu = 0.0;
    ++timer_;
    switch (phase_) {
        case Phase::coast:
            c.engine = 0;
            tr = tf = 0.0;
            if (timer_ >= 180) {
                phase_ = Phase::descend;
                timer_ = 0;
            }
            break;
        case Phase::descend: {
            if (r.altitude > r.top) {
                tu = -cap;  // the cruise drive's limit falls to the entry speed at the top by itself
            } else {
                const double h = r.height - hover_height;
                double down = std::min(std::sqrt(2.0 * braking * std::max(h, 0.0)) - lag, cap);
                down = std::max(down, std::min(2.0, h));
                tu = -down;
                if (h <= 0.5 && std::abs(r.v_up) < 1.0) {
                    phase_ = Phase::hover;
                    timer_ = 0;
                }
            }
            break;
        }
        case Phase::hover:
            tu = std::clamp(-(r.height - hover_height) * 0.5, -5.0, 5.0);
            if (timer_ > 60) {
                hover_min = std::min(hover_min, r.height);
                hover_max = std::max(hover_max, r.height);
            }
            if (timer_ >= 300) {
                phase_ = Phase::land;
                timer_ = 0;
            }
            break;
        case Phase::land:
            tu = -std::clamp(r.height * 0.25, 0.5, 5.0);
            if ((r.height < 0.6 && r.speed < 1.0) || (r.scene == "surface" && r.height < 0.05)) {
                touchdown_speed = r.speed;
                landing_offset = std::sqrt(r.to_right * r.to_right + r.to_forward * r.to_forward);
                phase_ = Phase::landed;
                timer_ = 0;
            }
            break;
        case Phase::landed:
            c.engine = 0;
            tr = tf = 0.0;
            if (r.landed && timer_ >= 150) {
                phase_ = Phase::exit;
                timer_ = 0;
            }
            break;
        case Phase::exit:
            c.engine = 0;
            tr = tf = 0.0;
            c.exit = timer_ <= 2;
            if (r.mode == "on_foot" && timer_ >= 60) {
                phase_ = Phase::walk;
                timer_ = 0;
            }
            break;
        case Phase::walk:
            c.engine = 0;
            tr = tf = 0.0;
            c.forward = r.walked < walk_distance ? veach::axis_one : 0;
            if (r.walked >= walk_distance && timer_ >= 30) {
                walked_at_dig = r.walked;
                phase_ = Phase::dig;
                timer_ = 0;
            }
            break;
        case Phase::dig:
            c.engine = 0;
            tr = tf = 0.0;
            c.dig = timer_ % 45 < 2 && r.digs < digs_wanted;
            if (r.digs >= digs_wanted && timer_ >= 150) {
                phase_ = Phase::step_in;
                timer_ = 0;
            }
            break;
        case Phase::step_in:
            c.engine = 0;
            tr = tf = 0.0;
            c.forward = r.walked < walked_at_dig + hole_ahead && timer_ < 240 ? quantize(0.4, 0.0, 1.0) : 0;
            if (timer_ >= 360) {
                phase_ = Phase::done;
                timer_ = 0;
            }
            break;
        case Phase::done:
            c.engine = 0;
            tr = tf = 0.0;
            break;
    }
    if (phase_ >= Phase::landed) {
        c.lift = c.right = 0;  // on the ground: forward is the walk
        return c;
    }
    c.lift = quantize(tu / cap, -1.0, 1.0);
    c.right = quantize(tr / cap, -1.0, 1.0);
    c.forward = quantize(tf / cap, -1.0, 1.0);
    return c;
}

}  // namespace slice
