#include <veach/space/orrery.hpp>

#include <veach/space/orbit.hpp>
#include <veach/space/presets.hpp>
#include <veach/space/sun.hpp>

#include <format>
#include <memory>
#include <optional>
#include <vector>

namespace veach::space {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;

/// The one realized system (a spike supports one orrery per world).
struct OrreryState {
    std::string key;  ///< system and scale it was built from
    System system;
    std::int32_t star = 0;
    std::vector<ecs::entity_t> bodies;
    std::vector<Pose> poses;
    std::int64_t tick = 0;
    bool built = false;
    bool clock_error = false;
};

}  // namespace

OrreryModule::OrreryModule(World& world) {
    world.module<OrreryModule>();

    world.component<UniverseClock>()
        .member(&UniverseClock::tick, "tick")
        .member(&UniverseClock::rate, "rate")
        .add<Scriptable>();
    world.component<Orrery>().member(&Orrery::system, "system").member(&Orrery::scale, "scale").add<Scriptable>();
    world.component<CelestialBody>()
        .member(&CelestialBody::name, "name")
        .member(&CelestialBody::parent, "parent")
        .member(&CelestialBody::x, "x")
        .member(&CelestialBody::y, "y")
        .member(&CelestialBody::z, "z")
        .member(&CelestialBody::vx, "vx")
        .member(&CelestialBody::vy, "vy")
        .member(&CelestialBody::vz, "vz")
        .member(&CelestialBody::radius, "radius")
        .member(&CelestialBody::distance_to_parent, "distance_to_parent")
        .member(&CelestialBody::tick, "tick")
        .add<Scriptable>();
    world.component<SunObserver>()
        .member(&SunObserver::body, "body")
        .member(&SunObserver::latitude, "latitude")
        .member(&SunObserver::longitude, "longitude")
        .member(&SunObserver::elevation, "elevation")
        .member(&SunObserver::azimuth, "azimuth")
        .member(&SunObserver::local_solar_time, "local_solar_time")
        .member(&SunObserver::east, "east")
        .member(&SunObserver::north, "north")
        .member(&SunObserver::up, "up")
        .member(&SunObserver::tick, "tick")
        .add<Scriptable>();

    auto state = std::make_shared<OrreryState>();

    // immediate(): it creates named child entities.
    world.system<const Orrery>("SpaceOrrery")
        .kind<phase::Simulation>()
        .immediate()
        .each([state](Entity e, const Orrery& orrery) {
            const std::string key = std::format("{}@{}", orrery.system, orrery.scale);
            if (!state->built || state->key != key) {
                for (const ecs::entity_t body : state->bodies) {
                    Entity(e.world(), body).destruct();
                }
                state->bodies.clear();
                state->built = false;
                state->key = key;
                std::optional<System> system = preset(orrery.system, orrery.scale);
                if (!system) {
                    log::error("space", "orrery: unknown system '{}' (known: sol, earthlike, reference)",
                               orrery.system);
                    return;
                }
                state->system = std::move(*system);
                state->poses.assign(state->system.nodes().size(), Pose{});
                state->star = primary_star(state->system);
                for (const OrbitNode& n : state->system.nodes()) {
                    state->bodies.push_back(e.world().entity().child_of(e).set_name(n.name.c_str()).id());
                }
                state->built = true;
                log::info("space", "orrery: '{}' at scale {}, {} bodies", orrery.system, orrery.scale,
                          state->bodies.size());
            }

            const UniverseClock* current = e.try_get<UniverseClock>();
            UniverseClock clock = current != nullptr ? *current : UniverseClock{};
            const std::int64_t next = clock.tick + clock.rate;
            if (clock.rate < 0 || next < 0 || next >= tick_limit) {
                if (!state->clock_error) {
                    log::error("space", "orrery: the clock can't advance from tick {} by {} (ticks stay in [0, 2^53))",
                               clock.tick, clock.rate);
                    state->clock_error = true;
                }
            } else {
                clock.tick = next;
                state->clock_error = false;
            }
            e.set<UniverseClock>(clock);

            state->tick = clock.tick;
            state->system.evaluate(clock.tick, state->poses);
            const std::span<const OrbitNode> nodes = state->system.nodes();
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const OrbitNode& n = nodes[i];
                const Pose& p = state->poses[i];
                const Pose parent = n.parent >= 0 ? state->poses[static_cast<std::size_t>(n.parent)] : Pose{};
                const math::dvec3 rel = p.position - parent.position;
                Entity(e.world(), state->bodies[i])
                    .set<CelestialBody>({.name = n.name,
                                         .parent = n.parent >= 0 ? state->system.node(n.parent).name : std::string(),
                                         .x = p.position.x,
                                         .y = p.position.y,
                                         .z = p.position.z,
                                         .vx = p.velocity.x,
                                         .vy = p.velocity.y,
                                         .vz = p.velocity.z,
                                         .radius = n.radius,
                                         .distance_to_parent = std::sqrt(math::dot(rel, rel)),
                                         .tick = clock.tick});
            }
        });

    world.system<SunObserver>("SpaceSunObservers").kind<phase::Simulation>().each([state](SunObserver& o) {
        if (!state->built) {
            return;
        }
        const std::int32_t body = state->system.find(o.body);
        if (body < 0) {
            return;
        }
        const SunSample s =
            sun_local(state->system, body, state->star, o.latitude * deg, o.longitude * deg, state->tick);
        o.elevation = s.elevation / deg;
        o.azimuth = s.azimuth / deg;
        o.local_solar_time = s.local_solar_time;
        o.east = s.local.x;
        o.north = s.local.y;
        o.up = s.local.z;
        o.tick = state->tick;
    });
}

}  // namespace veach::space
