// Spike SP-04 (VCH-080, docs/research/spikes/sp-04-orrery-clock.md): the universe clock's exact phase (ADR 0047),
// bodies on rails against JPL (ADR 0048), the committed cross-platform orbit hash (ADR 0043), and the sun from the
// system at two scales (ADR 0049).

#include <doctest/doctest.h>
#include <veach/core/dmath.hpp>
#include <veach/space/clock.hpp>
#include <veach/space/orbit.hpp>
#include <veach/space/presets.hpp>
#include <veach/space/sun.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string_view>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#include "support/dmath_vectors.hpp"

namespace {

namespace vs = veach::space;
using veach::math::dvec3;

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;
constexpr double eps = std::numeric_limits<double>::epsilon();

/// (m × 2^16) mod p with a 128-bit intermediate: an independent reference for phase_residue.
std::uint64_t residue_reference(std::uint64_t m, std::uint64_t p) {
#if defined(__SIZEOF_INT128__)
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(m) << 16) % p);
#else
    std::uint64_t remainder = 0;
    static_cast<void>(_udiv128(m >> 48, m << 16, p, &remainder));  // the quotient is below 2^16
    return remainder;
#endif
}

std::uint64_t splitmix(std::uint64_t& s) {
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/// Distance between two phases on the unit circle.
double circle_distance(double a, double b) {
    const double d = std::abs(a - b);
    return std::min(d, 1.0 - d);
}

double fraction(double x) {
    return x - std::floor(x);
}

}  // namespace

TEST_CASE("[sp04.clock] phase is exact for any tick below 2^53 and any period") {
    std::uint64_t seed = 0x5eed04;
    for (int i = 0; i < 20'000; ++i) {
        const auto tick = static_cast<std::int64_t>(splitmix(seed) % static_cast<std::uint64_t>(vs::tick_limit));
        const auto epoch = static_cast<std::int64_t>(splitmix(seed) % (std::uint64_t{1} << 40));
        // Periods from a fraction of a tick to the whole int64 range (ADR 0047's int64 product breaks above 2^47).
        const int bits = 1 + static_cast<int>(splitmix(seed) % 62);
        const auto p = static_cast<std::int64_t>(1 + splitmix(seed) % (std::uint64_t{1} << bits));
        std::int64_t m = (tick - epoch) % p;
        if (m < 0) {
            m += p;
        }
        const std::uint64_t expected = residue_reference(static_cast<std::uint64_t>(m), static_cast<std::uint64_t>(p));
        REQUIRE(vs::phase_residue(tick, epoch, p) == expected);
        const double ph = vs::phase(tick, epoch, p);
        REQUIRE(ph >= 0.0);
        REQUIRE(ph < 1.0);
    }
}

TEST_CASE("[sp04.clock] a whole-day period is exactly 0 at every midnight and 0.5 at every noon, up to 2^53") {
    const std::int64_t day_q = vs::period_q_from_seconds(86'400.0);
    CHECK(day_q == vs::ticks_per_day * vs::period_q_per_tick);
    for (const std::int64_t days : {std::int64_t{0}, std::int64_t{1}, std::int64_t{365}, std::int64_t{1'000'000},
                                    (vs::tick_limit - 1) / vs::ticks_per_day}) {
        CHECK(vs::phase(days * vs::ticks_per_day, 0, day_q) == 0.0);
        CHECK(vs::phase(days * vs::ticks_per_day + vs::ticks_per_day / 2, 0, day_q) == 0.5);
    }
    // Before the epoch too: the mod is non-negative.
    CHECK(vs::phase(-vs::ticks_per_day / 4, 0, day_q) == 0.75);
}

TEST_CASE("[sp04.clock] resonances 2:1 and 3:2 never drift") {
    // Inner period k, outer 2k (2:1); and 2k' against 3k' (3:2). Odd k so the periods are not whole ticks.
    const std::int64_t k = vs::period_q_from_seconds(1.769 * 86'400.0) | 1;                  // Io-like
    const std::int64_t k2 = vs::period_q_from_seconds(0.5 * 248.0 * 365.25 * 86'400.0) | 1;  // Neptune-Pluto-like
    double worst_21 = 0.0, worst_32 = 0.0, worst_double_drift = 0.0, worst_double_32 = 0.0;
    for (const std::int64_t tick :
         {std::int64_t{0}, std::int64_t{1}, std::int64_t{1'000'000}, std::int64_t{1'000'000'000}, std::int64_t{1} << 46,
          vs::tick_limit - 1, vs::tick_limit / 3, vs::tick_limit / 7}) {
        // The integer residues lock exactly: (X mod 2k) mod k == X mod k.
        CHECK(vs::phase_residue(tick, 0, 2 * k) % static_cast<std::uint64_t>(k) == vs::phase_residue(tick, 0, k));
        CHECK(vs::phase_residue(tick, 0, 3 * k2) % static_cast<std::uint64_t>(k2) ==
              vs::phase_residue(tick, 0, 2 * k2) % static_cast<std::uint64_t>(k2));
        // The phases agree to rounding, independent of the tick.
        worst_21 =
            std::max(worst_21, circle_distance(fraction(2.0 * vs::phase(tick, 0, 2 * k)), vs::phase(tick, 0, k)));
        worst_32 = std::max(worst_32, circle_distance(fraction(3.0 * vs::phase(tick, 0, 3 * k2)),
                                                      fraction(2.0 * vs::phase(tick, 0, 2 * k2))));
        // Double seconds (the alternative ADR 0047 rejected): t / P in double drifts from the exact phase, and the
        // 3:2 pair drifts apart, as t grows.
        const double t = static_cast<double>(tick) / 60.0;
        const double a = vs::seconds_from_period_q(3 * k2), b = vs::seconds_from_period_q(2 * k2);
        worst_double_drift = std::max(worst_double_drift, circle_distance(fraction(t / a), vs::phase(tick, 0, 3 * k2)));
        worst_double_32 = std::max(worst_double_32,
                                   circle_distance(fraction(3.0 * fraction(t / a)), fraction(2.0 * fraction(t / b))));
    }
    INFO(std::format("2:1 worst {:.3g}, 3:2 worst {:.3g}; double seconds: drift {:.3g}, 3:2 worst {:.3g}", worst_21,
                     worst_32, worst_double_drift, worst_double_32));
    CHECK(worst_21 <= 4 * eps);
    CHECK(worst_32 <= 4 * eps);
    CHECK(worst_double_32 > 1000.0 * worst_32);  // what the integer clock buys (measured: 1.8e-12 vs 2.2e-16)
}

TEST_CASE("[sp04.kepler] six Newton steps reach the double's limit for every e up to 0.9999") {
    std::vector<double> anomalies;
    for (int k = 1; k <= 15; ++k) {
        anomalies.push_back(std::pow(10.0, -k));
    }
    for (int k = 0; k <= 400; ++k) {
        anomalies.push_back(-pi + 2.0 * pi * k / 400.0);
    }
    for (const double e : {0.0, 0.1, 0.3, 0.5, 0.7, 0.75, 0.8, 0.8001, 0.9, 0.95, 0.99, 0.999, 0.9999}) {
        for (const double m : anomalies) {
            const double big_e = vs::solve_kepler(m, e);
            const double residual = big_e - e * veach::dmath::sin(big_e) - m;
            INFO(std::format("e = {}, M = {:a}: E = {:a}, residual {:.3g}", e, m, big_e, residual));
            REQUIRE(std::abs(residual) <= 4.0 * eps * (std::abs(big_e) + std::abs(m)));
        }
    }
    // ADR 0048's starter (π for e > 0.8) with its 12 steps has not converged here; the cubic starter has.
    const double e = 0.9999, m = 1e-6;
    double big_e = pi;
    for (int k = 0; k < 12; ++k) {
        big_e -= (big_e - e * veach::dmath::sin(big_e) - m) / (1.0 - e * veach::dmath::cos(big_e));
    }
    CHECK(std::abs(big_e - vs::solve_kepler(m, e)) > 1e-6);
}

namespace {

struct SolReference {
    std::string_view body;
    double jd;
    double formula[3];  ///< km, ecliptic J2000
    double de441[3];    ///< km, ecliptic J2000
};

const SolReference sol_reference[] = {
#include "support/sp04_sol_reference.inl"
};

/// System frame (x, z, −y) back to ecliptic, in km.
dvec3 ecliptic_km(dvec3 p) {
    return dvec3(p.x, -p.z, p.y) / 1000.0;
}

/// Heliocentric (or, for the Moon, geocentric) position in km.
dvec3 sol_position(const vs::System& sol, std::string_view body, std::int64_t tick) {
    const dvec3 p = sol.evaluate(sol.find(body), tick).position;
    if (body == "moon") {
        return ecliptic_km(p - sol.evaluate(sol.find("earth"), tick).position);
    }
    return ecliptic_km(p);
}

struct Spherical {
    double longitude, latitude, distance;
};
Spherical spherical(dvec3 v) {
    const double r = std::sqrt(veach::math::dot(v, v));
    return {veach::dmath::atan2(v.y, v.x), veach::dmath::asin(v.z / r), r};
}

struct Limit {
    std::string_view body;
    double longitude_arcsec, latitude_arcsec, distance_mm;  ///< distance in 1000 km
};
// JPL's stated nominal errors for 1800-2050, raised to 1.1 × the error of the published formula itself against
// DE441 where that is larger (measured independently in Python: sp-04-orrery-clock.md). Earth: EMB's plus the
// Moon split; the Moon: mean elements, no stated accuracy.
constexpr Limit limits[] = {
    {"mercury", 15, 1.1, 1.4}, {"venus", 20, 1, 4.3},
    {"emb", 20, 8, 7.9},       {"mars", 65, 2, 25},
    {"jupiter", 400, 10, 605}, {"saturn", 600, 25.3, 2062},
    {"uranus", 87, 2.5, 1476}, {"neptune", 50, 1.5, 1014},
    {"earth", 25, 10, 8},      {"moon", 12 * 3600, 1 * 3600, 7},
};

}  // namespace

TEST_CASE("[sp04.sol] the Sol preset reproduces JPL's published formulae and stays within their error of DE441") {
    const vs::System sol = vs::sol();
    for (const SolReference& ref : sol_reference) {
        const std::int64_t tick = vs::sol_tick_from_jd(ref.jd);
        const dvec3 p = sol_position(sol, ref.body, tick);
        const dvec3 formula(ref.formula[0], ref.formula[1], ref.formula[2]);
        const dvec3 de441(ref.de441[0], ref.de441[1], ref.de441[2]);
        const dvec3 d_formula = p - formula;
        const double formula_error = std::sqrt(veach::math::dot(d_formula, d_formula));
        const double scale = std::sqrt(veach::math::dot(formula, formula));
        INFO(std::format("{} at JD {}: {:.6f} km from the formula", ref.body, ref.jd, formula_error));
        CHECK(formula_error <= 1e-9 * scale);

        const Spherical a = spherical(p), b = spherical(de441);
        double dl = a.longitude - b.longitude;
        dl -= 2.0 * pi * std::floor((dl + pi) / (2.0 * pi));
        const double dl_arcsec = std::abs(dl) / deg * 3600.0;
        const double db_arcsec = std::abs(a.latitude - b.latitude) / deg * 3600.0;
        const double dr = std::abs(a.distance - b.distance) / 1000.0;
        for (const Limit& l : limits) {
            if (l.body == ref.body) {
                INFO(std::format("vs DE441: {:.1f}\" longitude, {:.1f}\" latitude, {:.1f} thousand km", dl_arcsec,
                                 db_arcsec, dr));
                CHECK(dl_arcsec <= l.longitude_arcsec);
                CHECK(db_arcsec <= l.latitude_arcsec);
                CHECK(dr <= l.distance_mm);
            }
        }
    }
}

namespace {

void hash_pose(veach::test::dmath_vectors::Hash& h, const vs::Pose& p) {
    for (int c = 0; c < 3; ++c) {
        h.add(p.position[c]);
        h.add(p.velocity[c]);
    }
}

void hash_vec(veach::test::dmath_vectors::Hash& h, dvec3 v) {
    for (int c = 0; c < 3; ++c) {
        h.add(v[c]);
    }
}

/// Every node's pose, every spinning node's axes and one observer's sun, at each tick.
std::uint64_t system_hash(const vs::System& system, std::span<const std::int64_t> ticks, std::string_view observer,
                          std::string_view star) {
    veach::test::dmath_vectors::Hash h;
    std::vector<vs::Pose> poses(system.nodes().size());
    const std::int32_t body = system.find(observer), sun = system.find(star);
    for (const std::int64_t tick : ticks) {
        system.evaluate(tick, poses);
        for (const vs::Pose& p : poses) {
            hash_pose(h, p);
        }
        for (const vs::OrbitNode& n : system.nodes()) {
            if (n.spin.period_q > 0) {
                const vs::Orientation o = vs::orientation(n.spin, tick);
                hash_vec(h, o.x);
                hash_vec(h, o.y);
            }
        }
        const vs::SunSample s = vs::sun_local(system, body, sun, 0.85, 0.04, tick);
        hash_vec(h, s.local);
        h.add(s.distance);
    }
    return h.value;
}

std::vector<std::int64_t> spread_ticks(std::int64_t first, std::int64_t last, int count) {
    std::vector<std::int64_t> ticks;
    for (int k = 0; k < count; ++k) {
        ticks.push_back(first + (last - first) / (count - 1) * k);
    }
    return ticks;
}

}  // namespace

TEST_CASE("[sp04.hash] orbit states give the same bits on every platform") {
    // Regenerate only on purpose: a different value is a change to the universe (dmath, the solver, the clock or the
    // elements). The macOS gate and Windows CI must both print these.
    std::vector<std::int64_t> ticks = {0, 1, 1'000'000, 1'000'000'000, std::int64_t{1} << 46, vs::tick_limit - 1};
    const std::vector<std::int64_t> spread = spread_ticks(0, vs::tick_limit - 1, 1000);
    ticks.insert(ticks.end(), spread.begin(), spread.end());
    const std::uint64_t reference = system_hash(vs::reference_system(), ticks, "circumbinary", "star_a");

    // Sol over JPL Table 1's interval, 1800-2050.
    const std::vector<std::int64_t> sol_ticks = spread_ticks(0, vs::sol_tick_from_jd(2'469'807.5), 1000);
    const std::uint64_t sol = system_hash(vs::sol(), sol_ticks, "earth", "sun");

    // The Earth-like planet at s = 0.01 over two of its years.
    const vs::System small = vs::earthlike({.body = 0.01, .orbit = 0.01});
    const std::int64_t year = small.node(1).period_q / vs::period_q_per_tick;
    const std::uint64_t earthlike = system_hash(small, spread_ticks(0, 2 * year, 1000), "planet", "sun");

    INFO(std::format("reference 0x{:016x}, sol 0x{:016x}, earthlike 0x{:016x}", reference, sol, earthlike));
    CHECK(reference == 0xdaeaf79cdb3e0e48ULL);
    CHECK(sol == 0x68f86b72db109716ULL);
    CHECK(earthlike == 0xbae95477aa9d6f98ULL);
}

namespace {

/// The noon elevation at `latitude` at the instant of a season event: the observer stands under the sun's meridian.
double event_noon_elevation(const vs::System& system, std::int64_t tick, double latitude) {
    const std::int32_t planet = system.find("planet"), sun = system.find("sun");
    const vs::SubsolarPoint sub = vs::subsolar_point(system, planet, sun, tick);
    const vs::SunSample s = vs::sun_local(system, planet, sun, latitude, sub.longitude, tick);
    CHECK(std::abs(s.local_solar_time - 12.0) < 1e-6);
    return s.elevation / deg;
}

/// The tick of a quarter of the planet's year after the June solstice (0: June, 1: September, 2: December).
std::int64_t season_tick(const vs::System& system, int quarter) {
    return (system.node(1).period_q * quarter / 4 + vs::period_q_per_tick / 2) / vs::period_q_per_tick;
}

}  // namespace

TEST_CASE("[sp04.sun] noon elevations at 60°N follow the tilt, identically at s = 1 and s = 0.01") {
    const vs::System full = vs::earthlike({.body = 1.0, .orbit = 1.0});
    const vs::System small = vs::earthlike({.body = 0.01, .orbit = 0.01});
    // Periods follow √s; the day doesn't.
    CHECK(
        std::abs(vs::seconds_from_period_q(small.node(1).period_q) / vs::seconds_from_period_q(full.node(1).period_q) -
                 0.1) < 1e-12);
    const double expected[] = {53.4, 30.0, 6.6};
    for (int quarter = 0; quarter < 3; ++quarter) {
        const double e_full = event_noon_elevation(full, season_tick(full, quarter), 60.0 * deg);
        const double e_small = event_noon_elevation(small, season_tick(small, quarter), 60.0 * deg);
        INFO(std::format("quarter {}: {:.9f}° at s = 1, {:.9f}° at s = 0.01", quarter, e_full, e_small));
        CHECK(std::abs(e_full - expected[quarter]) < 0.01);
        CHECK(std::abs(e_full - e_small) < 1e-9);
    }
    // Polar night at 80°N on the December solstice: the sun stays below the horizon all day, at both scales.
    for (const vs::System* system : {&full, &small}) {
        const std::int64_t december = season_tick(*system, 2);
        double highest = -90.0;
        for (std::int64_t t = december - vs::ticks_per_day / 2; t <= december + vs::ticks_per_day / 2; t += 60 * 10) {
            highest = std::max(highest, vs::sun_local(*system, 1, 0, 80.0 * deg, 0.0, t).elevation / deg);
        }
        INFO(std::format("highest sun at 80°N around the December solstice: {:.3f}°", highest));
        CHECK(highest < -13.0);
    }
}
