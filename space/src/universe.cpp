// The universe in a world (universe.hpp): frames on rails, terrain, gravity and the sky.

#include <veach/space/universe.hpp>

#include <veach/core/dmath.hpp>
#include <veach/physics/module.hpp>
#include <veach/space/atmosphere.hpp>
#include <veach/space/backdrop.hpp>
#include <veach/space/clock.hpp>
#include <veach/space/gen/address.hpp>
#include <veach/space/orrery.hpp>
#include <veach/space/sun.hpp>
#include <veach/space/surface.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

namespace veach::space {

namespace sg = ::space::gen;

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
/// Lux per W/m² of sunlight (128,000 lux at the solar constant's 1361 W/m², ADR 0038's sun).
constexpr double lux_per_watt = 128'000.0 / 1361.0;

math::dquat quat_of(const Orientation& o) {
    return math::normalize(math::quat_cast(math::dmat3(o.x, o.y, o.z)));  // columns: the body's axes
}

std::string key_of(const Universe& u) {
    return std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", u.seed, u.system, u.scale, u.orbit_scale,
                       u.atmosphere_exponent, u.relief_exponent, u.gravity, u.day, u.habitability_bias, u.home,
                       u.start_tick, u.surface_colors);
}

Result<UniverseConfig> config_of(const Universe& u) {
    UniverseConfig c;
    const char* end = u.seed.data() + u.seed.size();
    if (u.seed.empty() || std::from_chars(u.seed.data(), end, c.seed).ptr != end) {
        return std::unexpected(Error{ErrorCode::universe_settings_invalid,
                                     std::format("Universe.seed '{}' is not a decimal 64-bit integer", u.seed),
                                     "Write the seed as a string of digits, e.g. seed = \"42\" (ADR 0046)."});
    }
    const auto address = sg::parse_system_address(u.system);
    if (!address) {
        return std::unexpected(Error{ErrorCode::universe_system_invalid,
                                     std::format("Universe.system '{}' is not a system address", u.system),
                                     "Give it as S:<16 hex digits>, e.g. \"S:1000400100008000\" (ADR 0050)."});
    }
    c.system = address->value;
    c.scale = u.scale;
    c.orbit_scale = u.orbit_scale;
    c.atmosphere_exponent = u.atmosphere_exponent;
    c.relief_exponent = u.relief_exponent;
    c.gravity_multiplier = u.gravity;
    c.day_multiplier = u.day;
    c.habitability_bias = u.habitability_bias;
    return c;
}

void write_poses(World& world, UniverseWorld& u, std::int64_t tick) {
    const StarSystem& s = u.system();
    u.tick_ = tick;
    s.orbits.evaluate(tick, u.poses_);
    for (const RealizedBody& b : s.bodies) {
        const UniverseWorld::Frames& f = u.frames_.at(b.index);
        const Pose& p = u.poses_.at(static_cast<std::size_t>(b.node));
        Entity(world, f.orbit)
            .set(ReferenceFrame{.kind = FrameKind::inertial,
                                .up = FrameUp::radial,
                                .position = p.position,
                                .linear_velocity = p.velocity});
        const Orientation o = orientation(s.orbits.node(b.node).spin, tick);
        Entity(world, f.fixed)
            .set(ReferenceFrame{.kind = FrameKind::rotating,
                                .up = FrameUp::radial,
                                .orientation = quat_of(o),
                                .angular_velocity = o.y * u.spin_rate(b.index)});
    }
}

void build(World& world, Entity e, UniverseWorld& u, const Universe& settings) {
    const std::string key = key_of(settings);
    if (u.ready()) {
        if (u.key_ != key) {
            log::error("space", Error{ErrorCode::universe_settings_invalid,
                                      "the Universe was set again with other settings; the realized system stays",
                                      "Set Universe once, before the first step: scale and seed are genesis (ADR "
                                      "0055), changing them is a new universe."});
        }
        return;
    }
    const auto parsed_colors = parse_surface_colors(settings.surface_colors);
    if (!parsed_colors) {
        log::error("space", parsed_colors.error());
        return;
    }
    const SurfaceColors colors = *parsed_colors;
    auto config = config_of(settings);
    auto realized = config ? realize(*config) : Result<StarSystem>(std::unexpected(config.error()));
    if (!realized) {
        log::error("space", realized.error());
        return;
    }
    auto system = std::make_shared<const StarSystem>(std::move(*realized));
    const RealizedBody* home = settings.home.empty() ? system->home() : system->find(settings.home);
    if (home == nullptr || home->index == 0) {
        log::error("space", Error{ErrorCode::universe_system_invalid,
                                  std::format("the system has no body '{}' to be home", settings.home),
                                  "Name a planet or moon of the system (b1, b2, ...), or leave home empty for the "
                                  "first potentially habitable planet."});
        return;
    }
    u.system_ = system;
    u.backdrop_ = system_backdrop(system->config);
    u.home_ = home->index;
    u.key_ = key;
    u.poses_.assign(system->orbits.nodes().size(), Pose{});
    u.frames_.assign(system->bodies.size(), {});

    u.system_frame_ = world.entity().child_of(e).set_name("system").set(ReferenceFrame{});
    for (const RealizedBody& b : system->bodies) {
        const Entity orbit = world.entity()
                                 .child_of(u.system_frame_)
                                 .set_name(b.name.c_str())
                                 .set(CelestialFrame{.body = b.name, .index = b.index, .radius = b.radius});
        const Entity fixed = world.entity().child_of(orbit).set_name("fixed").set(
            CelestialFrame{.body = b.name, .index = b.index, .body_fixed = true, .radius = b.radius});
        u.frames_.at(b.index) = {orbit.id(), fixed.id()};
        u.body_by_frame_[orbit.id()] = b.index;
        u.body_by_frame_[fixed.id()] = b.index;
    }
    write_poses(world, u, settings.start_tick);
    e.set(UniverseClock{.tick = settings.start_tick, .rate = 1});

    // Terrain on home and its moons, in their body-fixed frames, each with its surface (surface.hpp): the game's layer
    // library (the TerrainLayers service) or the toolbox's procedural one. Only with a renderer: a server streams
    // the shapes alone.
    auto* bodies = services(world).find<TerrainBodies>();
    auto* store = services(world).find<render::MeshStore>();
    std::shared_ptr<const TerrainLayerLibrary> library;
    if (store != nullptr) {
        if (const auto* layers = services(world).find<TerrainLayers>(); layers != nullptr && layers->library) {
            library = layers->library;
        } else if (auto made = load_terrain_library(surface_layers(), {}, services(world).get<JobSystem>())) {
            library = *made;
        } else {
            log::error("space", made.error());
        }
    }
    for (const RealizedBody& b : system->bodies) {
        if (!b.solid || (b.index != home->index && b.parent != home->index)) {
            continue;
        }
        auto density = terrain_density(*system, b);
        if (!density) {
            log::error("space", density.error());
            continue;
        }
        u.densities_[b.index] = *density;
        voxel::TerrainSettings ts;
        ts.octree = voxel::OctreeBounds::around_origin((*density)->max_extent() + 16.0);
        ts.lambda = 4.0;
        auto terrain = std::make_shared<voxel::TerrainBody>(*density, ts);
        u.terrains_[b.index] = terrain;
        if (bodies == nullptr) {
            continue;
        }
        TerrainLook look;
        if (library != nullptr) {
            auto desc = surface_desc(*system, b, *library, colors);
            auto surface = desc ? TerrainSurface::make(library, std::move(*desc))
                                : Result<std::shared_ptr<const TerrainSurface>>(std::unexpected(desc.error()));
            auto material = surface ? (*surface)->material(*store) : Result<AssetId>(std::unexpected(surface.error()));
            if (material) {
                look = {.materials = {*material}, .surface = *surface};
                log::info("space", "{}: terrain surface {}", b.name, (*surface)->desc().name);
            } else {
                log::error("space", material.error());
            }
        }
        bodies->add(u.fixed_frame(b.index), terrain, std::move(look));
    }

    // Gravity: each body frame gets its own body's g R² (ADR 0059's dominant body, in that body's frame).
    if (physics::PhysicsWorld* physics = find_physics_world(world)) {
        const UniverseWorld* state = &u;
        physics->set_gravity([state](const physics::GravityQuery& q) {
            const RealizedBody* b = state->body_of_frame(q.frame);
            return b != nullptr ? gravity(*b, q.position) : math::dvec3(0.0);
        });
    }

    for (const RealizedBody& b : system->bodies) {
        if (b.day != DayCheck::ok) {
            log::warn("space",
                      "{}: the equator turns at {:.0f} % of the surface orbital speed (ADR 0049 {} above {} %)", b.name,
                      100.0 * b.equatorial_speed_ratio, b.day == DayCheck::refuse ? "refuses" : "warns",
                      b.day == DayCheck::refuse ? 70 : 30);
        }
    }
    const RealizedBody& h = *home;
    log::info("space",
              "universe {} system {}: s_p = {}, s_o = {}, s_a = {:.4f}; {} bodies; home {} ({}): R = {:.1f} km, g = "
              "{:.2f} m/s², day {:.2f} h, tilt {:.1f}°, atmosphere top {:.1f} km; stage hashes {:016x} {:016x} "
              "{:016x} {:016x}",
              settings.seed, settings.system, system->scale.s_p, system->scale.s_o, system->scale.s_a,
              system->bodies.size() - 1, h.name, h.kind, h.radius * 1e-3, h.surface_gravity,
              seconds_from_period_q(system->orbits.node(h.node).spin.period_q) / 3600.0,
              system->orbits.node(h.node).spin.obliquity * 180.0 / pi, h.atmosphere_top * 1e-3, system->hashes.universe,
              system->hashes.system, system->hashes.body_physics, system->hashes.atmosphere);
}

/// The sky: the body with an atmosphere nearest the camera (relative to its radius), the sun from the orbit tree.
void extract_sky(const World& world, const UniverseWorld& u, render::RenderWorld& out) {
    if (!u.ready() || out.views.empty()) {
        return;
    }
    const StarSystem& s = u.system();
    render::RenderView& view = out.views.front();
    const Entity render_frame = out.space.frame != 0 ? Entity(world, out.space.frame) : Entity{};
    const math::dvec3 camera = out.space.origin + math::dvec3(view.position);  // in the render frame
    const auto position_of = [&](std::uint64_t frame) -> std::optional<math::dvec3> {
        auto pose = frame_pose(world, Entity(world, frame), render_frame);
        if (!pose) {
            return std::nullopt;
        }
        return pose->position;
    };

    const RealizedBody* best = nullptr;
    math::dvec3 center{0.0};
    double best_score = std::numeric_limits<double>::infinity();
    for (const RealizedBody& b : s.bodies) {
        if (!b.atmosphere) {
            continue;
        }
        const auto p = position_of(u.frames_.at(b.index).orbit);
        if (!p) {
            continue;
        }
        const double score = (math::length(*p - camera) - b.radius) / b.radius;
        if (score < best_score) {
            best_score = score;
            best = &b;
            center = *p - camera;
        }
    }
    const auto star = position_of(u.frames_.at(0).orbit);
    if (!star) {
        return;
    }
    const math::dvec3 to_star = *star - camera;
    const double distance = math::length(to_star);
    const math::dvec3 sun = to_star / distance;
    const double physical = s.physical_distance(distance);
    const double lux = s.star_luminosity / (4.0 * pi * physical * physical) * lux_per_watt;
    const auto angular_radius = static_cast<float>(s.bodies.front().radius / distance);

    // The bodies that can eclipse the sun, for the sky to test per sample (ADR 0076 decision 2): the largest seen
    // from the camera, not the atmosphere's own body (the sky shades its night side) and not the star.
    struct Candidate {
        math::dvec3 center;  ///< camera-relative, m
        double radius = 0.0;
        double size = 0.0;  ///< angular radius
    };
    std::vector<Candidate> candidates;
    for (const RealizedBody& b : s.bodies) {
        if (b.index == 0 || &b == best || b.radius <= 0.0) {
            continue;
        }
        const auto p = position_of(u.frames_.at(b.index).orbit);
        if (!p) {
            continue;
        }
        const math::dvec3 c = *p - camera;
        const double d = math::length(c);
        if (d <= b.radius) {
            continue;
        }
        candidates.push_back({c, b.radius, b.radius / d});
    }
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) { return a.size > b.size; });
    if (candidates.size() > render::PlanetSky::max_occluders) {
        candidates.resize(render::PlanetSky::max_occluders);
    }
    // The sun's visible fraction at the camera: the input for lighting surfaces near it.
    double visible = 1.0;
    for (const Candidate& c : candidates) {
        if (math::dot(c.center, sun) <= 0.0) {
            continue;
        }
        const double separation = std::acos(std::clamp(math::dot(math::normalize(c.center), sun), -1.0, 1.0));
        visible *= disk_visible_fraction(static_cast<double>(angular_radius), std::asin(c.size), separation);
    }

    // The rotation of render space into the axes the renderer works in is the identity: render space has the camera
    // frame's axes, and the sky is camera-relative.
    out.sky.mode = SkyMode::physical;
    // The system's backdrop (ADR 0091), its galactic frame turned from the system frame's axes into render space's.
    render::RenderSpaceMap map(world, out.space);
    if (const auto rotation = map.rotation(u.system_frame().id())) {
        render::Backdrop backdrop = u.backdrop_;
        backdrop.orientation = math::mat3(*rotation) * backdrop.orientation;
        out.sky.backdrop = backdrop;
    }
    std::erase_if(out.lights, [](const render::RenderLight& l) { return l.kind == LightKind::directional; });
    math::vec3 transmittance(1.0f);
    if (best != nullptr) {
        const AtmosphereRecord& r = *best->atmosphere;
        render::PlanetSky p;
        p.medium = render_medium(r);
        p.center = math::vec3(center * 1e-3);
        const double altitude = math::length(center) - r.radius;
        p.altitude = static_cast<float>(altitude * 1e-3);
        p.ground_albedo = math::vec3(r.ground_albedo);
        p.suns.at(0) = {.direction = math::vec3(sun),
                        .illuminance = math::vec3(static_cast<float>(lux)),
                        .angular_radius = angular_radius};
        p.sun_count = 1;
        for (const Candidate& c : candidates) {
            p.occluders.at(p.occluder_count++) = {.center = math::vec3(c.center * 1e-3),
                                                  .radius = static_cast<float>(c.radius * 1e-3)};
        }
        out.sky.planet = p;
        // Through the air to the camera, in the local frame of atmosphere_transmittance (+Y up).
        const math::dvec3 up = -center / math::length(center);
        const double y = std::clamp(math::dot(sun, up), -1.0, 1.0);
        transmittance = render::atmosphere_transmittance(
            p.medium, std::max(p.altitude, 0.0f),
            math::vec3(static_cast<float>(std::sqrt(1.0 - y * y)), static_cast<float>(y), 0.0f));
    }
    render::RenderLight light{.kind = LightKind::directional,
                              .direction = math::vec3(-sun),
                              .radiance = math::vec3(static_cast<float>(lux * visible)) * transmittance};
    light.size = angular_radius;
    out.lights.push_back(light);
    render::AutoExposureSettings exposure;
    exposure.reset = out.frame <= 1;
    view.auto_exposure = exposure;
}

}  // namespace

Entity UniverseWorld::orbit_frame(std::uint32_t body) const {
    return body < frames_.size() ? Entity(system_frame_.world(), frames_[body].orbit) : Entity{};
}

Entity UniverseWorld::fixed_frame(std::uint32_t body) const {
    return body < frames_.size() ? Entity(system_frame_.world(), frames_[body].fixed) : Entity{};
}

const RealizedBody* UniverseWorld::body_of_frame(std::uint64_t frame) const {
    const auto it = body_by_frame_.find(frame);
    return it == body_by_frame_.end() ? nullptr : system_->body(it->second);
}

bool UniverseWorld::is_fixed_frame(std::uint64_t frame) const {
    const RealizedBody* b = body_of_frame(frame);
    return b != nullptr && frames_.at(b->index).fixed == frame;
}

Orientation UniverseWorld::orientation(std::uint32_t body, std::int64_t tick) const {
    return space::orientation(system_->orbits.node(system_->bodies.at(body).node).spin, tick);
}

double UniverseWorld::spin_rate(std::uint32_t body) const {
    const Spin& spin = system_->orbits.node(system_->bodies.at(body).node).spin;
    return spin.period_q > 0 ? 2.0 * pi / seconds_from_period_q(spin.period_q) : 0.0;
}

std::shared_ptr<const voxel::ProgramDensity> UniverseWorld::density(std::uint32_t body) const {
    const auto it = densities_.find(body);
    return it == densities_.end() ? nullptr : it->second;
}

voxel::TerrainBody* UniverseWorld::terrain(std::uint32_t body) const {
    const auto it = terrains_.find(body);
    return it == terrains_.end() ? nullptr : it->second.get();
}

UniverseWorld::LocalSun UniverseWorld::local_sun(std::uint32_t body, const math::dvec3& p, std::int64_t tick) const {
    LocalSun out;
    const double r = math::length(p);
    out.latitude = dmath::asin(std::clamp(p.y / r, -1.0, 1.0));
    out.longitude = dmath::atan2(-p.z, p.x);
    const SunSample s = sun_local(system_->orbits, system_->bodies.at(body).node, system_->bodies.front().node,
                                  out.latitude, out.longitude, tick);
    out.local_solar_time = s.local_solar_time;
    out.elevation = s.elevation;
    return out;
}

render::AtmosphereParams render_medium(const AtmosphereRecord& r) {
    const auto km = [](double m) {
        return static_cast<float>(m * 1e-3);
    };
    const auto per_km = [](math::dvec3 v) {
        return math::vec3(v * 1e3);
    };
    render::AtmosphereParams m;
    m.rayleigh = math::vec4(per_km(r.rayleigh), km(r.rayleigh_height));
    m.mie = math::vec4(per_km(r.mie_scattering), km(r.mie_height));
    m.mie_absorption = math::vec4(per_km(r.mie_absorption), static_cast<float>(r.mie_g));
    m.ozone = math::vec4(per_km(r.absorption), km(r.absorption_center));
    m.planet =
        math::vec4(km(r.radius), km(r.top), static_cast<float>(math::dot(r.ground_albedo, math::dvec3(1.0 / 3.0))),
                   km(r.absorption_half_width));
    return m;
}

UniverseModule::UniverseModule(World& world) {
    world.module<UniverseModule>();
    world.import<OrreryModule>();  // UniverseClock's reflection
    world.component<Universe>()
        .member(&Universe::seed, "seed")
        .member(&Universe::system, "system")
        .member(&Universe::scale, "scale")
        .member(&Universe::orbit_scale, "orbit_scale")
        .member(&Universe::atmosphere_exponent, "atmosphere_exponent")
        .member(&Universe::relief_exponent, "relief_exponent")
        .member(&Universe::gravity, "gravity")
        .member(&Universe::day, "day")
        .member(&Universe::habitability_bias, "habitability_bias")
        .member(&Universe::home, "home")
        .member(&Universe::surface_colors, "surface_colors")
        .member(&Universe::start_tick, "start_tick")
        .add<Scriptable>();
    world.component<CelestialFrame>()
        .member(&CelestialFrame::body, "body")
        .member(&CelestialFrame::index, "index")
        .member(&CelestialFrame::body_fixed, "body_fixed")
        .member(&CelestialFrame::radius, "radius")
        .add<Scriptable>();

    auto state = std::make_shared<UniverseWorld>();
    services(world).provide<UniverseWorld>(*state);

    world.observer<const Universe>("UniverseSet").event(OnSet).each([state](Entity e, const Universe& u) {
        World w = e.world();
        build(w, e, *state, u);
    });

    // The pose source (ADR 0085 decision 5): one tick per fixed step, every body frame written for the end of it.
    world.system<UniverseClock>("UniversePoses")
        .kind<phase::PostSimulation>()
        .with<Universe>()
        .each([state](Entity e, UniverseClock& clock) {
            if (!state->ready()) {
                return;
            }
            if (clock.tick + 1 >= tick_limit) {
                return;
            }
            clock.tick += 1;  // physics runs one step per tick: no warp while the universe has physics in it
            clock.rate = 1;
            World w = e.world();
            write_poses(w, *state, clock.tick);
        });

    if (auto* renderer = services(world).find<render::Renderer>()) {
        renderer->add_extractor("universe_sky",
                                [state](const World& w, render::RenderWorld& out) { extract_sky(w, *state, out); });
    }
}

}  // namespace veach::space
