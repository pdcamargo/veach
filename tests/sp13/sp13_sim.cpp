// Spike SP-13 (VCH-089), throwaway: see sp13_sim.hpp. Everything that feeds the simulation is built with + - * / √
// and veach::dmath, so a state hash means the same thing on every platform (ADR 0043).

#include "sp13/sp13_sim.hpp"

#include "support/jolt_runtime.hpp"  // Jolt.h first

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/space/clock.hpp>
#include <veach/space/orbit.hpp>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace veach::sp13 {
namespace {

using math::dquat;

namespace layers {
constexpr JPH::ObjectLayer static_ = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
}  // namespace layers

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;
constexpr float dt_f = 1.0f / 60.0f;

// Travel governor (ADR 0059) and the assisted-Newtonian flight model.
constexpr double tau_near = 5.0;
constexpr double v_floor = 100.0;
constexpr double assist_tau = 0.5;  ///< s: how fast the assist closes on its target velocity
constexpr double max_accel = 40.0;  ///< m/s²: thruster authority, about 4 g
constexpr double up_tau = 0.15;     ///< s: characters' and the craft's up follows −ĝ (ADR 0059)

constexpr dvec3 craft_half{2.0, 1.0, 2.0};
constexpr double craft_mass = 20'000.0;
constexpr double crate_half = 0.5;
constexpr double walk_speed = 5.0;

// Terrain: a lattice in the pad's tangent plane, projected onto the sphere; 16 m chunks of 2 m cells (ADR 0053's
// LOD0 size), each a static MeshShape body whose vertices are relative to the chunk's own origin.
constexpr int cells_per_chunk = 8;
constexpr double cell = 2.0;
constexpr double chunk_size = cell * cells_per_chunk;
constexpr int ring = 2;       ///< chunks installed within ±2 of an occupant (ADR 0058: 2 chunks)
constexpr int ring_keep = 3;  ///< and removed beyond ±3

constexpr std::int64_t reanchor_every = 60;  ///< ticks between surface re-anchor checks
constexpr double flight_reorigin_distance = 256.0;
constexpr double flight_reorigin_speed = 4.0;

JPH::RVec3 rvec(dvec3 v) {
    return {v.x, v.y, v.z};
}
JPH::Vec3 fvec(dvec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
dvec3 dv(JPH::RVec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
dvec3 dv(JPH::Vec3Arg v) {
    return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())};
}
JPH::Quat fquat(const dquat& q) {
    return JPH::Quat(static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w))
        .Normalized();
}
dquat dq(JPH::QuatArg q) {
    return {static_cast<double>(q.GetW()), static_cast<double>(q.GetX()), static_cast<double>(q.GetY()),
            static_cast<double>(q.GetZ())};
}

/// The shortest rotation taking unit vector a to unit vector b (no trigonometry).
dquat from_to(dvec3 a, dvec3 b) {
    const dvec3 c = math::cross(a, b);
    return math::normalize(dquat(1.0 + math::dot(a, b), c.x, c.y, c.z));
}

/// Moves unit vector `from` toward `to` by fraction `alpha` and renormalizes: a minimal rotation without trig.
dvec3 approach(dvec3 from, dvec3 to, double alpha) {
    return math::normalize(from + (to - from) * alpha);
}

dvec3 gravity_at(const Planet& p, dvec3 r) {
    const double d = math::length(r);
    if (d >= p.radius) {
        return r * (-p.gm / (d * d * d));
    }
    return r * (-p.gm / (p.radius * p.radius * p.radius));  // linear inside (ADR 0059)
}

struct Kin {
    dvec3 r{0.0}, v{0.0};
};

double fixed(std::int32_t value) {
    return static_cast<double>(value) / control_one;
}

std::int32_t quantize(double x, double lo, double hi) {
    const double clamped = std::clamp(x, lo, hi);
    return static_cast<std::int32_t>(std::nearbyint(clamped * control_one));
}

struct Hasher {
    std::uint64_t value = hash_id("sp13:state");
    void add(std::uint64_t bits) { value = hash(value, bits); }
    void add(double v) { add(std::bit_cast<std::uint64_t>(v)); }
    void add(float v) { add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v))); }
    void add(dvec3 v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
    void add(JPH::RVec3Arg v) {
        add(v.GetX());
        add(v.GetY());
        add(v.GetZ());
    }
    void add(JPH::Vec3Arg v) {
        add(v.GetX());
        add(v.GetY());
        add(v.GetZ());
    }
    void add(JPH::QuatArg q) {
        add(q.GetX());
        add(q.GetY());
        add(q.GetZ());
        add(q.GetW());
    }
};

/// A physics scene: a PhysicsSystem whose origin sits at `origin` in its parent frame at `origin_tick` and moves at
/// `velocity` (zero for the surface scene). Its axes are the parent frame's axes.
struct JoltScene {
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl temp{8 * 1024 * 1024};
    Frame frame = Frame::body_fixed;
    dvec3 origin{0.0};
    dvec3 velocity{0.0};
    std::int64_t origin_tick = 0;

    [[nodiscard]] dvec3 origin_at(std::int64_t tick) const {
        return origin + velocity * (static_cast<double>(tick - origin_tick) / 60.0);
    }
};

struct Character {
    JPH::Ref<JPH::CharacterVirtual> body;
    dvec3 up{0.0, 1.0, 0.0};
    dvec3 forward{0.0, 0.0, -1.0};
};

struct Crate {
    std::string_view name;
    JPH::BodyID id;
};

}  // namespace

std::string_view kind_name(Transfer::Kind kind) {
    switch (kind) {
        case Transfer::Kind::inertial_to_body_fixed: return "inertial -> body-fixed";
        case Transfer::Kind::body_fixed_to_inertial: return "body-fixed -> inertial";
        case Transfer::Kind::flight_to_surface: return "flight -> surface scene";
        case Transfer::Kind::surface_to_flight: return "surface scene -> flight";
        case Transfer::Kind::flight_reorigin: return "flight scene re-origin";
        case Transfer::Kind::surface_reanchor: return "surface scene re-anchor";
    }
    return "?";
}

Planet make_planet(double scale) {
    Planet p;
    p.scale = scale;
    p.radius = 6'371'000.0 * scale;
    p.gm = p.surface_gravity * p.radius * p.radius;
    p.atmosphere_top = 100'000.0 * std::sqrt(scale);
    p.handoff_down = p.atmosphere_top;
    p.handoff_up = p.atmosphere_top * 1.1;
    p.spin_period_q = space::period_q_from_seconds(86'164.0905);  // Earth's sidereal day, never scaled
    p.obliquity = 23.44 * deg;
    p.omega = 2.0 * pi / space::seconds_from_period_q(p.spin_period_q);
    const double lat = 30.0 * deg;
    p.site_up = dvec3(dmath::cos(lat), dmath::sin(lat), 0.0);  // longitude 0 is the body's +X
    p.site = p.site_up * p.radius;
    p.site_east = math::normalize(math::cross(dvec3(0.0, 1.0, 0.0), p.site_up));
    p.site_north = math::cross(p.site_up, p.site_east);
    return p;
}

Controls walk_controls(double walk, double turn) {
    Controls c;
    c.walk = quantize(walk, 0.0, 1.0);
    c.turn = quantize(turn, -1.0, 1.0);
    return c;
}

struct Sim::Impl {
    Config config;
    Planet planet;
    space::Spin spin;
    std::int64_t tick = 0;
    double up_alpha = 0.0;
    double last_ms = 0.0;

    // Jolt's allocator hooks must exist before the first Jolt member is built.
    bool jolt_ready = (test::ensure_jolt(), true);
    // Layer tables outlive the scenes: PhysicsSystem keeps pointers to them.
    JPH::ObjectLayerPairFilterTable pair_filter{layers::count};
    JPH::BroadPhaseLayerInterfaceTable broad_phase{layers::count, 2};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad_phase;
    std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;

    std::unique_ptr<JoltScene> surface;
    std::unique_ptr<JoltScene> flight;

    // The craft lives in one scene at a time.
    bool has_craft = false;
    Scene craft_scene = Scene::flight;
    JPH::BodyID craft;
    JPH::RefConst<JPH::Shape> craft_shape;

    bool has_walkers = false;
    JPH::BodyID base;
    dvec3 base_origin{0.0};
    std::vector<Crate> crates;
    Character walker, idler;
    JPH::RefConst<JPH::Shape> crate_shape;

    std::map<std::pair<int, int>, JPH::BodyID> chunks;
    std::vector<Transfer> transfers;

    explicit Impl(const Config& c) : config(c), planet(make_planet(c.scale)) {
        spin = {.period_q = planet.spin_period_q,
                .epoch_tick = 0,
                .phase0 = 0.0,
                .obliquity = planet.obliquity,
                .pole_longitude = 0.0};
        up_alpha = 1.0 - dmath::exp(-(1.0 / 60.0) / up_tau);

        pair_filter.EnableCollision(layers::static_, layers::moving);
        pair_filter.EnableCollision(layers::moving, layers::moving);
        broad_phase.MapObjectToBroadPhaseLayer(layers::static_, JPH::BroadPhaseLayer(0));
        broad_phase.MapObjectToBroadPhaseLayer(layers::moving, JPH::BroadPhaseLayer(1));
        object_vs_broad_phase =
            std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broad_phase, 2, pair_filter, layers::count);
        jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);

        surface = make_scene(4096, Frame::body_fixed);
        surface->origin = planet.site;
        flight = make_scene(64, Frame::inertial);

        craft_shape = new JPH::BoxShape(fvec(craft_half));
        crate_shape = new JPH::BoxShape(JPH::Vec3::sReplicate(static_cast<float>(crate_half)));
        if (config.walkers) {
            build_walkers();
        }
        if (config.craft) {
            build_craft();
        }
        update_terrain();
        surface->system.OptimizeBroadPhase();
    }

    ~Impl() {
        walker.body = nullptr;
        idler.body = nullptr;
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    std::unique_ptr<JoltScene> make_scene(JPH::uint bodies, Frame frame) {
        auto scene = std::make_unique<JoltScene>();
        scene->system.Init(bodies, 0, bodies * 2, bodies * 2, broad_phase, *object_vs_broad_phase, pair_filter);
        scene->system.SetGravity(JPH::Vec3::sZero());  // ADR 0058: gravity is a per-body force
        scene->frame = frame;
        return scene;
    }

    [[nodiscard]] space::Orientation orientation() const { return space::orientation(spin, tick); }
    [[nodiscard]] dvec3 omega_b() const { return {0.0, planet.omega, 0.0}; }

    [[nodiscard]] Kin to_inertial(Frame f, Kin k, const space::Orientation& o) const {
        if (f == Frame::inertial) {
            return k;
        }
        return {o.to_system(k.r), o.to_system(k.v + math::cross(omega_b(), k.r))};
    }
    [[nodiscard]] Kin to_body(Frame f, Kin k, const space::Orientation& o) const {
        if (f == Frame::body_fixed) {
            return k;
        }
        const dvec3 r = o.to_body(k.r);
        return {r, o.to_body(k.v) - math::cross(omega_b(), r)};
    }
    [[nodiscard]] static dquat orientation_quat(const space::Orientation& o) {
        return math::quat_cast(math::dmat3(o.x, o.y, o.z));  // columns: the body's axes in the system frame
    }

    /// A point on the sphere from tangent-plane offsets (east, north) at the pad.
    [[nodiscard]] dvec3 surface_point(double east, double north) const {
        return math::normalize(planet.site + planet.site_east * east + planet.site_north * north) * planet.radius;
    }
    [[nodiscard]] dvec3 lattice_vertex(int i, int j) const {
        return surface_point(cell * static_cast<double>(i), cell * static_cast<double>(j));
    }
    /// The chunk under a body-fixed position: its radial projection onto the pad's tangent plane.
    [[nodiscard]] std::pair<int, int> chunk_of(dvec3 r) const {
        const dvec3 n = math::normalize(r);
        const double lambda = planet.radius / math::dot(n, planet.site_up);
        const double e = lambda * math::dot(n, planet.site_east);
        const double s = lambda * math::dot(n, planet.site_north);
        return {static_cast<int>(std::floor(e / chunk_size)), static_cast<int>(std::floor(s / chunk_size))};
    }

    // --- Bodies ------------------------------------------------------------------------------------------------

    [[nodiscard]] JPH::BodyInterface& bodies(JoltScene& s) const { return s.system.GetBodyInterface(); }

    JPH::BodyID add_dynamic(JoltScene& scene, const JPH::RefConst<JPH::Shape>& shape, dvec3 local, dquat rotation,
                            dvec3 velocity, double mass, bool sleeps, bool lock_rotation) {
        JPH::BodyCreationSettings s(shape, rvec(local), fquat(rotation), JPH::EMotionType::Dynamic, layers::moving);
        s.mLinearVelocity = fvec(velocity);
        s.mLinearDamping = 0.0f;
        s.mAngularDamping = 0.0f;
        s.mMaxLinearVelocity = 2000.0f;
        s.mFriction = 0.8f;
        s.mRestitution = 0.0f;
        s.mAllowSleeping = sleeps;
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = static_cast<float>(mass);
        if (lock_rotation) {
            s.mAllowedDOFs =
                JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
        }
        return bodies(scene).CreateAndAddBody(s, JPH::EActivation::Activate);
    }

    /// Where a box of half height h stands on the sphere at tangent offsets (east, north), and its rotation.
    [[nodiscard]] std::pair<dvec3, dquat> standing(double east, double north, double half_height,
                                                   double clearance) const {
        const dvec3 ground = surface_point(east, north);
        const dvec3 up = math::normalize(ground);
        return {ground + up * (half_height + clearance), from_to({0.0, 1.0, 0.0}, up)};
    }

    void build_walkers() {
        has_walkers = true;
        JoltScene& s = *surface;
        // The base: a static block 12 m across, 40 m east of the pad.
        const auto [base_b, base_q] = standing(40.0, 0.0, 2.5, 0.0);
        base_origin = base_b;
        base = bodies(s).CreateAndAddBody(
            JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(6.0f, 2.5f, 6.0f)), rvec(base_b - s.origin),
                                      fquat(base_q), JPH::EMotionType::Static, layers::static_),
            JPH::EActivation::DontActivate);
        const auto [sleepy_b, sleepy_q] = standing(40.0, 12.0, crate_half, 0.01);
        crates.push_back({"crate_sleepy",
                          add_dynamic(s, crate_shape, sleepy_b - s.origin, sleepy_q, dvec3(0.0), 500.0, true, false)});
        const auto [awake_b, awake_q] = standing(44.0, 12.0, crate_half, 0.01);
        crates.push_back(
            {"crate_awake", add_dynamic(s, crate_shape, awake_b - s.origin, awake_q, dvec3(0.0), 500.0, false, false)});
        // The walker starts 200 m west of the pad facing south and turns left at 0.025 rad/s: a 200 m circle
        // around the pad. The idler stands 12 m south of the base.
        make_character(walker, -200.0, 0.0, -planet.site_north);
        make_character(idler, 40.0, -12.0, planet.site_east);
    }

    void make_character(Character& c, double east, double north, dvec3 facing) {
        JoltScene& s = *surface;
        const dvec3 ground = surface_point(east, north);
        c.up = math::normalize(ground);
        c.forward = math::normalize(facing - c.up * math::dot(facing, c.up));
        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mShape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, 0.9f, 0.0f), JPH::Quat::sIdentity(),
                                                               new JPH::CapsuleShape(0.6f, 0.3f))
                               .Create()
                               .Get();
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.3f);
        settings->mMaxSlopeAngle = static_cast<float>(45.0 * deg);
        settings->mUp = fvec(c.up);
        c.body = new JPH::CharacterVirtual(settings, rvec(ground + c.up * 0.02 - s.origin),
                                           fquat(from_to({0.0, 1.0, 0.0}, c.up)), &s.system);
    }

    void build_craft() {
        has_craft = true;
        const double top = planet.atmosphere_top;
        const double h0 = config.craft_altitude > 0.0 ? config.craft_altitude : 1.15 * top;
        const double v0 = config.craft_descent >= 0.0 ? config.craft_descent : 0.8 * std::sqrt(2.0 * 10.0 * h0);
        const Kin kb{planet.site + planet.site_up * (h0 + craft_half.y),
                     planet.site_east * config.craft_east - planet.site_up * v0};
        const space::Orientation o = orientation();
        const dquat q_b = from_to({0.0, 1.0, 0.0}, planet.site_up);
        const bool inertial = config.always_inertial || h0 > planet.handoff_down;
        flight->frame = inertial ? Frame::inertial : Frame::body_fixed;
        const Kin k = inertial ? to_inertial(Frame::body_fixed, kb, o) : kb;
        flight->origin = k.r;
        flight->velocity = k.v;
        flight->origin_tick = tick;
        const dquat q = inertial ? orientation_quat(o) * q_b : q_b;
        craft = add_dynamic(*flight, craft_shape, dvec3(0.0), q, dvec3(0.0), craft_mass, false, true);
        craft_scene = Scene::flight;
    }

    // --- State in frames ---------------------------------------------------------------------------------------

    [[nodiscard]] JoltScene& scene_of_craft() const { return craft_scene == Scene::surface ? *surface : *flight; }

    /// A rigid body's position and velocity in its scene's parent frame.
    [[nodiscard]] Kin body_kin(JoltScene& s, JPH::BodyID id) const {
        JPH::BodyInterface& b = s.system.GetBodyInterface();
        return {s.origin_at(tick) + dv(b.GetPosition(id)), s.velocity + dv(b.GetLinearVelocity(id))};
    }
    [[nodiscard]] Kin character_kin(const Character& c) const {
        return {surface->origin_at(tick) + dv(c.body->GetPosition()), dv(c.body->GetLinearVelocity())};
    }
    [[nodiscard]] Kin craft_kin_body(const space::Orientation& o) const {
        JoltScene& s = scene_of_craft();
        return to_body(s.frame, body_kin(s, craft), o);
    }
    [[nodiscard]] Kin craft_kin_inertial(const space::Orientation& o) const {
        JoltScene& s = scene_of_craft();
        return to_inertial(s.frame, body_kin(s, craft), o);
    }
    [[nodiscard]] double craft_altitude(const Kin& kb) const {
        return math::length(kb.r) - planet.radius - craft_half.y;
    }

    // --- Transfers ---------------------------------------------------------------------------------------------

    /// `before` and `after` are the same entity's state read back from its old and new scene, both expressed in the
    /// old scene's frame: the round trip measures the conversion and the float32 velocity Jolt stores.
    void log_transfer(Transfer::Kind kind, std::string_view who, const Kin& before, const Kin& after, double altitude) {
        transfers.push_back({.tick = tick,
                             .kind = kind,
                             .who = who,
                             .dv = math::length(after.v - before.v),
                             .dx = math::length(after.r - before.r),
                             .speed = math::length(before.v),
                             .altitude = altitude});
    }

    /// Moves the craft into scene `to` (which may be the scene it is in), whose frame becomes `frame`. A
    /// free-flight scene is re-centered on the craft: origin and velocity become the craft's, so its local
    /// position and velocity are zero. Pose and velocity convert in double through the inertial frame.
    void move_craft(Scene to, Frame frame, Transfer::Kind kind) {
        const space::Orientation o = orientation();
        JoltScene& from = scene_of_craft();
        const Frame from_frame = from.frame;
        JPH::BodyInterface& fb = bodies(from);
        const Kin parent = body_kin(from, craft);
        const dquat rot_from = dq(fb.GetRotation(craft));
        const dquat q_o = orientation_quat(o);
        dquat rot = rot_from;
        if (from_frame != frame) {
            rot = frame == Frame::inertial ? q_o * rot_from : math::conjugate(q_o) * rot_from;
        }
        const Kin target = from_frame == frame        ? parent
                           : frame == Frame::inertial ? to_inertial(from_frame, parent, o)
                                                      : to_body(from_frame, parent, o);
        const double altitude = craft_altitude(to_body(from_frame, parent, o));

        fb.RemoveBody(craft);
        fb.DestroyBody(craft);
        JoltScene& dest = to == Scene::surface ? *surface : *flight;
        dest.frame = frame;
        if (to == Scene::flight) {
            dest.origin = target.r;
            dest.velocity = target.v;
            dest.origin_tick = tick;
        }
        craft = add_dynamic(dest, craft_shape, target.r - dest.origin_at(tick), rot, target.v - dest.velocity,
                            craft_mass, false, true);
        craft_scene = to;
        const Kin after = body_kin(dest, craft);
        const Kin after_src = from_frame == frame             ? after
                              : from_frame == Frame::inertial ? to_inertial(frame, after, o)
                                                              : to_body(frame, after, o);
        log_transfer(kind, "craft", parent, after_src, altitude);
    }

    /// Re-centers the free-flight scene on the craft in place: no frame change, the body stays.
    void reorigin_flight() {
        const space::Orientation o = orientation();
        JPH::BodyInterface& b = bodies(*flight);
        const Kin parent = body_kin(*flight, craft);
        flight->origin = parent.r;
        flight->velocity = parent.v;
        flight->origin_tick = tick;
        b.SetPositionAndRotation(craft, JPH::RVec3::sZero(), b.GetRotation(craft), JPH::EActivation::DontActivate);
        b.SetLinearVelocity(craft, JPH::Vec3::sZero());
        log_transfer(Transfer::Kind::flight_reorigin, "craft", parent, body_kin(*flight, craft),
                     craft_altitude(to_body(flight->frame, parent, o)));
    }

    /// Moves the surface scene's anchor to `anchor` (body-fixed): every body and character shifts by the
    /// difference, in double; velocities are untouched (the axes don't change).
    void reanchor_surface(dvec3 anchor) {
        JoltScene& s = *surface;
        JPH::BodyInterface& b = bodies(s);
        struct Before {
            std::string_view who;
            Kin kin;
        };
        const auto collect = [&]() {
            std::vector<Before> out;
            if (has_walkers) {
                out.push_back({"walker", character_kin(walker)});
                out.push_back({"idler", character_kin(idler)});
                for (const Crate& c : crates) {
                    out.push_back({c.name, body_kin(s, c.id)});
                }
            }
            if (has_craft && craft_scene == Scene::surface) {
                out.push_back({"craft", body_kin(s, craft)});
            }
            return out;
        };
        const std::vector<Before> before = collect();

        const dvec3 shift = s.origin - anchor;  // new local = old local + (old origin − new origin)
        s.origin = anchor;
        JPH::BodyIDVector ids;
        s.system.GetBodies(ids);
        std::sort(ids.begin(), ids.end());
        for (const JPH::BodyID id : ids) {
            b.SetPosition(id, b.GetPosition(id) + rvec(shift), JPH::EActivation::DontActivate);
        }
        for (Character* c : {&walker, &idler}) {
            if (c->body != nullptr) {
                c->body->SetPosition(c->body->GetPosition() + rvec(shift));
                c->body->RefreshContacts(s.system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                                         s.system.GetDefaultLayerFilter(layers::moving), {}, {}, s.temp);
            }
        }
        s.system.OptimizeBroadPhase();

        const std::vector<Before> after = collect();
        for (std::size_t i = 0; i < before.size(); ++i) {
            log_transfer(Transfer::Kind::surface_reanchor, before[i].who, before[i].kin, after[i].kin,
                         math::length(before[i].kin.r) - planet.radius);
        }
    }

    // --- Terrain -----------------------------------------------------------------------------------------------

    [[nodiscard]] JPH::RefConst<JPH::Shape> chunk_shape(int ci, int cj, dvec3 chunk_origin) const {
        constexpr int n = cells_per_chunk + 1;
        constexpr auto un = static_cast<JPH::uint32>(n);
        JPH::VertexList vertices;
        vertices.reserve(static_cast<std::size_t>(n) * n);
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const dvec3 v = lattice_vertex(ci * cells_per_chunk + i, cj * cells_per_chunk + j) - chunk_origin;
                vertices.push_back(
                    JPH::Float3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)));
            }
        }
        JPH::IndexedTriangleList triangles;
        for (int j = 0; j + 1 < n; ++j) {
            for (int i = 0; i + 1 < n; ++i) {
                const auto k = static_cast<JPH::uint32>(j * n + i);
                // Normal (v1 − v0) × (v2 − v0) = east × north = up: the front face looks out of the planet.
                triangles.push_back(JPH::IndexedTriangle(k, k + 1, k + un + 1, 0));
                triangles.push_back(JPH::IndexedTriangle(k, k + un + 1, k + un, 0));
            }
        }
        JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            test::jolt_trace("sp13: chunk (%d, %d): %s", ci, cj, result.GetError().c_str());
            return nullptr;
        }
        return result.Get();
    }

    /// Installs the chunks within ±2 of every occupant (its position 0.25 s ahead) and removes the ones beyond ±3
    /// of all of them, in key order, at the tick boundary (ADR 0058's deterministic installs with K = 0).
    void update_terrain() {
        const space::Orientation o = orientation();
        std::vector<dvec3> occupants;
        if (has_walkers) {
            for (const Character* c : {&walker, &idler}) {
                const Kin k = character_kin(*c);
                occupants.push_back(k.r + k.v * 0.25);
            }
            for (const Crate& c : crates) {
                const Kin k = body_kin(*surface, c.id);
                occupants.push_back(k.r + k.v * 0.25);
            }
        }
        if (has_craft) {
            const Kin kb = craft_kin_body(o);
            if (craft_altitude(kb) < 64.0) {
                occupants.push_back(kb.r + kb.v * 0.25);
            }
        }
        std::set<std::pair<int, int>> want, keep;
        for (const dvec3& r : occupants) {
            const auto [ci, cj] = chunk_of(r);
            for (int dj = -ring_keep; dj <= ring_keep; ++dj) {
                for (int di = -ring_keep; di <= ring_keep; ++di) {
                    keep.insert({ci + di, cj + dj});
                    if (std::abs(di) <= ring && std::abs(dj) <= ring) {
                        want.insert({ci + di, cj + dj});
                    }
                }
            }
        }
        JPH::BodyInterface& b = bodies(*surface);
        for (auto it = chunks.begin(); it != chunks.end();) {
            if (!keep.contains(it->first)) {
                b.RemoveBody(it->second);
                b.DestroyBody(it->second);
                it = chunks.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& key : want) {
            if (chunks.contains(key)) {
                continue;
            }
            const dvec3 origin = lattice_vertex(key.first * cells_per_chunk, key.second * cells_per_chunk);
            const JPH::RefConst<JPH::Shape> shape = chunk_shape(key.first, key.second, origin);
            if (shape == nullptr) {
                continue;
            }
            JPH::BodyCreationSettings s(shape, rvec(origin - surface->origin), JPH::Quat::sIdentity(),
                                        JPH::EMotionType::Static, layers::static_);
            s.mFriction = 0.8f;
            chunks.emplace(key, b.CreateAndAddBody(s, JPH::EActivation::DontActivate));
        }
    }

    // --- The step ----------------------------------------------------------------------------------------------

    void boundary() {
        const space::Orientation o = orientation();
        if (has_craft && !config.always_inertial) {
            const Kin kb = craft_kin_body(o);
            const double alt = craft_altitude(kb);
            if (craft_scene == Scene::flight) {
                if (flight->frame == Frame::inertial && alt < planet.handoff_down) {
                    move_craft(Scene::flight, Frame::body_fixed, Transfer::Kind::inertial_to_body_fixed);
                } else if (flight->frame == Frame::body_fixed && alt > planet.handoff_up) {
                    move_craft(Scene::flight, Frame::inertial, Transfer::Kind::body_fixed_to_inertial);
                } else if (flight->frame == Frame::body_fixed && alt < planet.surface_scene_down) {
                    move_craft(Scene::surface, Frame::body_fixed, Transfer::Kind::flight_to_surface);
                }
            } else if (alt > planet.surface_scene_up) {
                move_craft(Scene::flight, Frame::body_fixed, Transfer::Kind::surface_to_flight);
            }
        }
        if (has_craft && craft_scene == Scene::flight) {
            JPH::BodyInterface& b = bodies(*flight);
            if (math::length(dv(b.GetPosition(craft))) > flight_reorigin_distance ||
                math::length(dv(b.GetLinearVelocity(craft))) > flight_reorigin_speed) {
                reorigin_flight();
            }
        }
        if (has_walkers && config.reanchor && tick % reanchor_every == 0) {
            const dvec3 local = dv(walker.body->GetPosition());
            if (math::length(local) > config.reanchor_distance) {
                reanchor_surface(surface->origin + local);
            }
        }
        update_terrain();
    }

    void add_gravity(JoltScene& s, JPH::BodyID id, double mass, bool fictitious) {
        JPH::BodyInterface& b = bodies(s);
        if (!b.IsActive(id)) {
            return;  // sleeping bodies get no force, so piles can sleep (SP-11)
        }
        const Kin k = body_kin(s, id);
        dvec3 a = gravity_at(planet, k.r);
        if (fictitious && s.frame == Frame::body_fixed) {
            const dvec3 w = omega_b();
            a += -2.0 * math::cross(w, k.v) - math::cross(w, math::cross(w, k.r));
        }
        b.AddForce(id, fvec(a * mass), JPH::EActivation::DontActivate);
    }

    /// The assisted-Newtonian flight model (ADR 0059): stick input is a target surface-relative velocity within
    /// the governor's cap; the assist counters gravity and closes on the target within thruster authority.
    /// Returns the commanded acceleration in body-fixed axes.
    [[nodiscard]] dvec3 flight_model(const Controls& c, const Kin& kb) const {
        if (fixed(c.engine) < 0.5) {
            return dvec3(0.0);
        }
        const dvec3 up = math::normalize(kb.r);
        const dvec3 east = math::normalize(math::cross(dvec3(0.0, 1.0, 0.0), up));
        const dvec3 north = math::cross(up, east);
        const double cap = std::max(v_floor, std::max(craft_altitude(kb), 0.0) / tau_near);
        dvec3 target = (up * fixed(c.lift) + east * fixed(c.strafe_x) + north * fixed(c.strafe_z)) * cap;
        const double speed = math::length(target);
        if (speed > cap) {
            target *= cap / speed;
        }
        dvec3 a = (target - kb.v) / assist_tau - gravity_at(planet, kb.r);
        const double magnitude = math::length(a);
        if (magnitude > max_accel) {
            a *= max_accel / magnitude;
        }
        return a;
    }

    void apply_craft(const Controls& c, const space::Orientation& o) {
        JoltScene& s = scene_of_craft();
        JPH::BodyInterface& b = bodies(s);
        const Kin kb = craft_kin_body(o);
        const dvec3 a_b = flight_model(c, kb);
        const dvec3 a = s.frame == Frame::inertial ? o.to_system(a_b) : a_b;
        b.AddForce(craft, fvec(a * craft_mass), JPH::EActivation::DontActivate);
        add_gravity(s, craft, craft_mass, config.fictitious && craft_scene == Scene::flight);
        // Attitude: the craft's up follows −ĝ by the minimal rotation (rotation is locked in the solver).
        const Kin k = body_kin(s, craft);
        const dquat q = dq(b.GetRotation(craft));
        const dvec3 current = q * dvec3(0.0, 1.0, 0.0);
        const dvec3 wanted = approach(current, math::normalize(k.r), up_alpha);
        if (math::length(math::cross(current, wanted)) > 1e-9) {
            b.SetRotation(craft, fquat(from_to(current, wanted) * q), JPH::EActivation::DontActivate);
        }
    }

    void update_character(Character& c, double walk, double turn) {
        JoltScene& s = *surface;
        const Kin k = character_kin(c);
        const dvec3 g = gravity_at(planet, k.r);
        c.up = approach(c.up, -math::normalize(g), up_alpha);
        // Heading is parallel-transported: projected onto the new tangent plane, never derived from north.
        c.forward = math::normalize(c.forward - c.up * math::dot(c.forward, c.up));
        if (turn != 0.0) {
            const double angle = turn * (1.0 / 60.0);
            c.forward =
                math::normalize(c.forward * dmath::cos(angle) + math::cross(c.up, c.forward) * dmath::sin(angle));
        }
        JPH::CharacterVirtual& body = *c.body;
        const JPH::Vec3 up = fvec(c.up);
        body.SetUp(up);
        body.SetRotation(fquat(from_to({0.0, 1.0, 0.0}, c.up)));
        const JPH::Vec3 velocity = body.GetLinearVelocity();
        const JPH::Vec3 vertical = up * velocity.Dot(up);
        JPH::Vec3 next = vertical;
        if (body.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround &&
            (velocity - body.GetGroundVelocity()).Dot(up) < 0.1f) {
            next = body.GetGroundVelocity();
        }
        next += fvec(g) * dt_f;
        next += fvec(c.forward * (walk * walk_speed));
        body.SetLinearVelocity(next);
        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mStickToFloorStepDown = -up * 0.5f;
        settings.mWalkStairsStepUp = up * 0.4f;
        body.ExtendedUpdate(dt_f, fvec(g), settings, s.system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                            s.system.GetDefaultLayerFilter(layers::moving), {}, {}, s.temp);
    }

    void step(const Controls& controls) {
        const auto start = std::chrono::steady_clock::now();
        boundary();
        const space::Orientation o = orientation();
        if (has_craft) {
            apply_craft(controls, o);
        }
        if (has_walkers) {
            for (const Crate& c : crates) {
                add_gravity(*surface, c.id, 500.0, false);
            }
            update_character(walker, fixed(controls.walk), fixed(controls.turn));
            update_character(idler, 0.0, 0.0);
        }
        surface->system.Update(dt_f, 1, &surface->temp, jobs.get());
        if (has_craft && craft_scene == Scene::flight) {
            flight->system.Update(dt_f, 1, &flight->temp, jobs.get());
        }
        ++tick;
        if (config.time_steps) {
            last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    }

    [[nodiscard]] std::uint64_t state_hash() const {
        Hasher h;
        h.add(static_cast<std::uint64_t>(tick));
        h.add(surface->origin);
        h.add(static_cast<std::uint64_t>(chunks.size()));
        if (has_craft) {
            JoltScene& s = scene_of_craft();
            JPH::BodyInterface& b = s.system.GetBodyInterface();
            h.add(static_cast<std::uint64_t>(craft_scene));
            h.add(static_cast<std::uint64_t>(s.frame));
            h.add(s.origin);
            h.add(s.velocity);
            h.add(static_cast<std::uint64_t>(s.origin_tick));
            h.add(b.GetPosition(craft));
            h.add(b.GetRotation(craft));
            h.add(b.GetLinearVelocity(craft));
        }
        if (has_walkers) {
            JPH::BodyInterface& b = surface->system.GetBodyInterface();
            for (const Crate& c : crates) {
                h.add(b.GetPosition(c.id));
                h.add(b.GetRotation(c.id));
                h.add(b.GetLinearVelocity(c.id));
                h.add(b.GetAngularVelocity(c.id));
            }
            for (const Character* c : {&walker, &idler}) {
                h.add(c->body->GetPosition());
                h.add(c->body->GetLinearVelocity());
                h.add(c->up);
                h.add(c->forward);
                h.add(static_cast<std::uint64_t>(c->body->GetGroundState()));
            }
        }
        return h.value;
    }
};

Sim::Sim(const Config& config) : impl_(std::make_unique<Impl>(config)) {}
Sim::~Sim() = default;

void Sim::step(const Controls& controls) {
    impl_->step(controls);
}
std::int64_t Sim::tick() const {
    return impl_->tick;
}
const Planet& Sim::planet() const {
    return impl_->planet;
}
const Config& Sim::config() const {
    return impl_->config;
}
std::uint64_t Sim::hash() const {
    return impl_->state_hash();
}
const std::vector<Transfer>& Sim::transfers() const {
    return impl_->transfers;
}
int Sim::terrain_chunks() const {
    return static_cast<int>(impl_->chunks.size());
}
double Sim::last_step_ms() const {
    return impl_->last_ms;
}
dvec3 Sim::surface_anchor() const {
    return impl_->surface->origin;
}

CraftView Sim::craft() const {
    CraftView view;
    view.tick = impl_->tick;
    if (!impl_->has_craft) {
        return view;
    }
    const Planet& p = impl_->planet;
    const space::Orientation o = impl_->orientation();
    const Kin kb = impl_->craft_kin_body(o);
    const dvec3 up = math::normalize(kb.r);
    const dvec3 east = math::normalize(math::cross(dvec3(0.0, 1.0, 0.0), up));
    const dvec3 north = math::cross(up, east);
    view.altitude = impl_->craft_altitude(kb);
    view.ve = math::dot(kb.v, east);
    view.vn = math::dot(kb.v, north);
    view.vu = math::dot(kb.v, up);
    const dvec3 ground = up * p.radius - p.site;
    view.offset_e = math::dot(ground, p.site_east);
    view.offset_n = math::dot(ground, p.site_north);
    view.cap = std::max(v_floor, std::max(view.altitude, 0.0) / tau_near);
    view.frame = static_cast<std::int32_t>(impl_->scene_of_craft().frame);
    view.scene = static_cast<std::int32_t>(impl_->craft_scene);
    return view;
}

dvec3 Sim::craft_position_inertial() const {
    return impl_->craft_kin_inertial(impl_->orientation()).r;
}
dvec3 Sim::craft_velocity_inertial() const {
    return impl_->craft_kin_inertial(impl_->orientation()).v;
}
dvec3 Sim::craft_position_body() const {
    return impl_->craft_kin_body(impl_->orientation()).r;
}

dvec3 Sim::position_body(std::string_view who) const {
    Impl& m = *impl_;
    if (who == "base") {
        return m.body_kin(*m.surface, m.base).r;
    }
    if (who == "walker") {
        return m.character_kin(m.walker).r;
    }
    if (who == "idler") {
        return m.character_kin(m.idler).r;
    }
    for (const Crate& c : m.crates) {
        if (c.name == who) {
            return m.body_kin(*m.surface, c.id).r;
        }
    }
    return dvec3(0.0);
}

dvec3 Sim::velocity_body(std::string_view who) const {
    Impl& m = *impl_;
    if (who == "walker") {
        return m.character_kin(m.walker).v;
    }
    if (who == "idler") {
        return m.character_kin(m.idler).v;
    }
    for (const Crate& c : m.crates) {
        if (c.name == who) {
            return m.body_kin(*m.surface, c.id).v;
        }
    }
    return dvec3(0.0);
}

bool Sim::walker_supported() const {
    return impl_->walker.body != nullptr && impl_->walker.body->IsSupported();
}

// --- Autopilot -------------------------------------------------------------------------------------------------

Autopilot::Autopilot(const Planet& planet, bool climb_after_landing, double walk, double turn)
    : planet_(planet), climb_(climb_after_landing), walk_(quantize(walk, 0.0, 1.0)), turn_(quantize(turn, -1.0, 1.0)) {}

Controls Autopilot::next(const CraftView& v) {
    constexpr double hover_height = 50.0;
    constexpr double braking = 10.0;       // m/s²: the braking curve v = √(2 a h), well inside the thrusters' 40
    constexpr double lag = braking * 0.5;  // the assist trails its target by a·τ (τ = 0.5 s)
    Controls c;
    c.walk = walk_;
    c.turn = turn_;
    c.engine = control_one;
    const double cap = v.cap;
    // Steer over the pad: a lateral target speed proportional to the offset, at most 30 m/s.
    double te = std::clamp(-v.offset_e / 10.0, -30.0, 30.0);
    double tn = std::clamp(-v.offset_n / 10.0, -30.0, 30.0);
    double tu = 0.0;
    switch (phase_) {
        case Phase::descend: {
            const double h = v.altitude - hover_height;
            double down = std::min(std::sqrt(2.0 * braking * std::max(h, 0.0)) - lag, cap);
            down = std::max(down, std::min(2.0, h));
            tu = -down;
            if (h <= 0.5 && std::abs(v.vu) < 1.0) {
                phase_ = Phase::hover;
                timer_ = 0;
            }
            break;
        }
        case Phase::hover:
            tu = std::clamp(-(v.altitude - hover_height) * 0.5, -5.0, 5.0);
            if (++timer_ > 60) {
                hover_min_ = std::min(hover_min_, v.altitude);
                hover_max_ = std::max(hover_max_, v.altitude);
            }
            if (timer_ >= 300) {
                phase_ = Phase::land;
            }
            break;
        case Phase::land:
            tu = -std::clamp(v.altitude * 0.25, 0.5, 5.0);
            if (v.altitude < 0.05) {
                touchdown_speed_ = std::sqrt(v.ve * v.ve + v.vn * v.vn + v.vu * v.vu);
                landing_offset_ = std::sqrt(v.offset_e * v.offset_e + v.offset_n * v.offset_n);
                phase_ = Phase::landed;
                timer_ = 0;
            }
            break;
        case Phase::landed:
            c.engine = 0;
            if (++timer_ >= 120) {
                phase_ = climb_ ? Phase::climb : Phase::done;
            }
            break;
        case Phase::climb:
            te = 0.0;
            tn = 0.0;
            tu = cap;
            if (v.altitude > planet_.handoff_up * 1.05) {
                phase_ = Phase::done;
            }
            break;
        case Phase::done:
            if (!climb_) {
                c.engine = 0;
            }
            te = 0.0;
            tn = 0.0;
            break;
    }
    c.lift = quantize(tu / cap, -1.0, 1.0);
    c.strafe_x = quantize(te / cap, -1.0, 1.0);
    c.strafe_z = quantize(tn / cap, -1.0, 1.0);
    return c;
}

}  // namespace veach::sp13
