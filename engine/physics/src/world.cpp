// PhysicsWorld (physics.hpp): scenes, bodies, characters, terrain colliders and the step.

#include "jolt.hpp"

#include <veach/core/assert.hpp>
#include <veach/core/jobs.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace veach::physics {

namespace {

using detail::dvec;
using detail::fquat;
using detail::fvec;
using detail::rvec;
namespace layers = detail::layers;

/// Body user data: the engine id in the low 32 bits; bit 32 marks a collider with adjacency-ring triangles.
constexpr std::uint64_t ring_flag = std::uint64_t{1} << 32;
/// A scene steps on the job system from this many Jolt bodies (terrain colliders included); below, on the calling
/// thread (SP-11: workers do not pay off under about 200 bodies).
constexpr std::size_t parallel_body_count = 256;

constexpr std::uint32_t index_of(BodyId id) {
    return static_cast<std::uint32_t>(id);
}

JPH::EMotionType jolt_motion(MotionType m) {
    switch (m) {
        case MotionType::static_body: return JPH::EMotionType::Static;
        case MotionType::kinematic: return JPH::EMotionType::Kinematic;
        case MotionType::dynamic: return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Dynamic;
}

GroundState ground_state(JPH::CharacterBase::EGroundState s) {
    switch (s) {
        case JPH::CharacterBase::EGroundState::OnGround: return GroundState::on_ground;
        case JPH::CharacterBase::EGroundState::OnSteepGround: return GroundState::on_steep_ground;
        case JPH::CharacterBase::EGroundState::NotSupported: return GroundState::not_supported;
        case JPH::CharacterBase::EGroundState::InAir: return GroundState::in_air;
    }
    return GroundState::in_air;
}

bool is_ring(const JPH::Body& body, const JPH::SubShapeID& sub_shape) {
    return (body.GetUserData() & ring_flag) != 0 && body.GetShape()->GetMaterial(sub_shape) == detail::ring_material();
}

/// Collects a scene's contact events from Jolt's threads, and rejects contacts with adjacency-ring triangles.
class Listener final : public JPH::ContactListener {
public:
    JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult& result) override {
        if (((body1.GetUserData() | body2.GetUserData()) & ring_flag) == 0) {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        if (is_ring(body1, result.mSubShapeID1) || is_ring(body2, result.mSubShapeID2)) {
            return JPH::ValidateResult::RejectContact;
        }
        return JPH::ValidateResult::AcceptContact;  // keep asking: the next manifold may be on a ring triangle
    }

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        Contact c;
        c.kind = Contact::Kind::added;
        c.a = BodyId{static_cast<std::uint32_t>(body1.GetUserData())};
        c.b = BodyId{static_cast<std::uint32_t>(body2.GetUserData())};
        c.sub_shape_a = manifold.mSubShapeID1.GetValue();
        c.sub_shape_b = manifold.mSubShapeID2.GetValue();
        c.point = dvec(manifold.GetWorldSpaceContactPointOn1(0));
        c.normal = detail::mvec(manifold.mWorldSpaceNormal);
        c.depth = manifold.mPenetrationDepth;
        const std::scoped_lock lock(mutex);
        events.push_back(c);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        Contact c;
        c.kind = Contact::Kind::removed;
        c.a = lookup(pair.GetBody1ID());
        c.b = lookup(pair.GetBody2ID());
        c.sub_shape_a = pair.GetSubShapeID1().GetValue();
        c.sub_shape_b = pair.GetSubShapeID2().GetValue();
        const std::scoped_lock lock(mutex);
        events.push_back(c);
    }

    [[nodiscard]] BodyId lookup(const JPH::BodyID& id) const {
        const auto it = ids->find(id.GetIndexAndSequenceNumber());
        return it != ids->end() ? it->second : BodyId{~0u};
    }

    std::mutex mutex;
    std::vector<Contact> events;
    const std::unordered_map<std::uint32_t, BodyId>* ids = nullptr;
};

/// Characters ignore adjacency-ring triangles too (the seams sweep's RingFilter).
class CharacterListener final : public JPH::CharacterContactListener {
public:
    bool OnContactValidate(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact) override {
        return contact.mMaterial != detail::ring_material();
    }
};

}  // namespace

// ---- Internal state ------------------------------------------------------------------------------------------

namespace {

struct ColliderRecord {
    BodyId body{};
    JPH::BodyID jolt;
    LatticePoint origin{};
    std::int64_t extent = 32;
    Shape shape;
    std::uint32_t triangles = 0;
    std::uint64_t hash = 0;
};

struct Pending {
    std::int64_t tick = 0;
    std::vector<ColliderMesh> meshes;
    std::vector<Result<CookedCollider>> cooked;
    std::unique_ptr<TaskGroup> group;
};

struct Scene {
    SceneId id{};
    SceneSettings settings;
    math::dvec3 origin{0.0};
    math::dvec3 velocity{0.0};
    std::int64_t origin_tick = 0;
    std::optional<LatticePoint> lattice_origin;
    SceneFrame frame;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    Listener listener;
    std::unordered_map<std::uint32_t, BodyId> ids;  ///< Jolt index and sequence number -> engine id
    std::vector<std::uint32_t> stale;               ///< ids of removed bodies, kept for one step's removed events
    std::size_t body_count = 0;                     ///< Jolt bodies, colliders included
    std::map<ChunkKey, ColliderRecord> colliders;
    std::vector<std::unique_ptr<Pending>> pending;
    double last_step_ms = 0.0;

    [[nodiscard]] math::dvec3 origin_at(std::int64_t tick) const {
        if (velocity == math::dvec3(0.0)) {
            return origin;
        }
        return origin + velocity * (static_cast<double>(tick - origin_tick) / 60.0);
    }
    [[nodiscard]] JPH::BodyInterface& bodies() const { return system->GetBodyInterface(); }
};

struct BodyRecord {
    bool live = false;
    bool collider = false;
    SceneId scene{};
    JPH::BodyID jolt;
    BodySettings settings;
    double mass = 0.0;  ///< for the gravity hook
};

struct CharacterRecord {
    bool live = false;
    SceneId scene{};
    JPH::Ref<JPH::CharacterVirtual> body;
    CharacterSettings settings;
    CharacterInput input;
};

}  // namespace

struct PhysicsWorld::Impl {
    JobSystem* jobs = nullptr;
    std::unique_ptr<detail::JoltJobs> jolt_jobs;
    // Layer tables outlive the scenes: each PhysicsSystem keeps pointers to them.
    JPH::ObjectLayerPairFilterTable pair_filter{layers::count};
    JPH::BroadPhaseLayerInterfaceTable broad_phase{layers::count, 2};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad_phase;
    CharacterListener character_listener;

    std::int64_t tick = 0;
    std::uint32_t next_scene = 0;
    std::map<std::uint32_t, std::unique_ptr<Scene>> scenes;
    std::vector<BodyRecord> bodies;
    std::vector<CharacterRecord> characters;
    GravityFn gravity;
    std::vector<Contact> contacts;

    Scene& scene(SceneId id) {
        const auto it = scenes.find(static_cast<std::uint32_t>(id));
        VEACH_ASSERT(it != scenes.end(), "physics: no scene {}", static_cast<std::uint32_t>(id));
        return *it->second;
    }
    const Scene& scene(SceneId id) const { return const_cast<Impl*>(this)->scene(id); }
    Scene* find_scene(SceneId id) {
        const auto it = scenes.find(static_cast<std::uint32_t>(id));
        return it == scenes.end() ? nullptr : it->second.get();
    }
    BodyRecord& body(BodyId id) {
        VEACH_ASSERT(index_of(id) < bodies.size() && bodies[index_of(id)].live, "physics: no body {}", index_of(id));
        return bodies[index_of(id)];
    }
    const BodyRecord& body(BodyId id) const { return const_cast<Impl*>(this)->body(id); }
    CharacterRecord& character(CharacterId id) {
        const auto i = static_cast<std::uint32_t>(id);
        VEACH_ASSERT(i < characters.size() && characters[i].live, "physics: no character {}", i);
        return characters[i];
    }

    // -- Jolt bodies --------------------------------------------------------------------------------------

    /// Creates the Jolt body of `id` in `s` from its settings (scene coordinates).
    Result<JPH::BodyID> create(Scene& s, BodyId id, const BodySettings& b, std::uint64_t flags = 0) {
        if (!b.shape) {
            return make_error(ErrorCode::physics_shape_invalid, "a body without a shape",
                              "Give BodySettings::shape a Shape (Shape::box, sphere, capsule, mesh...).");
        }
        const JPH::EMotionType motion = jolt_motion(b.motion);
        JPH::BodyCreationSettings c(b.shape.impl()->shape, rvec(b.position), fquat(b.rotation), motion,
                                    b.motion == MotionType::static_body ? layers::fixed : layers::moving);
        c.mLinearVelocity = fvec(b.linear_velocity);
        c.mAngularVelocity = fvec(b.angular_velocity);
        c.mFriction = b.friction;
        c.mRestitution = b.restitution;
        c.mLinearDamping = b.linear_damping;
        c.mAngularDamping = b.angular_damping;
        c.mMaxLinearVelocity = b.max_linear_velocity;
        c.mAllowSleeping = b.allow_sleeping;
        if (b.mass) {
            c.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            c.mMassPropertiesOverride.mMass = static_cast<float>(*b.mass);
        }
        if (b.lock_rotation) {
            c.mAllowedDOFs =
                JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
        }
        c.mUserData = static_cast<std::uint64_t>(index_of(id)) | flags;
        const JPH::BodyID jolt = s.bodies().CreateAndAddBody(
            c, b.motion == MotionType::static_body ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (jolt.IsInvalid()) {
            return make_error(ErrorCode::physics_capacity,
                              std::format("scene '{}' holds {} bodies, its max_bodies", s.settings.name, s.body_count),
                              "Raise SceneSettings::max_bodies for this scene.");
        }
        s.ids[jolt.GetIndexAndSequenceNumber()] = id;
        ++s.body_count;
        return jolt;
    }

    void destroy(Scene& s, JPH::BodyID jolt) {
        s.bodies().RemoveBody(jolt);
        s.bodies().DestroyBody(jolt);
        s.stale.push_back(jolt.GetIndexAndSequenceNumber());
        --s.body_count;
    }

    BodyId allocate_body() {
        bodies.emplace_back();
        return BodyId{static_cast<std::uint32_t>(bodies.size() - 1)};
    }

    // -- Re-basing a scene ---------------------------------------------------------------------------------

    /// Moves every body and character of `s` by `shift` (scene coordinates), in Jolt body id order then character
    /// id order, and rebuilds the broad phase.
    void shift_scene(Scene& s, const math::dvec3& shift) {
        JPH::BodyInterface& b = s.bodies();
        JPH::BodyIDVector ids;
        s.system->GetBodies(ids);
        std::sort(ids.begin(), ids.end());
        for (const JPH::BodyID id : ids) {
            b.SetPosition(id, b.GetPosition(id) + rvec(shift), JPH::EActivation::DontActivate);
        }
        for (CharacterRecord& c : characters) {
            if (c.live && c.scene == s.id) {
                c.body->SetPosition(c.body->GetPosition() + rvec(shift));
                c.body->RefreshContacts(s.system->GetDefaultBroadPhaseLayerFilter(layers::moving),
                                        s.system->GetDefaultLayerFilter(layers::moving), {}, {}, *s.temp);
            }
        }
        s.system->OptimizeBroadPhase();
    }

    /// A free-flight scene's new origin and velocity at this tick; its bodies keep their parent-frame state.
    void rebase(Scene& s, const math::dvec3& origin, const math::dvec3& velocity, BodyId exact = BodyId{~0u}) {
        const math::dvec3 old_origin = s.origin_at(tick);
        const math::dvec3 old_velocity = s.velocity;
        s.origin = origin;
        s.velocity = velocity;
        s.origin_tick = tick;
        JPH::BodyInterface& b = s.bodies();
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            BodyRecord& r = bodies[i];
            if (!r.live || r.scene != s.id) {
                continue;
            }
            if (BodyId{static_cast<std::uint32_t>(i)} == exact) {
                b.SetPositionAndRotation(r.jolt, JPH::RVec3::sZero(), b.GetRotation(r.jolt),
                                         JPH::EActivation::DontActivate);
                b.SetLinearVelocity(r.jolt, JPH::Vec3::sZero());
                continue;
            }
            const math::dvec3 p = old_origin + dvec(b.GetPosition(r.jolt));
            b.SetPosition(r.jolt, rvec(p - origin), JPH::EActivation::DontActivate);
            if (r.settings.motion != MotionType::static_body) {
                const math::dvec3 v = old_velocity + dvec(b.GetLinearVelocity(r.jolt));
                b.SetLinearVelocity(r.jolt, fvec(v - velocity));
            }
        }
        for (CharacterRecord& c : characters) {
            if (c.live && c.scene == s.id) {
                c.body->SetPosition(rvec(old_origin + dvec(c.body->GetPosition()) - origin));
                c.body->SetLinearVelocity(fvec(old_velocity + dvec(c.body->GetLinearVelocity()) - velocity));
            }
        }
    }

    // -- Colliders ----------------------------------------------------------------------------------------

    [[nodiscard]] JPH::RVec3 chunk_position(const Scene& s, const LatticePoint& o) const {
        const LatticePoint& base = *s.lattice_origin;
        const double unit = s.settings.lattice.unit_meters;
        return {static_cast<double>(o[0] - base[0]) * unit, static_cast<double>(o[1] - base[1]) * unit,
                static_cast<double>(o[2] - base[2]) * unit};
    }

    /// Wakes the bodies over a chunk whose collider changed (ADR 0077 decision 5): Jolt does not.
    void wake_chunk(Scene& s, const LatticePoint& o, std::int64_t extent) {
        const JPH::RVec3 p = chunk_position(s, o);
        const float edge = static_cast<float>(extent) * static_cast<float>(s.settings.lattice.unit_meters);
        const JPH::Vec3 lo(static_cast<float>(p.GetX()) - 1.0f, static_cast<float>(p.GetY()) - 1.0f,
                           static_cast<float>(p.GetZ()) - 1.0f);
        const JPH::Vec3 hi = lo + JPH::Vec3::sReplicate(edge + 2.0f);
        s.bodies().ActivateBodiesInAABox(JPH::AABox(lo, hi), {}, {});
    }

    Result<> install(Scene& s, const CookedCollider& c) {
        JPH::BodyInterface& b = s.bodies();
        const auto it = s.colliders.find(c.key);
        if (!c.shape) {
            if (it != s.colliders.end()) {
                destroy(s, it->second.jolt);
                bodies[index_of(it->second.body)].live = false;
                const LatticePoint origin = it->second.origin;
                const std::int64_t extent = it->second.extent;
                s.colliders.erase(it);
                wake_chunk(s, origin, extent);
            }
            return {};
        }
        const std::uint64_t hash = c.shape.hash();
        if (it != s.colliders.end()) {
            // In place: the body and its id stay; Jolt does not wake what rests on it, so wake it.
            b.SetShape(it->second.jolt, c.shape.impl()->shape, false, JPH::EActivation::DontActivate);
            b.SetUserData(it->second.jolt,
                          static_cast<std::uint64_t>(index_of(it->second.body)) | (has_ring(c) ? ring_flag : 0));
            bodies[index_of(it->second.body)].settings.shape = c.shape;
            it->second.shape = c.shape;
            it->second.hash = hash;
            it->second.triangles = c.triangles;
            wake_chunk(s, it->second.origin, it->second.extent);
            return {};
        }
        const BodyId id = allocate_body();
        BodySettings settings;
        settings.shape = c.shape;
        settings.motion = MotionType::static_body;
        settings.position = dvec(chunk_position(s, c.origin));
        settings.friction = s.settings.collider_friction;
        settings.restitution = s.settings.collider_restitution;
        auto jolt = create(s, id, settings, has_ring(c) ? ring_flag : 0);
        if (!jolt) {
            bodies.pop_back();
            return std::unexpected(jolt.error());
        }
        BodyRecord& r = bodies[index_of(id)];
        r.live = true;
        r.collider = true;
        r.scene = s.id;
        r.jolt = *jolt;
        r.settings = settings;
        s.colliders.emplace(c.key, ColliderRecord{id, *jolt, c.origin, c.extent, c.shape, c.triangles, hash});
        wake_chunk(s, c.origin, c.extent);
        return {};
    }

    static bool has_ring(const CookedCollider& c) { return c.shape.impl()->has_ring; }

    void install_due(Scene& s) {
        std::vector<std::pair<ChunkKey, const CookedCollider*>> due;
        std::vector<std::unique_ptr<Pending>> taken;
        for (auto it = s.pending.begin(); it != s.pending.end();) {
            if ((*it)->tick <= tick) {
                taken.push_back(std::move(*it));
                it = s.pending.erase(it);
            } else {
                ++it;
            }
        }
        if (taken.empty()) {
            return;
        }
        VEACH_ZONE("physics install colliders");
        for (const auto& p : taken) {
            if (p->group) {
                p->group->wait();  // the step waits for a late cook: it never simulates without it
            }
            for (const Result<CookedCollider>& c : p->cooked) {
                if (!c) {
                    log::error("physics", c.error());
                    continue;
                }
                due.emplace_back(c->key, &*c);
            }
        }
        std::stable_sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < due.size(); ++i) {
            if (i + 1 < due.size() && due[i + 1].first == due[i].first) {
                continue;  // a later request for the same chunk wins
            }
            if (auto installed = install(s, *due[i].second); !installed) {
                log::error("physics", installed.error());
            }
        }
    }

    // -- The step -----------------------------------------------------------------------------------------

    [[nodiscard]] BodyMotion motion_of(const BodyRecord& r) const {
        const Scene& s = scene(r.scene);
        JPH::BodyInterface& b = s.bodies();
        BodyMotion m;
        m.position = s.origin_at(tick) + dvec(b.GetPosition(r.jolt));
        m.velocity = s.velocity + dvec(b.GetLinearVelocity(r.jolt));
        m.rotation = detail::dquat(b.GetRotation(r.jolt));
        m.angular_velocity = dvec(b.GetAngularVelocity(r.jolt));
        return m;
    }

    void apply_gravity() {
        if (!gravity) {
            return;
        }
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const BodyRecord& r = bodies[i];
            if (!r.live || r.settings.motion != MotionType::dynamic || !r.settings.gravity) {
                continue;
            }
            Scene& s = scene(r.scene);
            JPH::BodyInterface& b = s.bodies();
            if (!b.IsActive(r.jolt)) {
                continue;  // sleeping bodies get no force, so they can keep sleeping (SP-11)
            }
            const math::dvec3 p = s.origin_at(tick) + dvec(b.GetPosition(r.jolt));
            const math::dvec3 v = s.velocity + dvec(b.GetLinearVelocity(r.jolt));
            math::dvec3 a = gravity({.scene = s.id, .frame = s.frame.frame, .position = p, .velocity = v});
            const math::dvec3& w = s.frame.angular_velocity;
            if (s.settings.kind == SceneKind::free_flight && s.settings.fictitious_forces && w != math::dvec3(0.0)) {
                a += -2.0 * math::cross(w, v) - math::cross(w, math::cross(w, p));  // Coriolis, centrifugal
            }
            b.AddForce(r.jolt, fvec(a * r.mass), JPH::EActivation::DontActivate);
        }
    }

    void update_characters() {
        for (CharacterRecord& c : characters) {
            if (!c.live) {
                continue;
            }
            Scene& s = scene(c.scene);
            JPH::CharacterVirtual& body = *c.body;
            math::dvec3 g(0.0);
            if (gravity) {
                g = gravity({.scene = s.id,
                             .frame = s.frame.frame,
                             .position = s.origin_at(tick) + dvec(body.GetPosition()),
                             .velocity = s.velocity + dvec(body.GetLinearVelocity())});
            }
            const JPH::Vec3 up = fvec(c.input.up);
            body.SetUp(up);
            body.SetRotation(fquat(c.input.rotation));
            const JPH::Vec3 velocity = body.GetLinearVelocity();
            JPH::Vec3 next = up * velocity.Dot(up);
            if (body.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround &&
                (velocity - body.GetGroundVelocity()).Dot(up) < 0.1f) {
                next = body.GetGroundVelocity();
            }
            next += fvec(g) * step_seconds;
            if (c.input.move) {
                next += fvec(*c.input.move);
            }
            body.SetLinearVelocity(next);
            JPH::CharacterVirtual::ExtendedUpdateSettings update;
            update.mStickToFloorStepDown = -up * c.settings.step_down;
            update.mWalkStairsStepUp = up * c.settings.step_up;
            body.ExtendedUpdate(step_seconds, fvec(g), update,
                                s.system->GetDefaultBroadPhaseLayerFilter(layers::moving),
                                s.system->GetDefaultLayerFilter(layers::moving), {}, {}, *s.temp);
        }
    }
};

// ---- PhysicsWorld --------------------------------------------------------------------------------------------

// Jolt's allocator hooks must exist before the first Jolt member of Impl is built.
PhysicsWorld::PhysicsWorld(const WorldSettings& settings) : impl_((initialize(), std::make_unique<Impl>())) {
    Impl& m = *impl_;
    m.jobs = settings.jobs;
    m.jolt_jobs = std::make_unique<detail::JoltJobs>(settings.jobs, settings.max_concurrency);
    // The tables read the pair filter and the layer mapping when they are built: fill those first.
    m.pair_filter.EnableCollision(layers::fixed, layers::moving);
    m.pair_filter.EnableCollision(layers::moving, layers::moving);
    m.broad_phase.MapObjectToBroadPhaseLayer(layers::fixed, JPH::BroadPhaseLayer(0));
    m.broad_phase.MapObjectToBroadPhaseLayer(layers::moving, JPH::BroadPhaseLayer(1));
    m.object_vs_broad_phase =
        std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(m.broad_phase, 2, m.pair_filter, layers::count);
}

PhysicsWorld::~PhysicsWorld() {
    Impl& m = *impl_;
    for (auto& [id, s] : m.scenes) {
        s->pending.clear();  // waits for cooks in flight
    }
    for (CharacterRecord& c : m.characters) {
        c.body = nullptr;
    }
    m.scenes.clear();
}

std::int64_t PhysicsWorld::tick() const {
    return impl_->tick;
}

void PhysicsWorld::step() {
    VEACH_ZONE("physics step");
    Impl& m = *impl_;
    m.contacts.clear();
    for (auto& [id, s] : m.scenes) {
        m.install_due(*s);
    }
    m.apply_gravity();
    m.update_characters();
    for (auto& [id, s] : m.scenes) {
        for (const std::uint32_t stale : s->stale) {
            s->ids.erase(stale);
        }
        s->stale.clear();
        if (s->body_count == 0) {
            s->last_step_ms = 0.0;
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        // Small scenes step on the calling thread: SP-11 measured no gain from workers below about 200 bodies, and a
        // one-craft scene spent most of its step waking them (VCH-115).
        m.jolt_jobs->begin(s->body_count >= parallel_body_count);
        const JPH::EPhysicsUpdateError error = s->system->Update(step_seconds, 1, s->temp.get(), m.jolt_jobs.get());
        m.jolt_jobs->end();
        s->last_step_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (error != JPH::EPhysicsUpdateError::None) {
            log::error("physics", Error{ErrorCode::physics_capacity,
                                        std::format("scene '{}' overflowed in tick {} (Jolt error bits {:#x})",
                                                    s->settings.name, m.tick, static_cast<unsigned>(error)),
                                        "Raise the scene's max_body_pairs or max_contact_constraints."});
        }
        for (Contact& c : s->listener.events) {
            c.scene = s->id;
            if (index_of(c.b) < index_of(c.a)) {
                std::swap(c.a, c.b);
                std::swap(c.sub_shape_a, c.sub_shape_b);
                c.normal = -c.normal;
            }
            m.contacts.push_back(c);
        }
        s->listener.events.clear();
    }
    const auto key = [](const Contact& c) {
        return std::make_tuple(static_cast<std::uint32_t>(c.scene), index_of(c.a), index_of(c.b), c.sub_shape_a,
                               c.sub_shape_b, c.kind, std::bit_cast<std::uint64_t>(c.point.x),
                               std::bit_cast<std::uint64_t>(c.point.y), std::bit_cast<std::uint64_t>(c.point.z));
    };
    std::sort(m.contacts.begin(), m.contacts.end(),
              [&](const Contact& a, const Contact& b) { return key(a) < key(b); });
    ++m.tick;
}

// -- Scenes --

Result<SceneId> PhysicsWorld::create_scene(const SceneSettings& settings) {
    Impl& m = *impl_;
    auto s = std::make_unique<Scene>();
    s->id = SceneId{m.next_scene++};
    s->settings = settings;
    s->frame = settings.frame;
    s->velocity = settings.velocity;
    s->origin_tick = m.tick;
    if (settings.lattice_origin) {
        s->lattice_origin = settings.lattice_origin;
        const LatticePoint& p = *settings.lattice_origin;
        const double unit = settings.lattice.unit_meters;
        s->origin = {static_cast<double>(p[0]) * unit, static_cast<double>(p[1]) * unit,
                     static_cast<double>(p[2]) * unit};
    } else {
        s->origin = settings.origin;
    }
    s->system = std::make_unique<JPH::PhysicsSystem>();
    s->system->Init(settings.max_bodies, 0, settings.max_body_pairs, settings.max_contact_constraints, m.broad_phase,
                    *m.object_vs_broad_phase, m.pair_filter);
    s->system->SetGravity(JPH::Vec3::sZero());  // ADR 0058: gravity is a per-body force from the hook
    s->listener.ids = &s->ids;
    s->system->SetContactListener(&s->listener);
    s->temp = std::make_unique<JPH::TempAllocatorImpl>(static_cast<JPH::uint>(settings.temp_allocator_bytes));
    const SceneId id = s->id;
    m.scenes.emplace(static_cast<std::uint32_t>(id), std::move(s));
    return id;
}

void PhysicsWorld::destroy_scene(SceneId scene) {
    Impl& m = *impl_;
    for (BodyRecord& r : m.bodies) {
        if (r.live && r.scene == scene) {
            r.live = false;
        }
    }
    for (CharacterRecord& c : m.characters) {
        if (c.live && c.scene == scene) {
            c.live = false;
            c.body = nullptr;
        }
    }
    m.scenes.erase(static_cast<std::uint32_t>(scene));
}

std::vector<SceneId> PhysicsWorld::scenes() const {
    std::vector<SceneId> out;
    for (const auto& [id, s] : impl_->scenes) {
        out.push_back(s->id);
    }
    return out;
}

SceneInfo PhysicsWorld::scene_info(SceneId scene) const {
    const Impl& m = *impl_;
    const Scene& s = m.scene(scene);
    SceneInfo info;
    info.id = s.id;
    info.name = s.settings.name;
    info.kind = s.settings.kind;
    info.frame = s.frame;
    info.origin = s.origin;
    info.velocity = s.velocity;
    info.origin_tick = s.origin_tick;
    info.lattice_origin = s.lattice_origin;
    info.bodies = s.body_count - s.colliders.size();
    info.awake = s.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    for (const CharacterRecord& c : m.characters) {
        info.characters += c.live && c.scene == scene ? 1 : 0;
    }
    info.colliders = s.colliders.size();
    for (const auto& p : s.pending) {
        info.pending_colliders += p->meshes.size();
    }
    info.last_step_ms = s.last_step_ms;
    return info;
}

math::dvec3 PhysicsWorld::origin_at(SceneId scene) const {
    return impl_->scene(scene).origin_at(impl_->tick);
}

void PhysicsWorld::set_frame(SceneId scene, const SceneFrame& frame) {
    impl_->scene(scene).frame = frame;
}

void PhysicsWorld::set_origin(SceneId scene, const math::dvec3& origin) {
    Scene& s = impl_->scene(scene);
    const math::dvec3 shift = s.origin_at(impl_->tick) - origin;  // new local = old local + (old − new origin)
    s.origin = origin;
    if (s.velocity != math::dvec3(0.0)) {
        s.origin_tick = impl_->tick;  // a still scene's origin holds at every tick: keep its tick
    }
    s.lattice_origin.reset();
    impl_->shift_scene(s, shift);
}

void PhysicsWorld::set_origin(SceneId scene, const math::dvec3& origin, const math::dvec3& velocity) {
    Scene& s = impl_->scene(scene);
    s.lattice_origin.reset();
    impl_->rebase(s, origin, velocity);
}

void PhysicsWorld::set_origin(SceneId scene, const LatticePoint& lattice_origin) {
    Scene& s = impl_->scene(scene);
    const double unit = s.settings.lattice.unit_meters;
    const math::dvec3 origin{static_cast<double>(lattice_origin[0]) * unit,
                             static_cast<double>(lattice_origin[1]) * unit,
                             static_cast<double>(lattice_origin[2]) * unit};
    set_origin(scene, origin);
    s.lattice_origin = lattice_origin;
}

void PhysicsWorld::recenter(SceneId scene, BodyId body) {
    Impl& m = *impl_;
    Scene& s = m.scene(scene);
    const BodyMotion at = m.motion_of(m.body(body));
    m.rebase(s, at.position, at.velocity, body);
}

void PhysicsWorld::optimize(SceneId scene) {
    impl_->scene(scene).system->OptimizeBroadPhase();
}

std::uint64_t PhysicsWorld::state_hash(SceneId scene) const {
    JPH::StateRecorderImpl recorder;
    impl_->scene(scene).system->SaveState(recorder);
    return detail::hash_bytes(recorder.GetData());
}

// -- Bodies --

Result<BodyId> PhysicsWorld::add_body(SceneId scene, const BodySettings& settings) {
    Impl& m = *impl_;
    Scene* s = m.find_scene(scene);
    if (s == nullptr) {
        return make_error(ErrorCode::physics_invalid_transfer,
                          std::format("no physics scene {}", static_cast<std::uint32_t>(scene)),
                          "Create the scene first (PhysicsWorld::create_scene).");
    }
    const BodyId id = m.allocate_body();
    auto jolt = m.create(*s, id, settings);
    if (!jolt) {
        m.bodies.pop_back();
        return std::unexpected(jolt.error());
    }
    BodyRecord& r = m.bodies[index_of(id)];
    r.live = true;
    r.scene = scene;
    r.jolt = *jolt;
    r.settings = settings;
    if (settings.mass) {
        r.mass = *settings.mass;
    } else {
        const JPH::BodyLockRead lock(s->system->GetBodyLockInterface(), *jolt);
        const JPH::MotionProperties* motion = lock.GetBody().GetMotionPropertiesUnchecked();
        r.mass = motion != nullptr && motion->GetInverseMassUnchecked() > 0.0f
                     ? static_cast<double>(1.0f / motion->GetInverseMassUnchecked())
                     : 0.0;
    }
    return id;
}

void PhysicsWorld::remove_body(BodyId body) {
    Impl& m = *impl_;
    BodyRecord& r = m.body(body);
    VEACH_ASSERT(!r.collider, "physics: remove terrain colliders with an empty ColliderMesh");
    m.destroy(m.scene(r.scene), r.jolt);
    r.live = false;
}

Result<> PhysicsWorld::transfer(BodyId body, SceneId to, const BodyMotion& motion, const TransferOptions& options) {
    Impl& m = *impl_;
    BodyRecord& r = m.body(body);
    Scene* dest = m.find_scene(to);
    if (dest == nullptr) {
        return make_error(ErrorCode::physics_invalid_transfer,
                          std::format("no physics scene {}", static_cast<std::uint32_t>(to)),
                          "Transfer into a scene that exists (PhysicsWorld::scenes).");
    }
    const std::size_t others = dest->body_count - (r.scene == to ? 1 : 0);
    if (options.frame && others > 0) {
        return make_error(
            ErrorCode::physics_invalid_transfer,
            std::format("scene '{}' changes frame with {} other bodies in it", dest->settings.name, others),
            "Only an empty scene changes its parent frame (a free-flight scene that follows one "
            "craft); move the others first, or transfer into a scene already in that frame.");
    }
    m.destroy(m.scene(r.scene), r.jolt);
    if (options.frame) {
        dest->frame = *options.frame;
    }
    if (options.recenter) {
        m.rebase(*dest, motion.position, motion.velocity);
    }
    BodySettings s = r.settings;
    s.position = motion.position - dest->origin_at(m.tick);
    s.linear_velocity = motion.velocity - dest->velocity;
    s.rotation = motion.rotation;
    s.angular_velocity = motion.angular_velocity;
    auto jolt = m.create(*dest, body, s);
    if (!jolt) {
        r.live = false;
        return std::unexpected(jolt.error());
    }
    r.scene = to;
    r.jolt = *jolt;
    return {};
}

bool PhysicsWorld::contains(BodyId body) const {
    return index_of(body) < impl_->bodies.size() && impl_->bodies[index_of(body)].live;
}

SceneId PhysicsWorld::scene_of(BodyId body) const {
    return impl_->body(body).scene;
}

std::vector<BodyId> PhysicsWorld::bodies(SceneId scene) const {
    std::vector<BodyId> out;
    for (std::size_t i = 0; i < impl_->bodies.size(); ++i) {
        const BodyRecord& r = impl_->bodies[i];
        if (r.live && !r.collider && r.scene == scene) {
            out.push_back(BodyId{static_cast<std::uint32_t>(i)});
        }
    }
    return out;
}

BodyState PhysicsWorld::state(BodyId body) const {
    const BodyRecord& r = impl_->body(body);
    JPH::BodyInterface& b = impl_->scene(r.scene).bodies();
    return {.position = dvec(b.GetPosition(r.jolt)),
            .rotation = detail::mquat(b.GetRotation(r.jolt)),
            .linear_velocity = detail::mvec(b.GetLinearVelocity(r.jolt)),
            .angular_velocity = detail::mvec(b.GetAngularVelocity(r.jolt)),
            .awake = b.IsActive(r.jolt)};
}

BodyMotion PhysicsWorld::motion(BodyId body) const {
    return impl_->motion_of(impl_->body(body));
}

const BodySettings& PhysicsWorld::settings(BodyId body) const {
    return impl_->body(body).settings;
}

double PhysicsWorld::mass(BodyId body) const {
    return impl_->body(body).mass;
}

double PhysicsWorld::inverse_mass(BodyId body) const {
    const BodyRecord& r = impl_->body(body);
    const JPH::BodyLockRead lock(impl_->scene(r.scene).system->GetBodyLockInterface(), r.jolt);
    const JPH::MotionProperties* motion = lock.GetBody().GetMotionPropertiesUnchecked();
    return motion != nullptr ? static_cast<double>(motion->GetInverseMassUnchecked()) : 0.0;
}

bool PhysicsWorld::awake(BodyId body) const {
    const BodyRecord& r = impl_->body(body);
    return impl_->scene(r.scene).bodies().IsActive(r.jolt);
}

void PhysicsWorld::add_force(BodyId body, const math::dvec3& force) {
    const BodyRecord& r = impl_->body(body);
    impl_->scene(r.scene).bodies().AddForce(r.jolt, fvec(force), JPH::EActivation::DontActivate);
}

void PhysicsWorld::add_impulse(BodyId body, const math::dvec3& impulse) {
    const BodyRecord& r = impl_->body(body);
    impl_->scene(r.scene).bodies().AddImpulse(r.jolt, fvec(impulse));
}

void PhysicsWorld::set_rotation(BodyId body, const math::dquat& rotation) {
    const BodyRecord& r = impl_->body(body);
    impl_->scene(r.scene).bodies().SetRotation(r.jolt, fquat(rotation), JPH::EActivation::DontActivate);
}

void PhysicsWorld::set_linear_velocity(BodyId body, const math::dvec3& velocity) {
    const BodyRecord& r = impl_->body(body);
    impl_->scene(r.scene).bodies().SetLinearVelocity(r.jolt, fvec(velocity));
}

void PhysicsWorld::wake(BodyId body) {
    const BodyRecord& r = impl_->body(body);
    impl_->scene(r.scene).bodies().ActivateBody(r.jolt);
}

// -- Characters --

Result<CharacterId> PhysicsWorld::add_character(SceneId scene, const CharacterSettings& settings) {
    Impl& m = *impl_;
    Scene* s = m.find_scene(scene);
    if (s == nullptr || !settings.shape) {
        return make_error(s == nullptr ? ErrorCode::physics_invalid_transfer : ErrorCode::physics_shape_invalid,
                          s == nullptr ? "no such physics scene" : "a character without a shape",
                          "Create the scene first and give the character a shape (a capsule above its feet).");
    }
    JPH::Ref<JPH::CharacterVirtualSettings> c = new JPH::CharacterVirtualSettings();
    c->mShape = settings.shape.impl()->shape;
    c->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), settings.supporting_plane);
    c->mMaxSlopeAngle = settings.max_slope;
    c->mEnhancedInternalEdgeRemoval = settings.enhanced_internal_edge_removal;
    c->mUp = fvec(settings.up);
    CharacterRecord r;
    r.live = true;
    r.scene = scene;
    r.settings = settings;
    r.input = {.up = settings.up, .rotation = settings.rotation, .move = std::nullopt};
    r.body = new JPH::CharacterVirtual(c, rvec(settings.position), fquat(settings.rotation), s->system.get());
    r.body->SetListener(&m.character_listener);
    m.characters.push_back(std::move(r));
    return CharacterId{static_cast<std::uint32_t>(m.characters.size() - 1)};
}

void PhysicsWorld::remove_character(CharacterId character) {
    CharacterRecord& c = impl_->character(character);
    c.live = false;
    c.body = nullptr;
}

std::vector<CharacterId> PhysicsWorld::characters(SceneId scene) const {
    std::vector<CharacterId> out;
    for (std::size_t i = 0; i < impl_->characters.size(); ++i) {
        if (impl_->characters[i].live && impl_->characters[i].scene == scene) {
            out.push_back(CharacterId{static_cast<std::uint32_t>(i)});
        }
    }
    return out;
}

void PhysicsWorld::set_input(CharacterId character, const CharacterInput& input) {
    impl_->character(character).input = input;
}

CharacterState PhysicsWorld::character(CharacterId character) const {
    const CharacterRecord& c = impl_->character(character);
    return {.position = dvec(c.body->GetPosition()),
            .linear_velocity = detail::mvec(c.body->GetLinearVelocity()),
            .ground_velocity = detail::mvec(c.body->GetGroundVelocity()),
            .ground = ground_state(c.body->GetGroundState()),
            .supported = c.body->IsSupported()};
}

SceneId PhysicsWorld::scene_of(CharacterId character) const {
    return impl_->character(character).scene;
}

void PhysicsWorld::set_gravity(GravityFn gravity) {
    impl_->gravity = std::move(gravity);
}

// -- Colliders --

Result<> PhysicsWorld::request_colliders(SceneId scene, std::vector<ColliderMesh> meshes) {
    Impl& m = *impl_;
    Scene& s = m.scene(scene);
    if (!s.lattice_origin) {
        return make_error(ErrorCode::physics_not_on_lattice,
                          std::format("scene '{}' has no lattice origin", s.settings.name),
                          "Create the surface scene with SceneSettings::lattice_origin (or re-anchor it with "
                          "set_origin(scene, LatticePoint)), so collider vertices land on lattice positions exactly.");
    }
    auto p = std::make_unique<Pending>();
    p->tick = m.tick + collider_install_delay;
    p->meshes = std::move(meshes);
    p->cooked.resize(p->meshes.size(), make_error(ErrorCode::internal, "not cooked"));
    const Lattice lattice = s.settings.lattice;
    if (m.jobs != nullptr && m.jobs->worker_count() > 0) {
        p->group = std::make_unique<TaskGroup>(*m.jobs);
        Pending* raw = p.get();
        for (std::size_t i = 0; i < raw->meshes.size(); ++i) {
            raw->group->run("physics cook collider",
                            [raw, i, lattice] { raw->cooked[i] = cook_collider(raw->meshes[i], lattice); });
        }
    } else {
        for (std::size_t i = 0; i < p->meshes.size(); ++i) {
            p->cooked[i] = cook_collider(p->meshes[i], lattice);
        }
    }
    s.pending.push_back(std::move(p));
    return {};
}

Result<> PhysicsWorld::install_colliders(SceneId scene, std::span<const CookedCollider> colliders) {
    Impl& m = *impl_;
    Scene& s = m.scene(scene);
    if (!s.lattice_origin) {
        return make_error(ErrorCode::physics_not_on_lattice,
                          std::format("scene '{}' has no lattice origin", s.settings.name),
                          "Create the surface scene with SceneSettings::lattice_origin.");
    }
    VEACH_ZONE("physics install colliders");
    for (const CookedCollider& c : colliders) {
        if (auto installed = m.install(s, c); !installed) {
            return installed;
        }
    }
    return {};
}

std::vector<ColliderInfo> PhysicsWorld::colliders(SceneId scene) const {
    const Scene& s = impl_->scene(scene);
    std::vector<ColliderInfo> out;
    out.reserve(s.colliders.size());
    for (const auto& [key, c] : s.colliders) {
        out.push_back({.key = key,
                       .slot = c.jolt.GetIndexAndSequenceNumber(),
                       .position = dvec(s.bodies().GetPosition(c.jolt)),
                       .shape_hash = c.hash,
                       .triangles = c.triangles});
    }
    return out;
}

// -- Queries --

std::optional<RayHit> PhysicsWorld::cast_ray(SceneId scene, const math::dvec3& from,
                                             const math::vec3& direction) const {
    const Scene& s = impl_->scene(scene);
    const JPH::RRayCast ray(rvec(from), fvec(direction));
    JPH::RayCastResult hit;
    if (!s.system->GetNarrowPhaseQuery().CastRay(ray, hit)) {
        return std::nullopt;
    }
    const auto it = s.ids.find(hit.mBodyID.GetIndexAndSequenceNumber());
    return RayHit{.fraction = static_cast<double>(hit.mFraction), .body = it != s.ids.end() ? it->second : BodyId{~0u}};
}

std::span<const Contact> PhysicsWorld::contacts() const {
    return impl_->contacts;
}

}  // namespace veach::physics
