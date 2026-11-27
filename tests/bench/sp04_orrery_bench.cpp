// Spike SP-04 (VCH-080), throwaway: cost of bodies on rails, and the tables in
// docs/research/spikes/sp-04-orrery-clock.md.
//   build/release/tests/veach-bench-sp04
// Prints ns per node per evaluation (best of 5) for the Sol preset, the hash's reference system and the Earth-like
// planet, the cost of the pieces (phase, Kepler solve, orientation, sun query), Sol against JPL Horizons DE441, and
// noon sun elevations at 60°N at s = 1 and s = 0.01.

#include <veach/core/dmath.hpp>
#include <veach/space/clock.hpp>
#include <veach/space/orbit.hpp>
#include <veach/space/presets.hpp>
#include <veach/space/sun.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

namespace vs = veach::space;
using veach::math::dvec3;

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;

volatile double sink = 0.0;

template <class F>
double best_ns(int repeats, int iterations, F&& f) {
    double best = 1e300;
    for (int r = 0; r < repeats; ++r) {
        const auto start = std::chrono::steady_clock::now();
        double acc = 0.0;
        for (int i = 0; i < iterations; ++i) {
            acc += f(i);
        }
        const auto stop = std::chrono::steady_clock::now();
        sink = sink + acc;
        best = std::min(best, std::chrono::duration<double, std::nano>(stop - start).count() / iterations);
    }
    return best;
}

std::int64_t spread(int i, std::int64_t span) {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL) %
                                     static_cast<std::uint64_t>(span));
}

void cost(const char* name, const vs::System& system, std::int64_t span) {
    std::vector<vs::Pose> poses(system.nodes().size());
    const double ns = best_ns(5, 200'000, [&](int i) {
        system.evaluate(spread(i, span), poses);
        return poses.back().position.x;
    });
    std::printf("| %s | %zu | %.0f | %.1f |\n", name, poses.size(), ns, ns / static_cast<double>(poses.size()));
}

struct SolReference {
    std::string_view body;
    double jd;
    double formula[3];
    double de441[3];
};
const SolReference sol_reference[] = {
#include "support/sp04_sol_reference.inl"
};

dvec3 ecliptic_km(dvec3 p) {
    return dvec3(p.x, -p.z, p.y) / 1000.0;
}

void sol_accuracy() {
    const vs::System sol = vs::sol();
    std::printf(
        "\n| Body | longitude (\") | latitude (\") | distance (1000 km) | worst date (JD) |\n|---|---|---|---|---|\n");
    std::string_view current;
    double wl = 0, wb = 0, wr = 0, wjd = 0;
    const auto flush = [&] {
        if (!current.empty()) {
            std::printf("| %.*s | %.1f | %.1f | %.1f | %.1f |\n", static_cast<int>(current.size()), current.data(), wl,
                        wb, wr, wjd);
        }
    };
    for (const SolReference& ref : sol_reference) {
        if (ref.body != current) {
            flush();
            current = ref.body;
            wl = wb = wr = wjd = 0;
        }
        const std::int64_t tick = vs::sol_tick_from_jd(ref.jd);
        dvec3 p = sol.evaluate(sol.find(ref.body), tick).position;
        if (ref.body == "moon") {
            p -= sol.evaluate(sol.find("earth"), tick).position;
        }
        p = ecliptic_km(p);
        const dvec3 h(ref.de441[0], ref.de441[1], ref.de441[2]);
        const double rp = std::sqrt(veach::math::dot(p, p)), rh = std::sqrt(veach::math::dot(h, h));
        double dl = veach::dmath::atan2(p.y, p.x) - veach::dmath::atan2(h.y, h.x);
        dl -= 2.0 * pi * std::floor((dl + pi) / (2.0 * pi));
        const double l = std::abs(dl) / deg * 3600.0;
        const double b = std::abs(veach::dmath::asin(p.z / rp) - veach::dmath::asin(h.z / rh)) / deg * 3600.0;
        const double r = std::abs(rp - rh) / 1000.0;
        if (l > wl) {
            wjd = ref.jd;
        }
        wl = std::max(wl, l);
        wb = std::max(wb, b);
        wr = std::max(wr, r);
    }
    flush();
}

std::int64_t season_tick(const vs::System& system, int quarter) {
    return (system.node(1).period_q * quarter / 4 + vs::period_q_per_tick / 2) / vs::period_q_per_tick;
}

/// Local noon at `longitude` nearest `around`: where the sun's east component turns negative (integer bisection).
std::int64_t noon_near(const vs::System& s, std::int64_t around, double latitude, double longitude) {
    const auto east = [&](std::int64_t t) {
        return vs::sun_local(s, 1, 0, latitude, longitude, t).local.x;
    };
    const std::int64_t step = 10 * 60 * vs::ticks_per_second;
    std::int64_t best = around;
    std::int64_t best_distance = INT64_MAX;
    for (std::int64_t t = around - vs::ticks_per_day; t < around + vs::ticks_per_day; t += step) {
        if (east(t) > 0.0 && east(t + step) <= 0.0) {
            std::int64_t lo = t, hi = t + step;
            while (hi - lo > 1) {
                const std::int64_t mid = lo + (hi - lo) / 2;
                (east(mid) > 0.0 ? lo : hi) = mid;
            }
            if (std::abs(hi - around) < best_distance) {
                best_distance = std::abs(hi - around);
                best = hi;
            }
        }
    }
    return best;
}

void elevations() {
    std::printf(
        "\n| Event | s | year (days) | noon elevation at the event (deg) | noon at longitude 0 nearest the event "
        "(deg) | its offset from the event (h) |\n|---|---|---|---|---|---|\n");
    const char* names[] = {"June solstice", "September equinox", "December solstice"};
    for (int q = 0; q < 3; ++q) {
        for (const double scale : {1.0, 0.01}) {
            const vs::System s = vs::earthlike({.body = scale, .orbit = scale});
            const std::int64_t tick = season_tick(s, q);
            const vs::SubsolarPoint sub = vs::subsolar_point(s, 1, 0, tick);
            const double at_event = vs::sun_local(s, 1, 0, 60.0 * deg, sub.longitude, tick).elevation / deg;
            const std::int64_t noon = noon_near(s, tick, 60.0 * deg, 0.0);
            const double at_noon = vs::sun_local(s, 1, 0, 60.0 * deg, 0.0, noon).elevation / deg;
            std::printf("| %s | %g | %.3f | %.9f | %.4f | %+.2f |\n", names[q], scale,
                        vs::seconds_from_period_q(s.node(1).period_q) / 86'400.0, at_event, at_noon,
                        static_cast<double>(noon - tick) / 3600.0 / 60.0);
        }
    }
}

}  // namespace

int main() {
    const std::int64_t sol_span = vs::sol_tick_from_jd(2'469'807.5);
    std::printf("| System | nodes | ns per evaluation of all nodes | ns per node |\n|---|---|---|---|\n");
    cost("Sol (kepler_secular, 2 Kepler solves for the Earth-Moon pair)", vs::sol(), sol_span);
    cost("reference (kepler_elliptic, e up to 0.9999)", vs::reference_system(), vs::tick_limit - 1);
    cost("Earth-like (kepler_elliptic, e = 0)", vs::earthlike(), vs::tick_limit - 1);

    std::printf("\n| Piece | ns per call |\n|---|---|\n");
    std::printf("| phase (int64 mod + 16 shift-subtract steps + 1 division) | %.1f |\n",
                best_ns(5, 2'000'000, [](int i) {
                    return vs::phase(spread(i, vs::tick_limit - 1), 12345, 0x3fffffffffffffffLL - i);
                }));
    for (const double e : {0.0167, 0.5, 0.9999}) {
        std::printf("| solve_kepler, e = %g | %.1f |\n", e, best_ns(5, 2'000'000, [e](int i) {
                        return vs::solve_kepler(-pi + 2.0 * pi * static_cast<double>(i % 100'003) / 100'003.0, e);
                    }));
    }
    const vs::System earth = vs::earthlike();
    const vs::Spin spin = earth.node(1).spin;
    std::printf("| orientation (spin) | %.1f |\n",
                best_ns(5, 2'000'000, [&](int i) { return vs::orientation(spin, spread(i, vs::tick_limit - 1)).x.x; }));
    std::printf("| sun_local (Earth-like: 2 node walks, orientation, frame, asin, atan2 x2) | %.1f |\n",
                best_ns(5, 500'000, [&](int i) {
                    return vs::sun_local(earth, 1, 0, 1.0, 0.5, spread(i, vs::tick_limit - 1)).elevation;
                }));
    std::printf("| dmath::sin (for scale) | %.1f |\n",
                best_ns(5, 2'000'000, [](int i) { return veach::dmath::sin(static_cast<double>(i) * 1e-3); }));

    sol_accuracy();
    elevations();
    return 0;
}
