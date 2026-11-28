#include <veach/space/mcp_tools.hpp>

#include <veach/space/clock.hpp>
#include <veach/space/gen/address.hpp>
#include <veach/space/gen/system.hpp>
#include <veach/space/orbit.hpp>
#include <veach/space/orrery.hpp>
#include <veach/space/presets.hpp>
#include <veach/space/sun.hpp>

#include <veach/runtime/mcp_tools.hpp>

#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace veach::space {

namespace {

using nlohmann::json;
namespace sg = ::space::gen;

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;
constexpr std::uint64_t exact_json_integer = std::uint64_t{1} << 53;

std::unexpected<Error> invalid(std::string message, std::string hint = {}) {
    return make_error(ErrorCode::mcp_invalid_arguments, std::move(message), std::move(hint));
}

std::string dec(std::uint64_t v) {
    return std::to_string(v);
}

/// A 64-bit argument: a decimal string (ADR 0046), or a JSON integer below 2^53 for convenience.
Result<std::uint64_t> u64_argument(const json& args, std::string_view name) {
    const json& v = args.at(std::string(name));
    if (v.is_number_unsigned() || (v.is_number_integer() && v.get<std::int64_t>() >= 0)) {
        const auto value = v.get<std::uint64_t>();
        if (value < exact_json_integer) {
            return value;
        }
    }
    if (v.is_string()) {
        const std::string text = v.get<std::string>();
        std::uint64_t value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (!text.empty() && ec == std::errc{} && end == text.data() + text.size()) {
            return value;
        }
    }
    return invalid(std::format("'{}' must be a non-negative 64-bit integer as a decimal string", name),
                   std::format("For example \"{}\": \"42\" (ADR 0046: 64-bit values cross MCP as strings).", name));
}

Result<double> number_argument(const json& args, std::string_view name, double fallback) {
    if (!args.contains(name)) {
        return fallback;
    }
    const json& v = args.at(std::string(name));
    if (!v.is_number() || !std::isfinite(v.get<double>())) {
        return invalid(std::format("'{}' must be a number", name));
    }
    return v.get<double>();
}

json vec3(math::dvec3 v) {
    return json::array({v.x, v.y, v.z});
}

std::vector<std::string> node_names(const System& system) {
    std::vector<std::string> names;
    for (const OrbitNode& n : system.nodes()) {
        names.push_back(n.name);
    }
    return names;
}

// ---- orrery_state ----

json orrery_state_schema() {
    std::vector<std::string> systems(preset_names().begin(), preset_names().end());
    return {{"type", "object"},
            {"properties",
             {{"system",
               {{"type", "string"},
                {"enum", systems},
                {"description", "The preset. Default: the world's Orrery, else \"sol\"."}}},
              {"scale",
               {{"type", "number"},
                {"exclusiveMinimum", 0},
                {"description", "s_p = s_o (ADR 0049). Default: the world's Orrery's, else 1."}}},
              {"tick",
               {{"type", "string"},
                {"pattern", "^[0-9]+$"},
                {"description",
                 "Universe tick (60 per second) as a decimal string, in [0, 2^53). Default: the world's orrery clock "
                 "for its system, else J2000.0 for sol (\"378683424000\") and 0 for the others."}}},
              {"body",
               {{"type", "string"},
                {"description",
                 "Only this body (\"earth\", \"moon\", \"planet\"...), plus the star seen from latitude/longitude on "
                 "its surface."}}},
              {"latitude", {{"type", "number"}, {"minimum", -90}, {"maximum", 90}, {"default", 0}}},
              {"longitude", {{"type", "number"}, {"default", 0}}}}},
            {"additionalProperties", false}};
}

Result<McpToolOutput> orrery_state(World& world, const json& args) {
    // The world's orrery: the first Orrery entity by id (the spike supports one per world).
    Entity orrery;
    world.each([&](Entity e, const Orrery&) {
        if (!orrery.is_valid() || e.id() < orrery.id()) {
            orrery = e;
        }
    });
    const Orrery* in_world = orrery.is_valid() ? orrery.try_get<Orrery>() : nullptr;

    std::string system_name = in_world != nullptr ? in_world->system : "sol";
    if (args.contains("system")) {
        if (!args["system"].is_string()) {
            return invalid("'system' must be a string");
        }
        system_name = args["system"].get<std::string>();
    }
    const bool from_world = in_world != nullptr && in_world->system == system_name;
    auto scale = number_argument(args, "scale", from_world ? in_world->scale : 1.0);
    if (!scale) {
        return std::unexpected(scale.error());
    }
    if (*scale <= 0.0) {
        return invalid("'scale' must be positive");
    }
    std::optional<System> system = preset(system_name, *scale);
    if (!system) {
        const std::vector<std::string> names(preset_names().begin(), preset_names().end());
        const std::string hint = did_you_mean(system_name, names);
        return invalid(std::format("unknown system '{}'", system_name),
                       hint.empty() ? "Systems: sol, earthlike, reference." : hint);
    }

    std::int64_t tick = system_name == "sol" ? sol_j2000_tick : 0;
    std::string source = "preset";
    if (from_world) {
        const UniverseClock* clock = orrery.try_get<UniverseClock>();
        tick = clock != nullptr ? clock->tick : 0;
        source = std::format("the orrery '{}'", orrery.path("::", "").c_str());
    }
    if (args.contains("tick")) {
        auto t = u64_argument(args, "tick");
        if (!t) {
            return std::unexpected(t.error());
        }
        if (*t >= static_cast<std::uint64_t>(tick_limit)) {
            return invalid(std::format("tick {} is out of range", *t), "Ticks are in [0, 2^53) (ADR 0047).");
        }
        tick = static_cast<std::int64_t>(*t);
    }

    std::int32_t only = -1;
    if (args.contains("body")) {
        if (!args["body"].is_string()) {
            return invalid("'body' must be a string");
        }
        const std::string body = args["body"].get<std::string>();
        only = system->find(body);
        if (only < 0) {
            const std::string hint = did_you_mean(body, node_names(*system));
            return invalid(std::format("no body '{}' in '{}'", body, system_name),
                           hint.empty() ? std::format("Bodies: {}.", json(node_names(*system)).dump()) : hint);
        }
    }
    auto latitude = number_argument(args, "latitude", 0.0);
    auto longitude = number_argument(args, "longitude", 0.0);
    if (!latitude || !longitude) {
        return std::unexpected(!latitude ? latitude.error() : longitude.error());
    }
    if (*latitude < -90.0 || *latitude > 90.0) {
        return invalid("'latitude' must be in [-90, 90] degrees");
    }

    const std::int32_t star = primary_star(*system);
    std::vector<Pose> poses(system->nodes().size());
    system->evaluate(tick, poses);
    const math::dvec3 star_position = poses[static_cast<std::size_t>(star)].position;

    json bodies = json::array();
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(system->nodes().size()); ++i) {
        if (only >= 0 && i != only) {
            continue;
        }
        const OrbitNode& n = system->node(i);
        const Pose& p = poses[static_cast<std::size_t>(i)];
        const math::dvec3 parent = n.parent >= 0 ? poses[static_cast<std::size_t>(n.parent)].position : p.position;
        const math::dvec3 to_parent = p.position - parent;
        const math::dvec3 to_star = p.position - star_position;
        json body = {{"name", n.name},
                     {"parent", n.parent >= 0 ? json(system->node(n.parent).name) : json(nullptr)},
                     {"model", n.model != nullptr ? json(n.model->id) : json(nullptr)},
                     {"position", vec3(p.position)},
                     {"velocity", vec3(p.velocity)},
                     {"distance_to_parent", std::sqrt(math::dot(to_parent, to_parent))},
                     {"distance_to_star", std::sqrt(math::dot(to_star, to_star))},
                     {"radius", n.radius}};
        // Orbit phase: the fraction of the orbital period (the mean longitude's, for kepler_secular) since the
        // elements' epoch, exact (ADR 0047).
        body["orbit"] = n.period_q > 0 ? json{{"period_s", seconds_from_period_q(n.period_q)},
                                              {"phase", phase(tick, n.epoch_tick, n.period_q)}}
                                       : json(nullptr);
        if (n.spin.period_q > 0) {
            json spin = {{"sidereal_period_s", seconds_from_period_q(n.spin.period_q)},
                         {"phase", phase(tick, n.spin.epoch_tick, n.spin.period_q)}};
            if (i != star) {
                const SunSample noon = sun_local(*system, i, star, 0.0, 0.0, tick);
                const SubsolarPoint sub = subsolar_point(*system, i, star, tick);
                spin["local_solar_time_at_prime_meridian"] = noon.local_solar_time;
                spin["subsolar_point"] = {{"latitude", sub.latitude / deg}, {"longitude", sub.longitude / deg}};
            }
            body["spin"] = std::move(spin);
        } else {
            body["spin"] = nullptr;
        }
        bodies.push_back(std::move(body));
    }

    json out = {{"system", system_name},
                {"scale", *scale},
                {"tick", std::to_string(tick)},
                {"source", source},
                {"star", system->node(star).name},
                {"frame", "system frame: right-handed, +Y north of the reference plane, meters, m/s"},
                {"bodies", std::move(bodies)}};
    if (only >= 0 && only != star) {
        const SunSample s = sun_local(*system, only, star, *latitude * deg, *longitude * deg, tick);
        out["sun"] = {{"body", system->node(only).name}, {"latitude", *latitude},
                      {"longitude", *longitude},         {"elevation", s.elevation / deg},
                      {"azimuth", s.azimuth / deg},      {"local_solar_time", s.local_solar_time},
                      {"distance", s.distance},          {"direction_enu", vec3(s.local)}};
    }
    return McpToolOutput{std::move(out)};
}

// ---- generate_system ----

json generate_system_schema() {
    const json u64 = {{"type", "string"}, {"pattern", "^[0-9]+$"}};
    json seed = u64;
    seed["description"] = "The universe seed, a uint64 as a decimal string (ADR 0046), e.g. \"42\".";
    return {{"type", "object"},
            {"properties",
             {{"seed", seed},
              {"address",
               {{"type", "string"},
                {"pattern", "^S:[0-9a-f]{16}$"},
                {"description", "A system address (ADR 0050), e.g. \"S:3f2a91c07e5b0000\"."}}},
              {"habitability_bias",
               {{"type", "number"},
                {"minimum", 0},
                {"maximum", 1},
                {"default", 0.5},
                {"description", "The universe's habitability bias (ADR 0066)."}}}}},
            {"required", {"seed", "address"}},
            {"additionalProperties", false}};
}

json orbit_json(const sg::Orbit& o) {
    return {{"semi_major_axis_m", o.semi_major_axis_m},
            {"eccentricity", o.eccentricity},
            {"inclination_rad", o.inclination_rad},
            {"ascending_node_rad", o.ascending_node_rad},
            {"argument_of_periapsis_rad", o.argument_of_periapsis_rad},
            {"mean_anomaly_at_epoch_rad", o.mean_anomaly_at_epoch_rad}};
}

json physics_json(const sg::BodyPhysics& p) {
    return {{"bond_albedo", p.bond_albedo},
            {"surface_gravity_m_s2", p.surface_gravity_m_s2},
            {"escape_velocity_m_s", p.escape_velocity_m_s},
            {"instellation_w_m2", p.instellation_w_m2},
            {"equilibrium_temperature_k", p.equilibrium_temperature_k},
            {"lightest_retained_molar_mass", p.lightest_retained_molar_mass},
            {"retains_atmosphere", p.retains_atmosphere},
            {"in_habitable_zone", p.in_habitable_zone},
            {"potentially_habitable", p.potentially_habitable},
            {"temperature_class", p.temperature_class}};
}

Result<McpToolOutput> generate_system(World&, const json& args) {
    if (!args.contains("seed") || !args.contains("address")) {
        return invalid(std::format("missing argument '{}'", args.contains("seed") ? "address" : "seed"),
                       "generate_system { seed: \"42\", address: \"S:3f2a91c07e5b0000\" }");
    }
    auto seed = u64_argument(args, "seed");
    if (!seed) {
        return std::unexpected(seed.error());
    }
    const json& address_arg = args.at("address");
    const std::optional<sg::SystemAddress> address =
        address_arg.is_string() ? sg::parse_system_address(address_arg.get<std::string>()) : std::nullopt;
    if (!address) {
        return invalid(std::format("'{}' is not a system address", address_arg.dump()),
                       "A system address is \"S:\" and 16 lowercase hex digits whose low 9 bits (the body) are 0, "
                       "e.g. \"S:3f2a91c07e5b0000\" (ADR 0050).");
    }
    auto bias = number_argument(args, "habitability_bias", 0.5);
    if (!bias) {
        return std::unexpected(bias.error());
    }
    if (*bias < 0.0 || *bias > 1.0) {
        return invalid("'habitability_bias' must be in [0, 1]");
    }

    const sg::UniverseRecord universe{.seed = *seed, .habitability_bias = *bias};
    const sg::StageContext ctx{.universe = &universe, .address = *address};
    const sg::SystemRecord r = sg::generate_system(ctx);
    const std::vector<sg::BodyPhysics> physics = sg::derive_all_body_physics(ctx, r);
    const sg::Violations v = sg::validate(r, physics);

    const sg::SystemCoords c = address->decode();
    double origin[3] = {0.0, 0.0, 0.0};
    address->cell_origin_ly(origin);

    json bodies = json::array();
    for (const sg::BodyRecord& b : r.bodies) {
        json body = {{"index", b.index},
                     {"address", sg::to_string(address->body(b.index))},
                     {"parent", b.parent},
                     {"composition", b.composition},
                     {"mass_kg", b.mass_kg},
                     {"radius_m", b.radius_m},
                     {"density_kg_m3", b.density_kg_m3},
                     {"orbit", orbit_json(b.orbit)},
                     {"sidereal_rotation_s", b.sidereal_rotation_s},
                     {"obliquity_rad", b.obliquity_rad},
                     {"tidally_locked", b.tidally_locked}};
        for (const sg::BodyPhysics& p : physics) {
            if (p.index == b.index) {
                body["physics"] = physics_json(p);
            }
        }
        bodies.push_back(std::move(body));
    }

    json violations = json::array();
    const auto failed = [&](std::string_view check, std::uint64_t count) {
        if (count > 0) {
            violations.push_back(std::format("{}: {}", check, count));
        }
    };
    failed("planet_hill", v.planet_hill);
    failed("planet_star", v.planet_star);
    failed("moon_roche", v.moon_roche);
    failed("moon_hill", v.moon_hill);
    failed("moon_spacing", v.moon_spacing);
    failed("body_overflow", v.body_overflow);
    failed("non_finite", v.non_finite);

    const sg::StarRecord& s = r.star;
    json out = {
        {"address", sg::to_string(*address)},
        {"seed", dec(*seed)},
        {"habitability_bias", *bias},
        {"coords", {{"layer", c.layer}, {"x", c.x}, {"y", c.y}, {"z", c.z}, {"system", dec(c.system)}}},
        {"cell_origin_ly", json::array({origin[0], origin[1], origin[2]})},
        {"cell_edge_ly", address->cell_edge_ly()},
        {"stages",
         {{std::string(sg::system_stage_id), {{"version", sg::system_stage_version}, {"hash", dec(sg::stage_hash(r))}}},
          {std::string(sg::body_physics_stage_id),
           {{"version", sg::body_physics_stage_version}, {"hash", dec(sg::stage_hash(physics))}}}}},
        {"star",
         {{"kind", s.kind},
          {"spectral_class", s.spectral_class},
          {"initial_mass_kg", s.initial_mass_kg},
          {"mass_kg", s.mass_kg},
          {"age_s", s.age_s},
          {"metallicity_feh", s.metallicity_feh},
          {"zams_luminosity_w", s.zams_luminosity_w},
          {"luminosity_w", s.luminosity_w},
          {"radius_m", s.radius_m},
          {"temperature_k", s.temperature_k}}},
        {"disk_inner_m", r.disk_inner_m},
        {"disk_outer_m", r.disk_outer_m},
        {"snow_line_m", r.snow_line_m},
        {"hz_inner_m", r.hz_inner_m},
        {"hz_outer_m", r.hz_outer_m},
        {"habitability_anchor", r.habitability_anchor},
        {"planet_count", r.planet_count},
        {"bodies", std::move(bodies)},
        {"violations", std::move(violations)},
    };
    return McpToolOutput{std::move(out)};
}

}  // namespace

SpaceMcpModule::SpaceMcpModule(World& world) {
    world.module<SpaceMcpModule>();
    world.import<OrreryModule>();  // orrery_state reads the world's Orrery and UniverseClock
    Services* services = find_services(world);
    McpTools* tools = services != nullptr ? services->find<McpTools>() : nullptr;
    if (tools == nullptr) {
        log::debug("space", "no McpTools service (a world made outside an Engine): no space MCP tools");
        return;
    }
    const auto add = [&](McpTool tool) {
        if (auto added = tools->add(std::move(tool)); !added) {
            log::error("space", added.error());
        }
    };
    add({.name = "orrery_state",
         .description =
             "Where every body of an SP-04 star system is at a universe tick, in one call: system-frame position "
             "and velocity (m, m/s), distance to its parent and to the star, orbit phase, spin phase and the local "
             "solar time on its prime meridian. With 'body': only that body, plus the sun's elevation, azimuth and "
             "local solar time at latitude/longitude on it. Computed on the spot; the world is not changed.",
         .input_schema = orrery_state_schema(),
         .handler = orrery_state});
    add({.name = "generate_system",
         .description = "Generate one star system with SP-05's stages from a universe seed and a system address: the "
                        "S1 'space:system' record (star, disk, habitable zone, planets and moons with orbits) and the "
                        "B1 'space:body.physics' record of each body, with both stage hashes. Physical, unscaled SI "
                        "units. 64-bit values are decimal strings.",
         .input_schema = generate_system_schema(),
         .handler = generate_system});
}

}  // namespace veach::space
