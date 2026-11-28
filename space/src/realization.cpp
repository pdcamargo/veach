// veach: canonical (realization.hpp). Scale enters here and nowhere upstream: the stages below get no scale factor.

#include <veach/space/realization.hpp>

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/procgen/field_graph.hpp>
#include <veach/procgen/stage.hpp>
#include <veach/space/clock.hpp>
#include <veach/space/gen/stages.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>

namespace veach::space {

namespace sg = ::space::gen;

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double dobson = 2.6867e20;  ///< molecules / m² per Dobson unit

/// 2π √(a³ / GM), seconds.
double period_seconds(double a, double gm) {
    return 2.0 * pi * std::sqrt(a * a * a / gm);
}

/// s^e, exactly √s for the default exponent.
double power(double s, double e) {
    return e == 0.5 ? std::sqrt(s) : dmath::pow(s, e);
}

Error settings_error(std::string message) {
    return Error{ErrorCode::universe_settings_invalid, std::move(message),
                 "Keep scale (s_p) in [0.01, 1], orbit_scale at 0 or in [scale, 1], the exponents in [0, 1] and the "
                 "multipliers above 0 (docs/specs/first-slice/spec.md)."};
}

/// Aerosol optics per kind (SP-10's three looks): albedo, extinction relative to 550 nm, phase g.
Aerosol aerosol_of(const sg::AtmosphereComposition& a) {
    Aerosol out{.optical_depth = a.aerosol_optical_depth, .scale_height = a.aerosol_scale_height_m};
    if (a.aerosol == sg::aerosol_dust) {
        out.albedo = {0.97, 0.86, 0.62};
        out.g = 0.65;
    } else if (a.aerosol == sg::aerosol_haze) {
        out.albedo = {0.96, 0.88, 0.62};
        out.relative_extinction = {0.8, 1.0, 1.3};
        out.g = 0.6;
    } else {
        out.albedo = math::dvec3(3.996 / 4.436);  // Hillaire 2020's clear-sky aerosol
        out.g = 0.8;
    }
    return out;
}

/// The renderer's composition from the stage's record (physical).
Composition composition_of(const sg::AtmosphereComposition& a, double gravity, double radius) {
    Composition c;
    for (std::uint32_t i = 0; i < a.gas_count; ++i) {
        c.gases.push_back({a.gases.at(i).gas, a.gases.at(i).fraction});
    }
    c.surface_pressure = a.surface_pressure_pa;
    c.temperature = a.surface_temperature_k;
    c.gravity = gravity;
    c.radius = radius;
    c.aerosol = aerosol_of(a);
    if (a.ozone_column_du > 0.0) {
        c.absorber = earth_air().absorber;
        c.absorber.column = a.ozone_column_du * dobson;
    } else {
        c.absorber = {};
    }
    c.ground_albedo = a.kind == sg::atmosphere_temperate ? math::dvec3(0.3) : math::dvec3(0.33, 0.24, 0.16);
    return c;
}

}  // namespace

Result<Scales> scale_factors(const UniverseConfig& c) {
    if (!(c.scale >= 0.01 && c.scale <= 1.0)) {
        return std::unexpected(settings_error(std::format("scale (s_p) is {}, outside [0.01, 1]", c.scale)));
    }
    const double s_o = c.orbit_scale == 0.0 ? c.scale : c.orbit_scale;
    if (!(s_o >= c.scale && s_o <= 1.0)) {
        return std::unexpected(settings_error(
            std::format("orbit_scale (s_o) is {}; it must be in [s_p = {}, 1] (ADR 0068)", s_o, c.scale)));
    }
    if (!(c.atmosphere_exponent >= 0.0 && c.atmosphere_exponent <= 1.0) ||
        !(c.relief_exponent >= 0.0 && c.relief_exponent <= 1.0)) {
        return std::unexpected(
            settings_error(std::format("atmosphere_exponent {} or relief_exponent {} is outside [0, 1]",
                                       c.atmosphere_exponent, c.relief_exponent)));
    }
    if (!(c.gravity_multiplier > 0.0) || !(c.day_multiplier > 0.0)) {
        return std::unexpected(settings_error(std::format("gravity_multiplier {} and day_multiplier {} must be above 0",
                                                          c.gravity_multiplier, c.day_multiplier)));
    }
    return Scales{.s_p = c.scale, .s_o = s_o, .s_a = power(c.scale, c.atmosphere_exponent)};
}

Result<StarSystem> realize(const UniverseConfig& config) {
    auto scales = scale_factors(config);
    if (!scales) {
        return std::unexpected(scales.error());
    }
    const auto address = sg::SystemAddress{config.system};
    const sg::SystemCoords coords = address.decode();
    if (coords.layer >= sg::address_layers || (config.system & ((std::uint64_t{1} << sg::body_bits) - 1u)) != 0) {
        return std::unexpected(Error{ErrorCode::universe_system_invalid,
                                     std::format("'{}' is not a system address", sg::to_string(address)),
                                     "Give the system as S:<16 hex digits> with a zero body field (ADR 0050)."});
    }

    // The stages, on the engine's stage runtime. No scale factor goes in.
    procgen::StageRegistry registry;
    auto stages = sg::register_stages(registry, {.seed = 0, .habitability_bias = config.habitability_bias});
    if (!stages) {
        return std::unexpected(stages.error());
    }
    procgen::StageRuntime runtime(registry, {.universe_seed = config.seed});
    auto universe = runtime.get(stages->universe, procgen::Address{0});
    auto record = runtime.get(stages->system, address.value);
    auto physics = runtime.get(stages->body_physics, address.value);
    auto air = runtime.get(stages->atmosphere, address.value);
    for (const Error* e : {universe ? nullptr : &universe.error(), record ? nullptr : &record.error(),
                           physics ? nullptr : &physics.error(), air ? nullptr : &air.error()}) {
        if (e != nullptr) {
            return std::unexpected(*e);
        }
    }

    StarSystem out;
    out.config = config;
    out.scale = *scales;
    out.record = **record;
    out.physics = **physics;
    out.atmospheres = **air;
    out.hashes = {.universe = sg::stage_hash(**universe),
                  .system = sg::stage_hash(out.record),
                  .body_physics = sg::stage_hash(out.physics),
                  .atmosphere = sg::stage_hash(out.atmospheres)};
    out.star_luminosity = out.record.star.luminosity_w;
    out.star_temperature = out.record.star.temperature_k;
    out.orbits = System(sg::to_string(address));

    const double s_p = scales->s_p, s_o = scales->s_o, s_a = scales->s_a;
    const double g_mult = config.gravity_multiplier;

    // The star: radius × s_o, GM ∝ s_o² (ADR 0049).
    RealizedBody star;
    star.index = 0;
    star.name = "star";
    star.kind = out.record.star.kind;
    star.radius = out.record.star.radius_m * s_o;
    star.gm = sg::si::G * out.record.star.mass_kg * s_o * s_o * g_mult;
    star.surface_gravity = star.gm / (star.radius * star.radius);
    star.sphere_of_influence = std::numeric_limits<double>::infinity();
    star.address = address.value;
    {
        OrbitNode n;
        n.name = star.name;
        n.gm = star.gm;
        n.radius = star.radius;
        star.node = out.orbits.add(std::move(n), "space:fixed");
    }
    out.bodies.push_back(std::move(star));

    for (const sg::BodyRecord& b : out.record.bodies) {
        const sg::BodyPhysics& p = out.physics.at(b.index - 1);
        RealizedBody body;
        body.index = b.index;
        body.name = std::format("b{}", b.index);
        body.parent = b.parent;
        body.kind = b.composition;
        body.solid = b.composition == "space:composition.rocky" || b.composition == "space:composition.icy";
        body.radius = b.radius_m * s_p;
        body.surface_gravity = p.surface_gravity_m_s2 * g_mult;
        body.gm = body.surface_gravity * body.radius * body.radius;
        body.address = address.body(b.index).value;
        body.terrain_seed = procgen::stage_seed(config.seed, body.address, "space:body.terrain", 1);

        // Planets orbit the star at a × s_o; moons their planet at a × s_p. Periods from the gameplay GMs.
        const RealizedBody& parent = *out.body(b.parent);
        const double a = b.orbit.semi_major_axis_m * (b.parent == 0 ? s_o : s_p);
        const double orbit_s = period_seconds(a, parent.gm + body.gm);
        body.sphere_of_influence = a * dmath::pow(body.gm / parent.gm, 0.4);

        // Spin: the physical sidereal day × the multiplier; a tidally locked body keeps its face to the parent.
        OrbitNode n;
        n.name = body.name;
        n.parent = parent.node;
        n.elements = {.a = a,
                      .e = b.orbit.eccentricity,
                      .i = b.orbit.inclination_rad,
                      .node = b.orbit.ascending_node_rad,
                      .arg_periapsis = b.orbit.argument_of_periapsis_rad,
                      .mean_anomaly = b.orbit.mean_anomaly_at_epoch_rad};
        n.period_q = period_q_from_seconds(orbit_s);
        n.gm = body.gm;
        n.radius = body.radius;
        n.spin = {.period_q = b.tidally_locked ? n.period_q
                                               : period_q_from_seconds(b.sidereal_rotation_s * config.day_multiplier),
                  .epoch_tick = 0,
                  .phase0 = 0.0,
                  .obliquity = b.obliquity_rad,
                  .pole_longitude = 0.0};
        const double day_s = seconds_from_period_q(n.spin.period_q);
        body.equatorial_speed_ratio = (2.0 * pi * body.radius / day_s) / std::sqrt(body.surface_gravity * body.radius);
        body.day = body.equatorial_speed_ratio > 0.7   ? DayCheck::refuse
                   : body.equatorial_speed_ratio > 0.3 ? DayCheck::warn
                                                       : DayCheck::ok;
        body.node = out.orbits.add(std::move(n), "space:kepler_elliptic");

        // Planetary-band relief: 6 km on planets, 3 km on moons (physical), × s_p^relief_exponent.
        if (body.solid) {
            body.relief = (b.parent == 0 ? 6000.0 : 3000.0) * power(s_p, config.relief_exponent);
        }

        // The atmosphere from its composition, then realized at (s_p, s_a).
        const sg::AtmosphereComposition& air_record = out.atmospheres.at(b.index - 1);
        if (air_record.present()) {
            const AtmosphereRecord physical =
                atmosphere_from(composition_of(air_record, body.surface_gravity, b.radius_m));
            body.atmosphere = scaled(physical, s_p, s_a);
            body.atmosphere_top = body.atmosphere->top - body.atmosphere->radius;
        }
        out.bodies.push_back(std::move(body));
    }
    return out;
}

const RealizedBody* StarSystem::find(std::string_view name) const {
    const auto it = std::ranges::find(bodies, name, &RealizedBody::name);
    return it == bodies.end() ? nullptr : &*it;
}

const RealizedBody* StarSystem::body(std::uint32_t index) const {
    return index < bodies.size() && bodies[index].index == index ? &bodies[index] : nullptr;
}

const RealizedBody* StarSystem::home() const {
    for (const RealizedBody& b : bodies) {
        if (b.index != 0 && b.parent == 0 && physics.at(b.index - 1).potentially_habitable) {
            return &b;
        }
    }
    for (const RealizedBody& b : bodies) {
        if (b.index != 0 && b.parent == 0 && b.solid) {
            return &b;
        }
    }
    return bodies.size() > 1 ? &bodies[1] : nullptr;
}

std::uint64_t StarSystem::realized_hash() const {
    std::uint64_t h = hash_id("space:realized");
    const auto add = [&h](double x) {
        h = hash(h, std::bit_cast<std::uint64_t>(x));
    };
    for (const RealizedBody& b : bodies) {
        h = hash(h, std::uint64_t{b.index});
        for (const double x : {b.radius, b.gm, b.relief, b.atmosphere_top, b.sphere_of_influence}) {
            add(x);
        }
    }
    for (const OrbitNode& n : orbits.nodes()) {
        add(n.elements.a);
        h = hash(h, static_cast<std::uint64_t>(n.period_q), static_cast<std::uint64_t>(n.spin.period_q));
    }
    return h;
}

math::dvec3 gravity(const RealizedBody& body, const math::dvec3& r) {
    const double d = std::sqrt(math::dot(r, r));
    if (d >= body.radius) {
        return r * (-body.gm / (d * d * d));
    }
    return r * (-body.gm / (body.radius * body.radius * body.radius));
}

double surface_radius(const voxel::ProgramDensity& density, const math::dvec3& n) {
    const double top = density.max_extent();
    double lo = std::max(0.0, 2.0 * density.settings().radius_m - top - 100.0), hi = top;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        (density.distance(n.x * mid, n.y * mid, n.z * mid) < 0.0 ? lo : hi) = mid;
    }
    return lo;
}

}  // namespace veach::space
