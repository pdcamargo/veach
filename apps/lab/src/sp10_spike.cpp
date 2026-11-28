#include "sp10_spike.hpp"

#include <veach/core/env.hpp>
#include <veach/render/atmosphere.hpp>
#include <veach/render/renderer.hpp>
#include <veach/space/atmosphere.hpp>
#include <veach/space/presets.hpp>
#include <veach/space/sun.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace lab {

namespace {

namespace math = veach::math;
namespace vs = veach::space;

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;

double number(const char* name, double fallback) {
    const std::optional<std::string> text = veach::env(name);
    return text ? std::strtod(text->c_str(), nullptr) : fallback;
}

/// A sun in the planet's local frame: direction (east, north, up), illuminance (rgb lux), angular radius.
struct LocalSun {
    math::dvec3 enu{0.0, 0.0, 1.0};
    math::dvec3 illuminance{0.0};
    double angular_radius = 0.004625;
};

math::dvec3 enu(double elevation, double azimuth) {  // azimuth from north toward east
    return {std::cos(elevation) * std::sin(azimuth), std::cos(elevation) * std::cos(azimuth), std::sin(elevation)};
}

/// A 4400 K star's color at unit luminance (Rec. 709), roughly.
constexpr math::dvec3 k_star{1.316, 0.947, 0.592};

struct Config {
    std::string composition;
    double s_p = 1.0;
    double s_a = 1.0;
    vs::AtmosphereRecord record;  ///< scaled
    double altitude = 2.0;        ///< m
    bool orbit = false;
    std::vector<LocalSun> suns;
    std::optional<double> eclipse;
    std::uint32_t march = 0;
    std::uint32_t steps = 32;
    double pitch = 0.0;  ///< radians
    double yaw = 0.0;    ///< radians from north toward east
    double fov = 60.0 * deg;
    double ev100 = 15.0;
    bool helmet = false;
    bool rebuild = false;  ///< nudge the medium each frame: times the transmittance and multiple-scattering LUTs
    std::uint32_t width = 960, height = 540;
    // The local frame at the camera's ground point, world: up is tilted off +Y so nothing relies on world Y.
    math::dvec3 up, north, east;
};

/// Two suns from the system query: the binary of space::reference_system() seen from its circumbinary planet, at
/// the first tick of its day where star A sets through 4 degrees at latitude 20.
std::vector<LocalSun> system_suns() {
    const vs::System system = vs::reference_system();
    const std::int32_t planet = system.find("circumbinary");
    const std::int32_t a = system.find("star_a");
    const std::int32_t b = system.find("star_b");
    const double latitude = 20.0 * deg;
    const double longitude = 0.0;
    const std::int64_t day = 30 * vs::ticks_per_day / 24;
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    // Find noon-ish (A highest) then bisect for A at 4 degrees, descending.
    std::int64_t best = 0;
    double best_elevation = -10.0;
    for (std::int64_t t = 0; t < day; t += day / 240) {
        const double e = vs::sun_local(system, planet, a, latitude, longitude, t).elevation;
        if (e > best_elevation) {
            best_elevation = e;
            best = t;
        }
    }
    lo = best;
    hi = best + day / 2;
    while (hi - lo > 1) {
        const std::int64_t mid = lo + (hi - lo) / 2;
        (vs::sun_local(system, planet, a, latitude, longitude, mid).elevation > 4.0 * deg ? lo : hi) = mid;
    }
    std::vector<LocalSun> suns;
    for (const auto& [star, luminosity, color] :
         {std::tuple{a, 1.0, math::dvec3(1.0)}, std::tuple{b, 0.13, k_star}}) {  // B: 0.6 solar masses, L ~ M^4
        const vs::SunSample s = vs::sun_local(system, planet, star, latitude, longitude, lo);
        const double au_over_d = vs::au / s.distance;
        suns.push_back({.enu = s.local,
                        .illuminance = color * (128000.0 * luminosity * au_over_d * au_over_d),
                        .angular_radius = system.node(star).radius / s.distance});
        veach::log::info("sp10",
                         "system sun {}: elevation {:.3f} deg, azimuth {:.3f} deg, {:.0f} lux, radius {:.4f} deg",
                         system.node(star).name, s.elevation / deg, s.azimuth / deg,
                         128000.0 * luminosity * au_over_d * au_over_d, system.node(star).radius / s.distance / deg);
    }
    return suns;
}

Config configure(const std::string& composition) {
    Config c;
    c.composition = composition;
    c.s_p = number("VEACH_SP10_SCALE", 1.0);
    const vs::Composition air = composition == "mars"    ? vs::mars_air()
                                : composition == "titan" ? vs::titan_air()
                                                         : vs::earth_air();
    // The star at the body's distance: 1, 1.524 and 9.58 AU (illuminance follows physical distance, ADR 0049).
    const double distance_au = composition == "mars" ? 1.524 : composition == "titan" ? 9.58 : 1.0;
    vs::AtmosphereRecord physical = vs::atmosphere_from(air);
    if (composition == "hillaire") {  // ADR 0038's Earth (Hillaire 2020's defaults), for comparison
        physical = {.radius = 6360e3,
                    .top = 6460e3,
                    .rayleigh = {5.802e-6, 13.558e-6, 33.1e-6},
                    .rayleigh_height = 8000.0,
                    .mie_scattering = math::dvec3(3.996e-6),
                    .mie_absorption = math::dvec3(4.40e-7),
                    .mie_height = 1200.0,
                    .mie_g = 0.8,
                    .absorption = {0.650e-6, 1.881e-6, 0.085e-6},
                    .absorption_center = 25000.0,
                    .absorption_half_width = 15000.0,
                    .ground_albedo = math::dvec3(0.3)};
    }
    // s_a = sqrt(s_p) (ADR 0049) unless VEACH_SP10_SA_EXPONENT asks for s_p^e, to compare.
    c.s_a = std::pow(c.s_p, number("VEACH_SP10_SA_EXPONENT", 0.5));
    c.record = vs::scaled(physical, c.s_p, c.s_a);
    const double thickness = c.record.top - c.record.radius;
    const std::string altitude = veach::env("VEACH_SP10_ALTITUDE").value_or("2");
    if (altitude == "orbit") {
        c.orbit = true;
        c.altitude = 2.0 * c.record.radius;
    } else if (altitude == "top-") {
        c.altitude = thickness * 0.995;
    } else if (altitude == "top+") {
        c.altitude = thickness * 1.005;
    } else {
        c.altitude = std::strtod(altitude.c_str(), nullptr);
    }

    const double sun_e = 128000.0 / (distance_au * distance_au);
    const double sun_r = 0.004625 / distance_au;
    const std::string suns = veach::env("VEACH_SP10_SUNS").value_or("one");
    if (suns == "system") {
        c.suns = system_suns();
    } else if (suns == "wide" || suns == "four") {
        c.suns = {{enu(30.0 * deg, 40.0 * deg), math::dvec3(sun_e), sun_r},
                  {enu(3.0 * deg, -70.0 * deg), k_star * (0.3 * sun_e), sun_r * 0.8}};
        if (suns == "four") {  // for the cost per sun
            c.suns.push_back({enu(15.0 * deg, 160.0 * deg), math::dvec3(0.2 * sun_e), sun_r});
            c.suns.push_back({enu(50.0 * deg, -120.0 * deg), math::dvec3(0.1 * sun_e), sun_r});
        }
    } else {
        c.suns = {{enu(number("VEACH_SP10_SUN_ELEVATION", 25.0) * deg, number("VEACH_SP10_SUN_AZIMUTH", 60.0) * deg),
                   math::dvec3(sun_e), sun_r}};
    }
    if (veach::env("VEACH_SP10_ECLIPSE")) {
        c.eclipse = number("VEACH_SP10_ECLIPSE", 0.0);
    }
    const std::string march = veach::env("VEACH_SP10_MARCH").value_or("auto");
    c.march = march == "always" ? 1u : march == "never" ? 2u : 0u;
    c.steps = static_cast<std::uint32_t>(number("VEACH_SP10_STEPS", 32.0));
    c.fov = number("VEACH_SP10_FOV", 60.0) * deg;
    c.ev100 = number("VEACH_SP10_EV", 15.0);
    c.helmet = veach::env_is_one("VEACH_SP10_HELMET");
    c.rebuild = veach::env_is_one("VEACH_SP10_REBUILD");
    if (const std::optional<std::string> size = veach::env("VEACH_SP10_SIZE")) {
        char* end = nullptr;
        c.width = static_cast<std::uint32_t>(std::strtoul(size->c_str(), &end, 10));
        c.height =
            static_cast<std::uint32_t>(std::strtoul(end != nullptr && *end == 'x' ? end + 1 : "540", nullptr, 10));
    }
    // Pitch: orbit looks at the center; otherwise the horizon sits 40% of the half-height below the image's center.
    const double r = c.record.radius;
    const double dip = std::acos(r / (r + std::max(c.altitude, 0.0)));
    c.pitch = c.orbit ? -90.0 * deg : -dip + std::atan(0.4 * std::tan(0.5 * c.fov));
    if (veach::env("VEACH_SP10_PITCH")) {
        c.pitch = number("VEACH_SP10_PITCH", 0.0) * deg;
    }
    // Yaw: north, or toward the system's first sun.
    c.yaw = suns == "system" ? std::atan2(c.suns.front().enu.x, c.suns.front().enu.y) : 0.0;
    if (veach::env("VEACH_SP10_YAW")) {
        c.yaw = number("VEACH_SP10_YAW", 0.0) * deg;
    }
    c.up = math::normalize(math::dvec3(0.25, 1.0, -0.15));
    c.north = math::normalize(math::dvec3(0.0, 0.0, -1.0) - c.up * math::dot(math::dvec3(0.0, 0.0, -1.0), c.up));
    c.east = math::cross(c.north, c.up);
    return c;
}

math::dvec3 world_of(const Config& c, math::dvec3 local) {
    return c.east * local.x + c.north * local.y + c.up * local.z;
}

/// The camera's view direction (world) and orientation.
math::dvec3 view_direction(const Config& c) {
    const math::dvec3 horizontal = c.north * std::cos(c.yaw) + c.east * std::sin(c.yaw);
    return math::normalize(horizontal * std::cos(c.pitch) + c.up * std::sin(c.pitch));
}

math::quat camera_rotation(const Config& c) {
    const math::dvec3 f = view_direction(c);
    const math::dvec3 reference = std::abs(math::dot(f, c.up)) > 0.999 ? c.north : c.up;
    const math::dvec3 right = math::normalize(math::cross(f, reference));
    const math::dvec3 up = math::cross(right, f);
    return math::quat_cast(math::mat3(math::vec3(right), math::vec3(up), math::vec3(-f)));
}

/// The occluder (moon) relative to the camera, meters, and its radius, for the first sun.
math::dvec4 occluder_of(const Config& c) {
    if (!c.eclipse || c.suns.empty()) {
        return math::dvec4(0.0);
    }
    const LocalSun& sun = c.suns.front();
    const double distance = 60.0 * c.record.radius;  // a moon's orbit follows s_p
    const double a = 1.15 * sun.angular_radius;      // a moon 15% larger than the sun's disk: a total eclipse
    // Turn the sun's direction toward local up by the separation (about the horizontal axis perpendicular to it).
    const math::dvec3 l = world_of(c, sun.enu);
    const math::dvec3 axis = math::normalize(math::cross(l, c.up));
    const double separation = *c.eclipse * (a + sun.angular_radius);
    const math::dvec3 toward = l * std::cos(separation) + math::cross(axis, l) * std::sin(separation) +
                               axis * math::dot(axis, l) * (1.0 - std::cos(separation));
    // From the ground point under the camera.
    const math::dvec3 center = -c.up * c.altitude + toward * distance;
    return {center, distance * std::sin(a)};
}

float km(double m) {
    return static_cast<float>(m * 1e-3);
}

}  // namespace

/// The renderer's medium (km) from the record (m): the record-to-render mapping ADR 0063 gives the renderer.
veach::render::AtmosphereParams sp10_medium_of(const vs::AtmosphereRecord& r) {
    const auto per_km = [](math::dvec3 v) {
        return math::vec3(v * 1e3);
    };
    veach::render::AtmosphereParams m;
    m.rayleigh = math::vec4(per_km(r.rayleigh), km(r.rayleigh_height));
    m.mie = math::vec4(per_km(r.mie_scattering), km(r.mie_height));
    m.mie_absorption = math::vec4(per_km(r.mie_absorption), static_cast<float>(r.mie_g));
    m.ozone = math::vec4(per_km(r.absorption), km(r.absorption_center));
    m.planet =
        math::vec4(km(r.radius), km(r.top), static_cast<float>(math::dot(r.ground_albedo, math::dvec3(1.0 / 3.0))),
                   km(r.absorption_half_width));
    return m;
}

namespace {

veach::render::AtmosphereParams medium_of(const vs::AtmosphereRecord& r) {
    return sp10_medium_of(r);
}

/// atmosphere_transmittance's frame: +Y up, from (east, north, up).
math::vec3 local_of(math::dvec3 enu) {
    return {static_cast<float>(enu.x), static_cast<float>(enu.z), static_cast<float>(enu.y)};
}

void extract(const Config& c, veach::render::RenderWorld& out) {
    namespace render = veach::render;
    const vs::AtmosphereRecord& r = c.record;
    render::PlanetSky p;
    p.medium = medium_of(r);
    p.center = math::vec3(-c.up * ((r.radius + c.altitude) * 1e-3));
    p.altitude = km(c.altitude);
    p.ground_albedo = math::vec3(r.ground_albedo);
    const math::dvec4 occ = occluder_of(c);
    p.occluder = math::vec4(math::vec3(math::dvec3(occ) * 1e-3), static_cast<float>(occ.w * 1e-3));
    p.march = c.march;
    p.march_steps = c.steps;

    // The suns replace the scene's directional lights: illuminance through the atmosphere at the camera, times the
    // fraction of the disk the moon leaves.
    std::erase_if(out.lights, [](const render::RenderLight& l) { return l.kind == veach::LightKind::directional; });
    const math::dvec3 moon_dir = occ.w > 0.0 ? math::normalize(math::dvec3(occ)) : math::dvec3(0.0);
    const double moon_radius = occ.w > 0.0 ? std::asin(occ.w / math::length(math::dvec3(occ))) : 0.0;
    for (std::size_t i = 0; i < c.suns.size() && i < p.suns.size(); ++i) {
        const LocalSun& sun = c.suns[i];
        const math::dvec3 l = world_of(c, sun.enu);
        p.suns.at(i) = {.direction = math::vec3(l),
                        .illuminance = math::vec3(sun.illuminance),
                        .angular_radius = static_cast<float>(sun.angular_radius)};
        ++p.sun_count;
        const math::vec3 t = render::atmosphere_transmittance(p.medium, p.altitude, local_of(sun.enu));
        double visible = 1.0;
        if (occ.w > 0.0 && math::dot(moon_dir, l) > 0.0) {
            visible = vs::disk_visible_fraction(sun.angular_radius, moon_radius,
                                                2.0 * std::asin(std::min(0.5 * math::length(moon_dir - l), 1.0)));
        }
        render::RenderLight light{.kind = veach::LightKind::directional,
                                  .direction = math::vec3(-l),
                                  .radiance = math::vec3(sun.illuminance * visible) * t};
        light.size = static_cast<float>(sun.angular_radius);
        out.lights.push_back(light);
    }
    out.sky.mode = veach::SkyMode::physical;
    if (c.rebuild) {
        p.medium.planet.z += (out.frame % 2 == 0) ? 1e-4f : 0.0f;
    }
    out.sky.planet = p;
}

}  // namespace

void register_sp10_spike(veach::World& world) {
    const std::optional<std::string> composition = veach::env("VEACH_SP10");
    if (!composition) {
        return;
    }
    const Config c = configure(*composition);
    const vs::AtmosphereRecord& r = c.record;
    const math::dvec3 tau = vs::vertical_optical_depth(r);
    veach::log::info("sp10",
                     "{} at s_p = {} (s_a = {:.4f}): radius {:.1f} km, top {:.2f} km above the ground, Rayleigh "
                     "height {:.3f} km, aerosol height {:.3f} km, vertical optical depth ({:.4f}, {:.4f}, {:.4f}); "
                     "camera at {:.4g} m, pitch {:.2f} deg",
                     c.composition, c.s_p, c.s_a, r.radius * 1e-3, (r.top - r.radius) * 1e-3, r.rayleigh_height * 1e-3,
                     r.mie_height * 1e-3, tau.x, tau.y, tau.z, c.altitude, c.pitch / deg);
    if (!c.suns.empty()) {
        const math::vec3 t =
            veach::render::atmosphere_transmittance(medium_of(r), km(c.altitude), local_of(c.suns.front().enu));
        veach::log::info("sp10",
                         "first sun at {:.2f} deg elevation: transmittance at the camera ({:.4f}, {:.4f}, {:.4f})",
                         std::asin(c.suns.front().enu.z) / deg, t.x, t.y, t.z);
    }

    world.entity("sp10_camera")
        .set<veach::Transform>({.translation = math::vec3(0.0f), .rotation = camera_rotation(c)})
        .set<veach::Camera>(
            {.fov_y = static_cast<float>(c.fov), .near_plane = 0.1f, .ev100 = static_cast<float>(c.ev100)});
    // The renderer draws nothing without a mesh: the helmet stands behind the camera, or in view with HELMET=1.
    const math::dvec3 f = view_direction(c);
    const math::dvec3 right = math::normalize(math::cross(f, std::abs(math::dot(f, c.up)) > 0.999 ? c.north : c.up));
    const math::dvec3 at = c.helmet ? f * 1.6 - c.up * 0.35 + right * 0.45 : -f * 3.0;
    world.entity("sp10_helmet")
        .set<veach::SceneInstance>({.path = "assets/flight_helmet/FlightHelmet.gltf"})
        .set<veach::Transform>(
            {.translation = math::vec3(at), .rotation = camera_rotation(c)});  // its face (+Z) toward the camera
    world.system<veach::Window>("Sp10Size")
        .kind<veach::phase::Startup>()
        .each([w = c.width, h = c.height](veach::Window& window) {
            window.width = w;
            window.height = h;
        });
    if (c.composition == "legacy") {
        return;  // the same view under the Sky component's atmosphere, for the timings
    }
    auto& renderer = veach::services(world).get<veach::render::Renderer>();
    renderer.add_extractor("sp10_planet_sky",
                           [c](const veach::World&, veach::render::RenderWorld& out) { extract(c, out); });
}

}  // namespace lab
