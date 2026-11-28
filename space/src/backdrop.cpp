// veach: canonical
// A star system's backdrop from its address (backdrop.hpp, ADR 0091). dmath, veach::hash, + - * / and sqrt only.

#include <veach/space/backdrop.hpp>

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/space/gen/address.hpp>

#include <algorithm>
#include <cmath>

namespace veach::space {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double sun_radius_ly = 26'000.0;    ///< where the default (Earth-like) look is tuned
constexpr double disk_length_ly = 30'000.0;   ///< the disk's light falls by e over this, outward
constexpr double ring_ly = 30'000.0;          ///< star formation peaks in a ring here
constexpr double scale_height_ly = 6'000.0;   ///< the band fades by e this far above the plane
constexpr double band_distance_ly = 8'000.0;  ///< the band's light comes from about this far: its tilt below

math::dvec3 unit_direction(HashStream& h) {
    const double z = h.uniform(-1.0, 1.0);
    const double phi = h.uniform(0.0, 2.0 * pi);
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * dmath::cos(phi), z, r * dmath::sin(phi)};
}

double length(const math::dvec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

}  // namespace

render::GalaxyView galaxy_view(const UniverseConfig& config) {
    const ::space::gen::SystemAddress address{config.system};
    double origin[3] = {0.0, 0.0, 0.0};
    address.cell_origin_ly(origin);
    const double half = address.cell_edge_ly() * 0.5;
    const double x = origin[0] + half;
    const double y = origin[1] + half;
    const double z = origin[2] + half;
    const double radius = std::sqrt(x * x + z * z);
    const double height = std::abs(y);

    render::GalaxyView v;
    HashStream h = HashStream::make(config.seed, "space:backdrop.galaxy", config.system);
    v.north = unit_direction(h);
    const math::dvec3 a = unit_direction(h);
    math::dvec3 c = a - v.north * (a.x * v.north.x + a.y * v.north.y + a.z * v.north.z);
    if (length(c) < 1e-6) {
        c = std::abs(v.north.x) < 0.9 ? math::dvec3(1.0, 0.0, 0.0) : math::dvec3(0.0, 0.0, 1.0);
        c = c - v.north * (c.x * v.north.x + c.y * v.north.y + c.z * v.north.z);
    }
    v.center = c / length(c);

    const double above = dmath::exp(-height / scale_height_ly);
    v.band_brightness = 4e-4 * std::clamp(dmath::exp(-(radius - sun_radius_ly) / disk_length_ly), 0.25, 3.0) * above;
    v.bulge = 0.15 + 2.2 * dmath::exp(-radius / 20'000.0);
    v.anticenter = 0.12 + 0.88 * dmath::exp(-radius / 18'000.0);
    v.band_width = std::min(0.12 * (1.0 + height / 2'500.0), 0.6);
    v.band_latitude = std::clamp(-dmath::atan2(y, band_distance_ly), -0.8, 0.8);
    v.dust = dmath::exp(-height / 4'000.0) * (radius < 60'000.0 ? 1.0 : 0.6);
    const double ring = (radius - ring_ly) / ring_ly;
    v.nebulae = 1.4 * dmath::exp(-height / 1'500.0) * (0.5 + dmath::exp(-ring * ring));
    return v;
}

render::Backdrop system_backdrop(const UniverseConfig& config) {
    return render::generate_backdrop(hash(config.seed, hash_id("space:backdrop"), config.system), galaxy_view(config));
}

}  // namespace veach::space
