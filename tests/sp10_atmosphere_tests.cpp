// Spike SP-10 (VCH-086), throwaway: an atmosphere from its composition, the s_a scale rule and the eclipse's
// disk-overlap fraction (docs/research/spikes/sp-10-atmosphere.md).
#include <veach/space/atmosphere.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>

using namespace veach;

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;

double relative(double a, double b) {
    return std::abs(a - b) / std::abs(b);
}

}  // namespace

TEST_CASE("[sp10.rayleigh] Earth's air from its composition gives the measured Rayleigh optical depth") {
    const space::AtmosphereRecord air = space::atmosphere_from(space::earth_air());
    const double column = air.rayleigh_height * (1.0 - std::exp(-(air.top - air.radius) / air.rayleigh_height));
    // Bodhaine et al. 1999: 0.0973 at 550 nm for 1013.25 hPa.
    CHECK(relative(air.rayleigh.y * column, 0.0973) < 0.03);
    CHECK(relative(air.rayleigh.z / air.rayleigh.x, std::pow(680.0 / 440.0, 4.0)) < 1e-12);  // λ⁻⁴
    CHECK(relative(air.rayleigh_height, 8430.0) < 0.01);                                     // kT / (m g) at 288 K
    // Hillaire's 13.558e-6 per meter is 18% above this (his optical depth, 0.108 with an 8 km height, 11%).
    CHECK(relative(air.rayleigh.y, 13.558e-6) < 0.2);
    // The ozone tent reproduces Hillaire's peak absorption.
    CHECK(relative(air.absorption.y, 1.881e-6) < 1e-3);
}

TEST_CASE("[sp10.composition] Mars-like and Titan-like air") {
    const space::AtmosphereRecord mars = space::atmosphere_from(space::mars_air());
    CHECK(relative(mars.rayleigh_height, 10.7e3) < 0.02);
    const math::dvec3 tau = space::vertical_optical_depth(mars);
    CHECK(tau.y > 0.49);  // the dust's 0.5 dominates
    CHECK(mars.rayleigh.y * mars.rayleigh_height < 0.01);
    CHECK(mars.mie_absorption.z > mars.mie_absorption.x);  // the dust absorbs the blue
    const space::AtmosphereRecord titan = space::atmosphere_from(space::titan_air());
    CHECK(titan.rayleigh.y * titan.rayleigh_height > 1.0);  // 1.5 bar under 1.35 m/s²: a thick column
}

TEST_CASE("[sp10.scale] s_a = sqrt(s_p) keeps the vertical optical depth and scales the shell") {
    const space::AtmosphereRecord physical = space::atmosphere_from(space::earth_air());
    const math::dvec3 tau = space::vertical_optical_depth(physical);
    for (const double s : {1.0, 0.1, 0.01}) {
        const space::AtmosphereRecord r = space::scaled(physical, s);
        const math::dvec3 t = space::vertical_optical_depth(r);
        for (int c = 0; c < 3; ++c) {
            CHECK(relative(t[c], tau[c]) < 1e-9);
        }
        CHECK(relative(r.radius, physical.radius * s) < 1e-12);
        CHECK(relative(r.top - r.radius, (physical.top - physical.radius) * std::sqrt(s)) < 1e-12);
    }
    // At s = 0.01 an Earth-like atmosphere is about 10 km thick on a 63.7 km planet (ADR 0049's table).
    const space::AtmosphereRecord small = space::scaled(physical, 0.01);
    CHECK(std::abs((small.top - small.radius) - 10.1e3) < 0.2e3);
}

TEST_CASE("[sp10.eclipse] the visible fraction of a sun's disk matches the disk-overlap integral") {
    const auto numeric = [](double b, double a, double c) {
        // The sun's disk sampled on a fine polar grid; points inside the occluder's disk are hidden.
        const int n = 600;
        double visible = 0.0;
        double total = 0.0;
        for (int i = 0; i < n; ++i) {
            const double r = b * std::sqrt((i + 0.5) / n);
            for (int j = 0; j < n; ++j) {
                const double phi = 2.0 * pi * (j + 0.5) / n;
                const double x = r * std::cos(phi) - c;
                const double y = r * std::sin(phi);
                total += 1.0;
                visible += x * x + y * y > a * a ? 1.0 : 0.0;
            }
        }
        return visible / total;
    };
    const double sun = 0.004625;
    for (const auto& [a, c] : std::array<std::pair<double, double>, 7>{{{1.15 * sun, 0.0},
                                                                        {1.15 * sun, 0.5 * 2.15 * sun},
                                                                        {1.15 * sun, 2.0 * sun},
                                                                        {1.15 * sun, 2.2 * sun},
                                                                        {0.5 * sun, 0.2 * sun},
                                                                        {0.5 * sun, 1.2 * sun},
                                                                        {3.0 * sun, 3.5 * sun}}}) {
        CAPTURE(a);
        CAPTURE(c);
        CHECK(std::abs(space::disk_visible_fraction(sun, a, c) - numeric(sun, a, c)) < 2e-3);
    }
    CHECK(space::disk_visible_fraction(sun, 1.15 * sun, 0.1 * sun) == 0.0);            // total
    CHECK(relative(space::disk_visible_fraction(sun, 0.5 * sun, 0.0), 0.75) < 1e-12);  // annular
}
