// The first slice (slice.hpp). Everything that feeds the simulation is + − × ÷ √, floor, nearbyint and
// veach::dmath, so the state hash is the same bits on every platform (ADR 0043); Jolt, the voxel terrain and the
// orbit tree are canonical or cross-platform deterministic on their own.

#include "slice.hpp"

#include "slice_shaders.hpp"
#include "slice_shaders_materials.hpp"

#include <veach/core/dmath.hpp>
#include <veach/core/env.hpp>
#include <veach/core/hash.hpp>
#include <veach/physics/module.hpp>
#include <veach/platform/clock.hpp>
#include <veach/render/scene_renderer.hpp>
#include <veach/rhi/rhi.hpp>
#include <veach/runtime/terrain_colliders.hpp>
#include <veach/space/flight.hpp>
#include <veach/space/mcp_tools.hpp>
#include <veach/space/sun.hpp>
#include <veach/space/universe.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <vector>

namespace slice {

namespace {

namespace math = veach::math;
namespace ph = veach::physics;
namespace vs = veach::space;
namespace vx = veach::voxel;
namespace dmath = veach::dmath;
using math::dquat;
using math::dvec3;

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;
constexpr double dt = 1.0 / 60.0;

// The craft and the character never scale (ADR 0049).
constexpr dvec3 craft_half{2.0, 1.0, 2.0};
constexpr double craft_mass = 20'000.0;
constexpr double walk_speed = 5.0;
constexpr double turn_rate = 1.0;  ///< rad/s at full stick, on foot
constexpr double up_tau = 0.15;    ///< s (ADR 0059)
constexpr double flight_recenter_distance = 256.0;
constexpr double flight_recenter_speed = 4.0;
constexpr double probe_height = 100.0;  ///< the craft asks for terrain colliders below this height
constexpr double exit_offset = 4.5;     ///< m to the craft's right, where the character gets out
constexpr double dig_ahead = 2.5;       ///< m ahead of the feet
constexpr std::int32_t dig_radius_mm = 1600;
constexpr std::uint32_t dig_count = 3;

double fixed(std::int32_t v) {
    return static_cast<double>(v) / veach::axis_one;
}

double length(const dvec3& v) {
    return std::sqrt(math::dot(v, v));
}

dvec3 unit(const dvec3& v) {
    return v / length(v);
}

dquat quat_of(const vs::Orientation& o) {
    return math::normalize(math::quat_cast(math::dmat3(o.x, o.y, o.z)));
}

struct Kin {
    dvec3 r{0.0}, v{0.0};
};

/// A box or capsule-like mesh for the MeshStore (12 triangles), centered, in the entity's space.
veach::render::MeshData box_mesh(const math::vec3& half, veach::AssetId material, float lift) {
    veach::render::MeshData d;
    d.materials = {material};
    const std::array<math::vec3, 6> normals{math::vec3(1, 0, 0),  math::vec3(-1, 0, 0), math::vec3(0, 1, 0),
                                            math::vec3(0, -1, 0), math::vec3(0, 0, 1),  math::vec3(0, 0, -1)};
    for (const math::vec3& n : normals) {
        const math::vec3 a = std::abs(n.y) > 0.5f ? math::vec3(1, 0, 0) : math::vec3(0, 1, 0);
        const math::vec3 t = math::cross(a, n);  // u axis
        const math::vec3 b = math::cross(n, t);  // v axis
        const auto base = static_cast<std::uint32_t>(d.vertices.size());
        for (const auto& [su, sv] :
             {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f}, std::pair{-1.0f, 1.0f}}) {
            const math::vec3 p = (n + t * su + b * sv) * half + math::vec3(0.0f, lift, 0.0f);
            veach::MeshVertex v;
            v.position = {p.x, p.y, p.z};
            v.normal = {n.x, n.y, n.z};
            v.tangent = {t.x, t.y, t.z, 1.0f};
            v.uv = {su * 0.5f + 0.5f, sv * 0.5f + 0.5f};
            d.vertices.push_back(v);
        }
        for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            d.indices.push_back(base + i);
        }
    }
    return d;
}

struct Hasher {
    std::uint64_t value = veach::hash_id("slice:state");
    void add(std::uint64_t bits) { value = veach::hash(value, bits); }
    void add(double v) { add(std::bit_cast<std::uint64_t>(v)); }
    void add(float v) { add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v))); }
    void add(const dvec3& v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
    void add(const math::vec3& v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
    void add(const math::quat& q) {
        add(q.x);
        add(q.y);
        add(q.z);
        add(q.w);
    }
};

/// The slice's state: one per world.
class Game {
public:
    [[nodiscard]] bool started() const { return started_; }

    /// Builds the scene once the universe is realized: the landing site, the physics scenes, the craft in orbit.
    void start(veach::World world, const Slice& options) {
        auto& u = veach::services(world).get<vs::UniverseWorld>();
        physics_ = veach::find_physics_world(world);
        if (!u.ready() || physics_ == nullptr) {
            return;
        }
        started_ = true;
        universe_ = &u;
        home_ = &u.home();
        density_ = u.density(home_->index);
        terrain_ = u.terrain(home_->index);
        if (density_ == nullptr || terrain_ == nullptr) {
            veach::log::error("slice", "home {} has no terrain: the slice needs a solid planet", home_->name);
            started_ = false;
            return;
        }
        inertial_ = u.orbit_frame(home_->index);
        fixed_ = u.fixed_frame(home_->index);
        omega_ = u.spin_rate(home_->index);
        bands_ = vs::HandoffBands::of(*home_, u.system().scale.s_a);
        top_ = bands_.body_fixed_below;
        up_alpha_ = vs::filter_alpha(dt, up_tau);
        autopilot_ = options.autopilot || veach::env_is_one("VEACH_SLICE_AUTOPILOT");
        shots_ = veach::env("VEACH_SLICE_SHOTS").value_or("");

        choose_site();
        // The surface scene: in the body-fixed frame, on the LOD0 lattice point at the site (ADR 0077 decision 6).
        const std::array<std::int64_t, 3> lattice{
            static_cast<std::int64_t>(std::floor(site_.x / vx::lod0_cell_meters)),
            static_cast<std::int64_t>(std::floor(site_.y / vx::lod0_cell_meters)),
            static_cast<std::int64_t>(std::floor(site_.z / vx::lod0_cell_meters))};
        surface_ = *physics_->create_scene({.name = "slice.surface",
                                            .kind = ph::SceneKind::surface,
                                            .frame = scene_frame(vs::FlightFrame::body_fixed),
                                            .lattice_origin = lattice,
                                            .max_bodies = 4096,
                                            .max_body_pairs = 8192,
                                            .max_contact_constraints = 8192});
        flight_ = *physics_->create_scene({.name = "slice.flight",
                                           .kind = ph::SceneKind::free_flight,
                                           .frame = scene_frame(vs::FlightFrame::inertial),
                                           .max_bodies = 64,
                                           .max_body_pairs = 128,
                                           .max_contact_constraints = 128});
        colliders_ = std::make_unique<veach::TerrainColliders>(*physics_, surface_);
        terrain_->set_collider_sink(colliders_.get());

        // The craft, in a circular orbit over the site, in the planet's inertial frame.
        const double altitude = options.orbit_altitude > 0.0 ? options.orbit_altitude : home_->radius;
        const vs::Orientation o = orientation();
        const dvec3 r_b = site_up_ * (home_->radius + altitude);
        const dvec3 r_i = o.to_system(r_b);
        const dvec3 v_i = o.to_system(site_east_) * std::sqrt(home_->gm / length(r_b));
        physics_->set_frame(flight_, scene_frame(vs::FlightFrame::inertial));
        physics_->set_origin(flight_, r_i, v_i);
        heading_ = {.up = site_up_, .forward = site_east_};
        craft_ = *physics_->add_body(flight_, {.shape = ph::Shape::box(math::vec3(craft_half)),
                                               .position = dvec3(0.0),
                                               .rotation = quat_of(o) * heading_.rotation(),
                                               .friction = 0.8f,
                                               .linear_damping = 0.0f,
                                               .angular_damping = 0.0f,
                                               .max_linear_velocity = 30000.0f,
                                               .mass = craft_mass,
                                               .allow_sleeping = false,
                                               .lock_rotation = true});
        craft_frame_ = vs::FlightFrame::inertial;
        craft_scene_ = vs::FlightScene::free_flight;
        start_tick_ = u.tick();

        // What the renderer draws: the craft, the character and the camera (TerrainViewer on a GPU only: a server or
        // a test on the null RHI streams nothing it would not draw, and colliders don't depend on it).
        craft_entity_ = world.entity("craft").child_of(inertial_);
        pilot_entity_ = world.entity("pilot");
        camera_ = world.entity("camera").child_of(inertial_).set(veach::Camera{.near_plane = 0.3f, .active = true});
        auto* device = veach::services(world).find<veach::rhi::Device>();
        if (device != nullptr && device->backend() != veach::rhi::Backend::null) {
            camera_.add<veach::TerrainViewer>();
        }
        if (auto* store = veach::services(world).find<veach::render::MeshStore>()) {
            const veach::AssetId hull = store->add_material(
                {.name = "hull", .base_color = {0.80f, 0.78f, 0.72f, 1.0f}, .metallic = 0.1f, .roughness = 0.5f});
            const veach::AssetId suit = store->add_material(
                {.name = "suit", .base_color = {0.85f, 0.45f, 0.10f, 1.0f}, .metallic = 0.0f, .roughness = 0.6f});
            if (auto m = store->add(box_mesh(math::vec3(craft_half), hull, 0.0f))) {
                craft_entity_.set(veach::RuntimeMeshInstance{*m});
            }
            if (auto m = store->add(box_mesh(math::vec3(0.3f, 0.9f, 0.3f), suit, 0.9f))) {
                pilot_mesh_ = *m;
            }
        }
        write_entities(world);
        const vs::UniverseWorld::LocalSun sun = u.local_sun(home_->index, site_, u.tick());
        veach::log::info("slice",
                         "landing site on {} at {:.3f}°{} {:.3f}°{}, ground {:.1f} m above the radius, local solar "
                         "time {}, sun {:.1f}° up; craft in orbit {:.0f} m up at {:.1f} m/s (inertial); bands: "
                         "body-fixed below {:.0f} m, inertial above {:.0f} m, surface scene {:.0f}/{:.0f} m",
                         home_->name, std::abs(sun.latitude / deg), sun.latitude >= 0.0 ? "N" : "S",
                         std::abs(sun.longitude / deg), sun.longitude >= 0.0 ? "E" : "W", length(site_) - home_->radius,
                         clock_text(sun.local_solar_time), sun.elevation / deg, altitude, length(v_i),
                         bands_.body_fixed_below, bands_.inertial_above, bands_.surface_below, bands_.surface_above);
    }

    /// One fixed step, Simulation phase: controls, handoffs at the tick boundary, colliders, forces.
    void simulate(veach::World world) {
        Controls c = autopilot_ ? autopilot_controls_ : read_controls(world);
        const bool exit_edge = c.exit && !last_exit_;
        const bool dig_edge = c.dig && !last_dig_;
        last_exit_ = c.exit;
        last_dig_ = c.dig;
        controls_ = c;

        // The ground under the craft at the start of the step: one bisection, shared by the handoffs, the colliders
        // and the flight model (a transfer keeps the position).
        craft_ground_ = ground_radius(craft_body().r);
        boundary();
        if (exit_edge && !on_foot_ && landed_) {
            get_out(world);
        }
        if (dig_edge && on_foot_ && digs_ < 16) {
            dig(world);
        }
        update_colliders(world);
        if (on_foot_) {
            walk();
        }
        fly();
        ++steps_;
    }

    /// PostSimulation, after the physics step and the universe's poses (the end of the step).
    void publish(veach::World world, veach::Entity e) {
        write_entities(world);
        SliceReadout r = readout();
        if (landed_ && landed_since_ >= 0 && steps_ - landed_since_ == 120 && landing_hash_ == 0) {
            landing_hash_ = hash();
            veach::log::info("slice", "landing hash {:016x} at step {}", landing_hash_, steps_);
        }
        r.landing_hi = static_cast<std::int64_t>(landing_hash_ >> 32);
        r.landing_lo = static_cast<std::int64_t>(landing_hash_ & 0xFFFFFFFFULL);
        const std::uint64_t h = hash();
        r.hash_hi = static_cast<std::int64_t>(h >> 32);
        r.hash_lo = static_cast<std::int64_t>(h & 0xFFFFFFFFULL);
        e.set<SliceReadout>(r);
        if (steps_ % 300 == 0 || r.mode != last_mode_) {
            log_readout(r);
            last_mode_ = r.mode;
        }
        if (autopilot_) {
            autopilot_controls_ = pilot_.next(r);
            if (pilot_.phase() != last_phase_) {
                veach::log::info("slice", "autopilot: {}", Autopilot::name(pilot_.phase()));
                last_phase_ = pilot_.phase();
                shot(world, Autopilot::name(last_phase_));
            }
            altitude_shots(world, r);
            if (pilot_.phase() == Autopilot::Phase::done && ++done_frames_ == 90) {
                veach::log::info("slice", "done: dig hash {:016x}, landing hash {:016x}", h, landing_hash_);
                world.quit();
            }
        }
    }

    /// Frame timing for the log (wall clock: statistics only, never fed back).
    void time_frame(veach::World world) {
        const double now = veach::clock::now_seconds();
        if (last_frame_ > 0.0) {
            frame_ms_ = 0.9 * frame_ms_ + 0.1 * (now - last_frame_) * 1e3;
        }
        last_frame_ = now;
        if (auto* device = veach::services(world).find<veach::rhi::Device>()) {
            gpu_ms_ = 0.9 * gpu_ms_ + 0.1 * static_cast<double>(device->last_frame_gpu_ms());
        }
    }

private:
    // ---- Frames ----------------------------------------------------------------------------------------------

    [[nodiscard]] ph::SceneFrame scene_frame(vs::FlightFrame f) const {
        return f == vs::FlightFrame::body_fixed
                   ? ph::SceneFrame{.frame = fixed_.id(), .angular_velocity = dvec3(0.0, omega_, 0.0)}
                   : ph::SceneFrame{.frame = inertial_.id()};
    }
    [[nodiscard]] veach::Entity frame_entity(vs::FlightFrame f) const {
        return f == vs::FlightFrame::body_fixed ? fixed_ : inertial_;
    }
    [[nodiscard]] vs::Orientation orientation() const {
        return universe_->orientation(home_->index, universe_->tick());
    }
    [[nodiscard]] dvec3 omega_b() const { return {0.0, omega_, 0.0}; }
    [[nodiscard]] Kin to_body(vs::FlightFrame f, const Kin& k, const vs::Orientation& o) const {
        if (f == vs::FlightFrame::body_fixed) {
            return k;
        }
        const dvec3 r = o.to_body(k.r);
        return {r, o.to_body(k.v) - math::cross(omega_b(), r)};
    }
    [[nodiscard]] Kin from_body(vs::FlightFrame f, const Kin& k, const vs::Orientation& o) const {
        if (f == vs::FlightFrame::body_fixed) {
            return k;
        }
        return {o.to_system(k.r), o.to_system(k.v + math::cross(omega_b(), k.r))};
    }
    [[nodiscard]] Kin craft_kin() const {
        const ph::BodyMotion m = physics_->motion(craft_);
        return {m.position, m.velocity};
    }
    [[nodiscard]] Kin craft_body() const { return to_body(craft_frame_, craft_kin(), orientation()); }
    [[nodiscard]] double ground_radius(const dvec3& r) const { return vs::surface_radius(*density_, unit(r)); }

    // ---- The landing site ------------------------------------------------------------------------------------

    /// The flattest of 60 candidate points at 20° to 40° north (the field's slope over ±30 m).
    void choose_site() {
        // In daylight (the sun at least 20 degrees up at the start) if any candidate is; the flattest of those.
        double best = 1e300;
        bool lit_found = false;
        for (int i = 0; i < 5; ++i) {
            const double lat = (20.0 + 5.0 * i) * deg;
            for (int j = 0; j < 12; ++j) {
                const double lon = 30.0 * j * deg;
                const vs::LocalFrame f = vs::local_frame(lat, lon);
                const bool lit = universe_->local_sun(home_->index, f.up, universe_->tick()).elevation > 20.0 * deg;
                if (lit_found && !lit) {
                    continue;
                }
                const double h0 = ground_radius(f.up);
                double slope = 0.0;
                for (const dvec3& d : {f.east, -f.east, f.north, -f.north}) {
                    const dvec3 p = f.up * h0 + d * 30.0;
                    slope = std::max(slope, std::abs(ground_radius(p) - h0) / 30.0);
                }
                if (lit && !lit_found) {
                    lit_found = true;
                    best = 1e300;
                }
                if (slope < best) {
                    best = slope;
                    site_up_ = f.up;
                    site_east_ = f.east;
                    site_ = f.up * h0;
                }
            }
        }
        site_slope_ = best;
    }

    // ---- Tick-boundary moves ---------------------------------------------------------------------------------

    void boundary() {
        if (on_foot_) {
            return;
        }
        const vs::Orientation o = orientation();
        const Kin kb = craft_body();
        const double altitude = length(kb.r) - home_->radius;
        const double height = length(kb.r) - craft_ground_ - craft_half.y;
        switch (vs::decide_handoff(bands_, craft_frame_, craft_scene_, altitude, height)) {
            case vs::Handoff::to_body_fixed:
                move_craft(vs::FlightScene::free_flight, vs::FlightFrame::body_fixed, "inertial -> body-fixed", o);
                break;
            case vs::Handoff::to_inertial:
                move_craft(vs::FlightScene::free_flight, vs::FlightFrame::inertial, "body-fixed -> inertial", o);
                break;
            case vs::Handoff::to_surface:
                move_craft(vs::FlightScene::surface, vs::FlightFrame::body_fixed, "free flight -> surface scene", o);
                break;
            case vs::Handoff::to_free_flight:
                move_craft(vs::FlightScene::free_flight, vs::FlightFrame::body_fixed, "surface scene -> free flight",
                           o);
                break;
            case vs::Handoff::none: break;
        }
        if (craft_scene_ == vs::FlightScene::free_flight) {
            const ph::BodyState s = physics_->state(craft_);
            if (length(s.position) > flight_recenter_distance ||
                length(dvec3(s.linear_velocity)) > flight_recenter_speed) {
                physics_->recenter(flight_, craft_);
                ++recenters_;
            }
        }
    }

    /// Moves the craft into `scene` with its frame becoming `frame`, converting pose and velocity in double through
    /// the body's orientation at this tick (ADR 0045's handoff, as SP-13 measured it).
    void move_craft(vs::FlightScene scene, vs::FlightFrame frame, std::string_view what, const vs::Orientation& o) {
        const ph::BodyMotion m = physics_->motion(craft_);
        const Kin before{m.position, m.velocity};
        Kin kb = to_body(craft_frame_, before, o);
        double speed = length(kb.v);
        if (scene == vs::FlightScene::surface && speed > bands_.surface_entry_speed) {
            // No continuous collision detection yet: nothing enters the surface scene faster than this (ADR 0080).
            veach::log::warn("slice", "the craft reached the surface scene at {:.1f} m/s; slowed to {:.0f} m/s", speed,
                             bands_.surface_entry_speed);
            kb.v *= bands_.surface_entry_speed / speed;
            speed = bands_.surface_entry_speed;
        }
        const Kin target = from_body(frame, kb, o);
        const dquat rotation =
            frame == vs::FlightFrame::inertial ? quat_of(o) * heading_.rotation() : heading_.rotation();
        ph::TransferOptions options;
        if (scene == vs::FlightScene::free_flight) {
            options.frame = scene_frame(frame);
            options.recenter = true;
        }
        const ph::SceneId dest = scene == vs::FlightScene::surface ? surface_ : flight_;
        if (auto moved = physics_->transfer(
                craft_, dest, {.position = target.r, .velocity = target.v, .rotation = rotation}, options);
            !moved) {
            veach::log::error("slice", moved.error());
            return;
        }
        craft_scene_ = scene;
        craft_frame_ = frame;
        const Kin after = to_body(frame, craft_kin(), o);
        veach::log::info("slice",
                         "handoff {} at {:.1f} m altitude, {:.2f} m/s surface-relative: |dv| {:.3g} m/s, |dx| "
                         "{:.3g} m",
                         what, length(kb.r) - home_->radius, speed, length(after.v - kb.v), length(after.r - kb.r));
        ++handoffs_;
    }

    // ---- The character and the dig ---------------------------------------------------------------------------

    void get_out(veach::World world) {
        const Kin kb = craft_body();
        const dvec3 up = unit(kb.r);
        const dvec3 side = kb.r + heading_.right() * exit_offset;
        const dvec3 feet = unit(side) * ground_radius(side);
        walker_heading_ = vs::transport({.up = up, .forward = heading_.forward}, unit(feet), 1.0, 0.0);
        const dvec3 origin = physics_->origin_at(surface_);
        auto shape = ph::Shape::offset(ph::Shape::capsule(0.6f, 0.3f), math::vec3(0.0f, 0.9f, 0.0f));
        auto id = shape ? physics_->add_character(surface_, {.shape = *shape,
                                                             .max_slope = static_cast<float>(50.0 * deg),
                                                             .position = feet + walker_heading_.up * 0.3 - origin,
                                                             .rotation = walker_heading_.rotation(),
                                                             .up = walker_heading_.up})
                        : veach::Result<ph::CharacterId>(std::unexpected(shape.error()));
        if (!id) {
            veach::log::error("slice", id.error());
            return;
        }
        walker_ = *id;
        on_foot_ = true;
        exit_point_ = feet;
        pilot_entity_.child_of(fixed_);
        if (pilot_mesh_.is_valid()) {
            pilot_entity_.set(veach::RuntimeMeshInstance{pilot_mesh_});
        }
        veach::log::info("slice", "the pilot gets out {:.1f} m right of the craft", exit_offset);
        static_cast<void>(world);
    }

    [[nodiscard]] Kin walker_body() const {
        const ph::CharacterState s = physics_->character(walker_);
        return {physics_->origin_at(surface_) + s.position, dvec3(s.linear_velocity)};
    }

    void dig(veach::World world) {
        const Kin k = walker_body();
        const double depth = 0.5 + 0.8 * static_cast<double>(digs_ % dig_count);
        if (digs_ == 0) {
            dig_center_ = k.r + walker_heading_.forward * dig_ahead;
            dig_ground_ = ground_radius(dig_center_);
        }
        const dvec3 n = unit(dig_center_);
        const dvec3 c = n * (dig_ground_ - depth);
        vx::BrushOp op{.kind = vx::BrushKind::subtract,
                       .center_mm = {static_cast<std::int64_t>(std::nearbyint(c.x * 1000.0)),
                                     static_cast<std::int64_t>(std::nearbyint(c.y * 1000.0)),
                                     static_cast<std::int64_t>(std::nearbyint(c.z * 1000.0))},
                       .radius_mm = dig_radius_mm,
                       .tick = static_cast<std::uint64_t>(universe_->tick())};
        auto& jobs = veach::services(world).get<veach::JobSystem>();
        auto r = terrain_->apply(op, jobs);
        if (!r) {
            veach::log::error("slice", r.error());
            return;
        }
        ++digs_;
        veach::log::info("slice",
                         "dig {}: {:.1f} m sphere {:.1f} m under the ground, {} LOD0 chunks changed, edits hash "
                         "{:016x}",
                         digs_, dig_radius_mm * 1e-3, depth, r->changed.size(), terrain_->edits().hash());
    }

    void walk() {
        const Kin k = walker_body();
        const double yaw = fixed(controls_.yaw) * turn_rate * dt;
        walker_heading_ = vs::transport(walker_heading_, unit(k.r), up_alpha_, yaw);
        const double forward = std::clamp(fixed(controls_.forward), -1.0, 1.0);
        ph::CharacterInput input{.up = walker_heading_.up, .rotation = walker_heading_.rotation()};
        if (forward != 0.0) {
            input.move = walker_heading_.forward * (forward * walk_speed);
        }
        physics_->set_input(walker_, input);
    }

    // ---- Flight ----------------------------------------------------------------------------------------------

    void fly() {
        const vs::Orientation o = orientation();
        const Kin kb = craft_body();
        const double altitude = length(kb.r) - home_->radius;
        const double height = length(kb.r) - craft_ground_ - craft_half.y;
        const double yaw = on_foot_ ? 0.0 : fixed(controls_.yaw) * params_.yaw_rate * dt;
        heading_ = vs::transport(heading_, unit(kb.r), up_alpha_, yaw);
        const vs::FlightControls fc{.lift = on_foot_ ? 0.0 : fixed(controls_.lift),
                                    .right = on_foot_ ? 0.0 : fixed(controls_.right),
                                    .forward = on_foot_ ? 0.0 : fixed(controls_.forward),
                                    .yaw = 0.0,
                                    .engine = on_foot_ ? 0.0 : fixed(controls_.engine)};
        const vs::FlightState state{.position = kb.r,
                                    .velocity = kb.v,
                                    .heading = heading_,
                                    .altitude = altitude,
                                    .height = height,
                                    .gravity = vs::gravity(*home_, kb.r)};
        command_ = vs::fly(governor_, params_, fc, state, top_);
        if (command_.engine && command_.cruise && craft_scene_ == vs::FlightScene::free_flight) {
            // The cruise drive is kinematic: the scene moves with the commanded velocity, the craft rests in it.
            const Kin target = from_body(craft_frame_, {kb.r, command_.velocity}, o);
            physics_->set_origin(flight_, craft_kin().r, target.v);
            physics_->set_linear_velocity(craft_, dvec3(0.0));
        } else if (command_.engine) {
            const dvec3 a =
                craft_frame_ == vs::FlightFrame::inertial ? o.to_system(command_.acceleration) : command_.acceleration;
            physics_->add_force(craft_, a * craft_mass);
        }
        physics_->set_rotation(
            craft_, craft_frame_ == vs::FlightFrame::inertial ? quat_of(o) * heading_.rotation() : heading_.rotation());
        // Landed: in the surface scene, engine off, at rest.
        const bool was = landed_;
        landed_ = craft_scene_ == vs::FlightScene::surface && !command_.engine && length(kb.v) < 0.3 && height < 1.0;
        if (landed_ && !was && landed_since_ < 0) {
            landed_since_ = steps_;
        }
    }

    void update_colliders(veach::World world) {
        std::vector<vx::ColliderProbe> probes;
        const auto probe = [&](const Kin& k) {
            probes.push_back({.position_m = {k.r.x, k.r.y, k.r.z}, .velocity_m_s = {k.v.x, k.v.y, k.v.z}});
        };
        const Kin kb = craft_body();
        if (craft_frame_ == vs::FlightFrame::body_fixed && length(kb.r) - craft_ground_ - craft_half.y < probe_height) {
            probe(kb);
        }
        if (on_foot_) {
            probe(walker_body());
        }
        auto& jobs = veach::services(world).get<veach::JobSystem>();
        const vx::ColliderStats s =
            terrain_->update_colliders(probes, static_cast<std::uint64_t>(physics_->tick()), jobs);
        collider_installs_ += s.installed;
    }

    // ---- Input -----------------------------------------------------------------------------------------------

    [[nodiscard]] static Controls read_controls(const veach::World& world) {
        const auto axis = [&](std::size_t i) {
            return veach::quantize_axis(static_cast<double>(veach::axis_value(world, axis_actions.at(i).name)));
        };
        return {.lift = axis(0),
                .right = axis(1),
                .forward = axis(2),
                .yaw = axis(3),
                .engine = axis(4),
                .exit = veach::action_down(world, exit_action.name),
                .dig = veach::action_down(world, dig_action.name)};
    }

    // ---- Output ----------------------------------------------------------------------------------------------

    void write_entities(veach::World world) {
        const vs::Orientation o = universe_->orientation(home_->index, universe_->tick());
        const veach::Entity frame = frame_entity(craft_frame_);
        if (craft_entity_.parent() != frame) {
            craft_entity_.child_of(frame);
        }
        const ph::BodyMotion m = physics_->motion(craft_);
        craft_entity_.set(veach::Transform{.translation = m.position, .rotation = math::quat(m.rotation)});
        Kin subject = craft_body();
        vs::Heading h = heading_;
        if (on_foot_) {
            const Kin k = walker_body();
            pilot_entity_.set(veach::Transform{.translation = k.r, .rotation = math::quat(walker_heading_.rotation())});
            subject = k;
            h = walker_heading_;
        }
        // The camera: a wide view of the planet while coasting in orbit, a chase view in flight, over the shoulder
        // on foot (looking down into the hole once there is one).
        dvec3 eye{0.0}, look{0.0}, up_hint{0.0};
        veach::Entity camera_frame = fixed_;
        if (!on_foot_ && !command_.engine && craft_scene_ == vs::FlightScene::free_flight &&
            craft_frame_ == vs::FlightFrame::inertial) {
            camera_frame = inertial_;
            const dvec3 star = sun_direction_inertial();
            const dvec3 y{0.0, 1.0, 0.0};
            dvec3 side = math::cross(y, star);
            side = length(side) > 1e-6 ? unit(side) : dvec3(1.0, 0.0, 0.0);
            const dvec3 d = unit(side * dmath::cos(35.0 * deg) + star * dmath::sin(35.0 * deg) + y * 0.35);
            eye = d * (home_->radius * 3.4);
            look = dvec3(0.0);
            up_hint = y;
        } else {
            const dvec3 up = unit(subject.r);
            eye = subject.r - h.forward * (on_foot_ ? 5.0 : 28.0) + up * (on_foot_ ? 3.0 : 8.0);
            if (on_foot_ && digs_ > 0) {
                // Over the hole, from where the pilot dug it: anchored at the undug ground, so the camera never
                // follows the pilot underground.
                const dvec3 n = unit(dig_center_);
                eye = n * (dig_ground_ + 5.0) - h.forward * 6.0;
                look = n * (dig_ground_ - 2.0);
            } else if (on_foot_) {
                look = subject.r + h.forward * 6.0 + up * 1.2;
            } else {
                const double altitude = std::max(length(subject.r) - home_->radius, 1.0);
                const double r = home_->radius;
                const double dip = dmath::acos(std::clamp(r / (r + altitude), 0.0, 1.0));
                const double pitch = dip + 0.3 * (0.5 * pi - dip);
                look = subject.r + (h.forward * dmath::cos(pitch) - up * dmath::sin(pitch)) * 100.0;
            }
            up_hint = up;
            if (craft_frame_ == vs::FlightFrame::inertial && !on_foot_) {
                camera_frame = inertial_;
                eye = o.to_system(eye);
                look = o.to_system(look);
                up_hint = o.to_system(up_hint);
            }
        }
        if (camera_.parent() != camera_frame) {
            camera_.child_of(camera_frame);
        }
        const dvec3 z = unit(eye - look);
        const dvec3 x = unit(math::cross(up_hint, z));
        const dvec3 y = math::cross(z, x);
        camera_.set(veach::Transform{.translation = eye,
                                     .rotation = math::quat(math::normalize(math::quat_cast(math::dmat3(x, y, z))))});
        static_cast<void>(world);
    }

    [[nodiscard]] dvec3 sun_direction_inertial() const {
        const vs::StarSystem& s = universe_->system();
        const vs::Pose star = s.orbits.evaluate(s.bodies.front().node, universe_->tick());
        const vs::Pose planet = s.orbits.evaluate(home_->node, universe_->tick());
        return unit(star.position - planet.position);
    }

    [[nodiscard]] std::string mode() const {
        if (on_foot_) {
            return "on_foot";
        }
        if (landed_) {
            return "landed";
        }
        if (!command_.engine) {
            return "coasting";
        }
        return command_.cruise ? "cruise" : "assisted";
    }

    [[nodiscard]] SliceReadout readout() const {
        SliceReadout r;
        r.tick = universe_->tick();
        r.step = steps_;
        r.mode = mode();
        r.scale = universe_->system().scale.s_p;
        r.top = top_;
        r.engine = command_.engine;
        r.landed = landed_;
        r.digs = digs_;
        r.colliders = static_cast<std::uint32_t>(physics_->scene_info(surface_).colliders);
        Kin k = on_foot_ ? walker_body() : craft_body();
        const vs::Heading& h = on_foot_ ? walker_heading_ : heading_;
        const double lift = on_foot_ ? 0.0 : craft_half.y;
        r.frame = on_foot_ || craft_frame_ == vs::FlightFrame::body_fixed ? "body_fixed" : "inertial";
        r.scene = on_foot_ || craft_scene_ == vs::FlightScene::surface ? "surface" : "free_flight";
        r.altitude = length(k.r) - home_->radius - lift;
        const double ground = ground_radius(k.r);
        r.height = length(k.r) - ground - lift;
        r.speed = length(k.v);
        r.cap = command_.cruise ? vs::cruise_limit(governor_, params_, r.altitude, r.height, top_)
                                : governor_.cap(r.height);
        const dvec3 up = unit(k.r);
        r.v_up = math::dot(k.v, up);
        r.v_right = math::dot(k.v, h.right());
        r.v_forward = math::dot(k.v, h.forward);
        const dvec3 to_site = site_ - k.r;
        const dvec3 flat = to_site - up * math::dot(to_site, up);
        r.to_right = math::dot(flat, h.right());
        r.to_forward = math::dot(flat, h.forward);
        const vs::UniverseWorld::LocalSun sun = universe_->local_sun(home_->index, k.r, universe_->tick());
        r.latitude = sun.latitude / deg;
        r.longitude = sun.longitude / deg;
        r.local_solar_time = sun.local_solar_time;
        r.sun_elevation = sun.elevation / deg;
        if (on_foot_) {
            r.walked = length(k.r - exit_point_);
            r.depth = digs_ > 0 ? dig_ground_ - length(k.r) : ground - length(k.r);
        }
        return r;
    }

    [[nodiscard]] static std::string clock_text(double hours) {
        const auto minutes = static_cast<int>(std::floor(hours * 60.0));
        return std::format("{:02}:{:02}", (minutes / 60) % 24, minutes % 60);
    }

    void log_readout(const SliceReadout& r) const {
        veach::log::info("slice",
                         "{:7.1f} s {:9} {:10} {:11} altitude {:11.1f} m, height {:10.1f} m, speed {:8.1f} m/s (cap "
                         "{:.0f}), local solar time {} at {:.2f}° {:.2f}° (sun {:.1f}° up), s = {}, {:.1f} ms/frame",
                         static_cast<double>(r.step) * dt, r.mode, r.frame, r.scene, r.altitude, r.height, r.speed,
                         r.cap, clock_text(r.local_solar_time), r.latitude, r.longitude, r.sun_elevation, r.scale,
                         frame_ms_);
    }

    /// The canonical state: the tick, the craft's scene, frame, origin and Jolt state, the character, the headings,
    /// the edits and the installed colliders.
    [[nodiscard]] std::uint64_t hash() const {
        Hasher h;
        h.add(static_cast<std::uint64_t>(universe_->tick()));
        h.add(static_cast<std::uint64_t>(physics_->tick()));
        const ph::SceneInfo s = physics_->scene_info(physics_->scene_of(craft_));
        const ph::BodyState b = physics_->state(craft_);
        h.add(static_cast<std::uint64_t>(craft_scene_) | (static_cast<std::uint64_t>(craft_frame_) << 8));
        h.add(s.origin);
        h.add(s.velocity);
        h.add(static_cast<std::uint64_t>(s.origin_tick));
        h.add(b.position);
        h.add(b.rotation);
        h.add(b.linear_velocity);
        h.add(heading_.up);
        h.add(heading_.forward);
        if (on_foot_) {
            const ph::CharacterState c = physics_->character(walker_);
            h.add(c.position);
            h.add(c.linear_velocity);
            h.add(static_cast<std::uint64_t>(c.ground));
            h.add(walker_heading_.up);
            h.add(walker_heading_.forward);
        }
        h.add(static_cast<std::uint64_t>(digs_));
        h.add(terrain_->edits().hash());
        for (const ph::ColliderInfo& c : physics_->colliders(surface_)) {
            h.add(static_cast<std::uint64_t>(c.key.level));
            h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.key.x)) |
                  (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.key.y)) << 32));
            h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.key.z)));
            h.add(c.shape_hash);
        }
        return h.value;
    }

    // ---- Demo: screenshots and frame times -------------------------------------------------------------------

    void shot(veach::World world, std::string_view name) {
        if (shots_.empty()) {
            return;
        }
        auto* renderer = veach::services(world).find<veach::render::Renderer>();
        if (renderer == nullptr) {
            return;
        }
        std::filesystem::create_directories(shots_);
        const double s = universe_->system().scale.s_p;
        renderer->request_screenshot(std::filesystem::path(shots_) /
                                     std::format("slice_s{}_{:03}_{}.png", s, shot_++, name));
        auto* device = veach::services(world).find<veach::rhi::Device>();
        std::string passes;
        if (device != nullptr) {
            const veach::rhi::FrameTimings t = device->last_frame_timings();
            for (const veach::rhi::PassTiming& p : t.passes) {
                passes += std::format(" {} {:.3f}", p.name, p.gpu_ms);
            }
            veach::log::info("slice",
                             "frame times at '{}': cpu {:.2f} ms/frame, gpu {:.3f} ms (both smoothed over about 10 "
                             "frames); last frame's passes:{}",
                             name, frame_ms_, gpu_ms_, passes);
        }
    }

    void altitude_shots(veach::World world, const SliceReadout& r) {
        struct Mark {
            double altitude;
            const char* name;
        };
        const std::array<Mark, 5> marks{{{home_->radius * 0.5, "high"},
                                         {top_ * 1.05, "atmosphere_top"},
                                         {top_ * 0.3, "in_atmosphere"},
                                         {2000.0, "2km"},
                                         {200.0, "200m"}}};
        for (std::size_t i = 0; i < marks.size(); ++i) {
            if ((altitude_marks_ & (1u << i)) == 0 && r.height < marks[i].altitude && r.step > 200) {
                altitude_marks_ |= 1u << i;
                shot(world, marks[i].name);
            }
        }
    }

    bool started_ = false;
    vs::UniverseWorld* universe_ = nullptr;
    const vs::RealizedBody* home_ = nullptr;
    std::shared_ptr<const vx::ProgramDensity> density_;
    vx::TerrainBody* terrain_ = nullptr;
    ph::PhysicsWorld* physics_ = nullptr;
    veach::Entity inertial_, fixed_;
    double omega_ = 0.0;
    vs::HandoffBands bands_;
    vs::Governor governor_;
    vs::CraftParams params_;
    double top_ = 0.0;
    double up_alpha_ = 0.0;

    dvec3 site_{0.0}, site_up_{0.0, 1.0, 0.0}, site_east_{1.0, 0.0, 0.0};
    double site_slope_ = 0.0;
    double craft_ground_ = 0.0;  ///< the surface radius under the craft at the start of this step
    ph::SceneId surface_{}, flight_{};
    std::unique_ptr<veach::TerrainColliders> colliders_;

    ph::BodyId craft_{};
    vs::FlightFrame craft_frame_ = vs::FlightFrame::inertial;
    vs::FlightScene craft_scene_ = vs::FlightScene::free_flight;
    vs::Heading heading_;
    vs::FlightCommand command_;
    Controls controls_;
    bool last_exit_ = false, last_dig_ = false;
    bool landed_ = false;
    std::int64_t landed_since_ = -1;
    std::uint64_t landing_hash_ = 0;

    bool on_foot_ = false;
    ph::CharacterId walker_{};
    vs::Heading walker_heading_;
    dvec3 exit_point_{0.0};
    std::uint32_t digs_ = 0;
    dvec3 dig_center_{0.0};
    double dig_ground_ = 0.0;

    std::int64_t steps_ = 0;
    std::int64_t start_tick_ = 0;
    std::uint32_t handoffs_ = 0, recenters_ = 0, collider_installs_ = 0;
    std::string last_mode_;

    veach::Entity craft_entity_, pilot_entity_, camera_;
    veach::AssetId pilot_mesh_;

    bool autopilot_ = false;
    Autopilot pilot_;
    Autopilot::Phase last_phase_ = Autopilot::Phase::coast;
    Controls autopilot_controls_;
    std::string shots_;
    std::uint32_t shot_ = 0, altitude_marks_ = 0, done_frames_ = 0;
    double frame_ms_ = 0.0, gpu_ms_ = 0.0, last_frame_ = 0.0;
};

}  // namespace

std::array<std::int32_t, 5> raw_axes(const Controls& c) {
    const std::array<std::int32_t, 5> values{c.lift, c.right, c.forward, c.yaw, c.engine};
    std::array<std::int32_t, 5> out{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double v = fixed(values.at(i));
        const auto d = static_cast<double>(axis_actions.at(i).dead_zone);
        const double raw = v == 0.0 ? 0.0 : (v < 0.0 ? -1.0 : 1.0) * (d + std::abs(v) * (1.0 - d));
        out.at(i) = veach::quantize_axis(raw);
    }
    return out;
}

SliceModule::SliceModule(veach::World& world) {
    world.module<SliceModule>();
    world.component<Slice>()
        .member(&Slice::orbit_altitude, "orbit_altitude")
        .member(&Slice::autopilot, "autopilot")
        .add<veach::Scriptable>();
    world.component<SliceReadout>()
        .member(&SliceReadout::tick, "tick")
        .member(&SliceReadout::step, "step")
        .member(&SliceReadout::mode, "mode")
        .member(&SliceReadout::frame, "frame")
        .member(&SliceReadout::scene, "scene")
        .member(&SliceReadout::scale, "scale")
        .member(&SliceReadout::altitude, "altitude")
        .member(&SliceReadout::height, "height")
        .member(&SliceReadout::speed, "speed")
        .member(&SliceReadout::cap, "cap")
        .member(&SliceReadout::top, "top")
        .member(&SliceReadout::v_up, "v_up")
        .member(&SliceReadout::v_right, "v_right")
        .member(&SliceReadout::v_forward, "v_forward")
        .member(&SliceReadout::to_right, "to_right")
        .member(&SliceReadout::to_forward, "to_forward")
        .member(&SliceReadout::latitude, "latitude")
        .member(&SliceReadout::longitude, "longitude")
        .member(&SliceReadout::local_solar_time, "local_solar_time")
        .member(&SliceReadout::sun_elevation, "sun_elevation")
        .member(&SliceReadout::engine, "engine")
        .member(&SliceReadout::landed, "landed")
        .member(&SliceReadout::walked, "walked")
        .member(&SliceReadout::depth, "depth")
        .member(&SliceReadout::digs, "digs")
        .member(&SliceReadout::colliders, "colliders")
        .member(&SliceReadout::hash_hi, "hash_hi")
        .member(&SliceReadout::hash_lo, "hash_lo")
        .member(&SliceReadout::landing_hi, "landing_hi")
        .member(&SliceReadout::landing_lo, "landing_lo")
        .add<veach::Scriptable>();
    for (const AxisAction& a : axis_actions) {
        veach::bind_axis(world, a.name,
                         {.source = a.axis, .negative = a.negative, .positive = a.positive, .dead_zone = a.dead_zone});
    }
    veach::bind_action(world, exit_action.name, exit_action.pad, exit_action.key);
    veach::bind_action(world, dig_action.name, dig_action.pad, dig_action.key);

    // The scene renderer (real-time and photo mode) with the standard material: terrain chunks and the craft are
    // runtime meshes.
    if (auto* renderer = veach::services(world).find<veach::render::Renderer>()) {
        renderer->add_feature(std::make_unique<veach::render::SceneRenderer>(
            veach::render::SceneShaders{slice_shaders::path_tracer(), slice_shaders::realtime_visibility(),
                                        slice_shaders::realtime_shade_bins()},
            slice_shaders::material_kinds, veach::services(world).get<veach::AssetServer>()));
    }

    auto game = std::make_shared<Game>();
    // The slice lives on an entity named "slice", made here if the script did not set Slice: immediate(), it creates
    // entities and physics scenes.
    world.system("SliceStep").kind<veach::phase::Simulation>().immediate().run([game](veach::Iter& it) {
        veach::World w = it.world();
        veach::Entity e = w.lookup("slice");
        if (!e.is_valid()) {
            e = w.entity("slice");
        }
        if (!game->started()) {
            const Slice* options = e.try_get<Slice>();
            game->start(w, options != nullptr ? *options : Slice{});
            if (!game->started()) {
                return;
            }
        }
        game->simulate(w);
    });
    world.system("SlicePublish").kind<veach::phase::PostSimulation>().immediate().run([game](veach::Iter& it) {
        if (!game->started()) {
            return;
        }
        veach::World w = it.world();
        game->publish(w, w.lookup("slice"));
    });
    world.system("SliceFrameTime").kind<veach::phase::Render>().run([game](veach::Iter& it) {
        game->time_frame(it.world());
    });
}

void import_modules(veach::Engine& engine) {
    engine.import<veach::PhysicsModule>();          // scenes per frame, the gravity hook (VCH-113)
    engine.import<veach::TerrainModule>();          // voxel terrain, streamed and drawn (VCH-114)
    engine.import<veach::space::UniverseModule>();  // the realized universe: frames, terrain, gravity, sky
    engine.import<veach::space::SpaceMcpModule>();  // orrery_state and generate_system over MCP
    engine.import<SliceModule>();                   // the craft, the pilot, the dig
}

}  // namespace slice
