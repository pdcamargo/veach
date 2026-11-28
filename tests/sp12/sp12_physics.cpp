// Spike SP-12 (VCH-088), throwaway: see sp12_physics.hpp. The only SP-12 file that includes Jolt.

#include "sp12/sp12_physics.hpp"

#include "sp11/sp11_scene.hpp"

#include <veach/core/profile.hpp>

#include <Jolt/Jolt.h>  // Must come first.

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

namespace veach::sp12 {

struct CookedShape {
    JPH::RefConst<JPH::Shape> shape;
};

namespace {

constexpr std::uint64_t fnv_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

struct Hash {
    std::uint64_t value = fnv_basis;
    void add(std::uint64_t bits) { value = mix(value ^ bits) * fnv_prime; }
    void add(double v) { add(std::bit_cast<std::uint64_t>(v)); }
    void add(float v) { add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v))); }
};

/// A StreamOut that only hashes what is written (FNV-1a over the bytes).
class HashStream final : public JPH::StreamOut {
public:
    void WriteBytes(const void* data, size_t bytes) override {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            value = (value ^ p[i]) * fnv_prime;
        }
    }
    bool IsFailed() const override { return false; }
    std::uint64_t value = fnv_basis;
};

std::uint64_t hash_bytes(const std::string& bytes) {
    std::uint64_t h = fnv_basis;
    for (const char c : bytes) {
        h = (h ^ static_cast<unsigned char>(c)) * fnv_prime;
    }
    return h;
}

namespace layers {
constexpr JPH::ObjectLayer static_ = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
}  // namespace layers

constexpr float step_seconds = 1.0f / 60.0f;
constexpr double gravity = 9.81;

}  // namespace

Collider cook_collider(const voxel::ChunkMesh& mesh, const CookOptions& options) {
    VEACH_ZONE("sp12 cook collider");
    sp11::ensure_jolt();
    Collider out;
    out.key = mesh.key;
    if (mesh.indices.empty()) {
        return out;
    }
    const auto t0 = std::chrono::steady_clock::now();
    JPH::VertexList vertices;
    vertices.reserve(mesh.position.size());
    for (const auto& p : mesh.position) {
        vertices.push_back(JPH::Float3(p[0], p[1], p[2]));
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        // The mesher winds counter-clockwise seen from outside the solid: Jolt's front face too.
        triangles.push_back(JPH::IndexedTriangle(mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2], 0));
    }
    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
    settings.mBuildQuality = options.favor_build_speed ? JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed
                                                       : JPH::MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        std::fprintf(stderr, "sp12: cooking chunk (%d, %d, %d): %s\n", mesh.key.x, mesh.key.y, mesh.key.z,
                     result.GetError().c_str());
        return out;
    }
    auto cooked = std::make_shared<CookedShape>();
    cooked->shape = result.Get();
    HashStream stream;
    cooked->shape->SaveBinaryState(stream);
    out.hash = stream.value;
    out.triangles = cooked->shape->GetStats().mNumTriangles;
    out.shape = std::move(cooked);
    out.cook_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

struct PhysicsScene::Impl {
    struct TerrainBody {
        JPH::BodyID id;
        std::uint64_t shape_hash = 0;
    };

    Impl() : temp_allocator(16 * 1024 * 1024), jobs(JPH::cMaxPhysicsJobs) {
        // The tables read the pair filter and the layer mapping when they are built: fill those first.
        pair_filter.EnableCollision(layers::static_, layers::moving);
        pair_filter.EnableCollision(layers::moving, layers::moving);
        broad_phase.MapObjectToBroadPhaseLayer(layers::static_, JPH::BroadPhaseLayer(0));
        broad_phase.MapObjectToBroadPhaseLayer(layers::moving, JPH::BroadPhaseLayer(1));
        object_vs_broad_phase =
            std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broad_phase, 2, pair_filter, layers::count);
        system.Init(1024, 0, 4096, 4096, broad_phase, *object_vs_broad_phase, pair_filter);
        system.SetGravity(JPH::Vec3::sZero());  // ADR 0058: per-body gravity forces
    }

    JPH::RVec3 chunk_position(const voxel::ChunkKey& key) const {
        const auto o = voxel::chunk_origin(key);
        return JPH::RVec3(static_cast<double>(o[0] - origin[0]) * voxel::lod0_cell_meters,
                          static_cast<double>(o[1] - origin[1]) * voxel::lod0_cell_meters,
                          static_cast<double>(o[2] - origin[2]) * voxel::lod0_cell_meters);
    }

    /// Wakes the bodies over a chunk whose collider changed: a sleeping ball doesn't see a new shape under it.
    void wake(const voxel::ChunkKey& key) {
        const JPH::RVec3 p = chunk_position(key);
        const float edge = static_cast<float>(voxel::chunk_units(0)) * static_cast<float>(voxel::lod0_cell_meters);
        const JPH::Vec3 lo(static_cast<float>(p.GetX()) - 1.0f, static_cast<float>(p.GetY()) - 1.0f,
                           static_cast<float>(p.GetZ()) - 1.0f);
        const JPH::Vec3 hi = lo + JPH::Vec3::sReplicate(edge + 2.0f);
        system.GetBodyInterface().ActivateBodiesInAABox(JPH::AABox(lo, hi), {}, {});
    }

    std::array<std::int64_t, 3> origin{};
    JPH::BroadPhaseLayerInterfaceTable broad_phase{layers::count, 2};
    JPH::ObjectLayerPairFilterTable pair_filter{layers::count};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad_phase;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl temp_allocator;
    JPH::JobSystemSingleThreaded jobs;
    std::map<voxel::ChunkKey, TerrainBody> terrain;
    JPH::BodyID ball;
    float ball_mass = 0.0f;
};

PhysicsScene::PhysicsScene(const std::array<std::int64_t, 3>& origin) {
    sp11::ensure_jolt();
    impl_ = std::make_unique<Impl>();
    impl_->origin = origin;
}

PhysicsScene::~PhysicsScene() = default;

void PhysicsScene::install(const std::vector<Collider>& colliders) {
    VEACH_ZONE("sp12 install colliders");
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    for (const Collider& c : colliders) {
        const auto it = impl_->terrain.find(c.key);
        if (c.shape == nullptr) {
            if (it != impl_->terrain.end()) {
                bodies.RemoveBody(it->second.id);
                bodies.DestroyBody(it->second.id);
                impl_->terrain.erase(it);
                impl_->wake(c.key);
            }
            continue;
        }
        if (it != impl_->terrain.end()) {
            bodies.SetShape(it->second.id, c.shape->shape, false, JPH::EActivation::DontActivate);
            it->second.shape_hash = c.hash;
            impl_->wake(c.key);
            continue;
        }
        const JPH::BodyCreationSettings settings(c.shape->shape, impl_->chunk_position(c.key), JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static, layers::static_);
        const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        impl_->terrain.emplace(c.key, Impl::TerrainBody{id, c.hash});
        impl_->wake(c.key);
    }
}

void PhysicsScene::add_ball(const std::array<double, 3>& position, float radius) {
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    JPH::BodyCreationSettings settings(new JPH::SphereShape(radius), JPH::RVec3(position[0], position[1], position[2]),
                                       JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, layers::moving);
    settings.mFriction = 0.6f;
    settings.mRestitution = 0.1f;
    // A rolling ball keeps rolling in a round pit for a long time with Jolt's defaults (0.05); a dug-in rock settles.
    settings.mLinearDamping = 0.2f;
    settings.mAngularDamping = 1.0f;
    impl_->ball = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), impl_->ball);
    impl_->ball_mass = 1.0f / lock.GetBody().GetMotionProperties()->GetInverseMass();
}

void PhysicsScene::step() {
    VEACH_ZONE("sp12 physics step");
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    if (!impl_->ball.IsInvalid() && bodies.IsActive(impl_->ball)) {
        // Radial gravity: toward the planet's center (the body-local origin), from double positions.
        const JPH::RVec3 p = bodies.GetPosition(impl_->ball);
        const double x = static_cast<double>(impl_->origin[0]) * voxel::lod0_cell_meters + p.GetX();
        const double y = static_cast<double>(impl_->origin[1]) * voxel::lod0_cell_meters + p.GetY();
        const double z = static_cast<double>(impl_->origin[2]) * voxel::lod0_cell_meters + p.GetZ();
        const double r = std::sqrt(x * x + y * y + z * z);
        const double f = -gravity * static_cast<double>(impl_->ball_mass) / r;
        bodies.AddForce(impl_->ball,
                        JPH::Vec3(static_cast<float>(f * x), static_cast<float>(f * y), static_cast<float>(f * z)),
                        JPH::EActivation::DontActivate);
    }
    impl_->system.Update(step_seconds, 1, &impl_->temp_allocator, &impl_->jobs);
}

BallState PhysicsScene::ball() const {
    const JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    BallState s;
    if (impl_->ball.IsInvalid()) {
        return s;
    }
    const JPH::RVec3 p = bodies.GetPosition(impl_->ball);
    const JPH::Vec3 v = bodies.GetLinearVelocity(impl_->ball);
    s.position = {p.GetX(), p.GetY(), p.GetZ()};
    s.velocity = {v.GetX(), v.GetY(), v.GetZ()};
    s.active = bodies.IsActive(impl_->ball);
    return s;
}

std::uint64_t PhysicsScene::ball_hash() const {
    const JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    Hash h;
    const JPH::RVec3 p = bodies.GetPosition(impl_->ball);
    const JPH::Quat q = bodies.GetRotation(impl_->ball);
    const JPH::Vec3 v = bodies.GetLinearVelocity(impl_->ball);
    const JPH::Vec3 w = bodies.GetAngularVelocity(impl_->ball);
    h.add(p.GetX());
    h.add(p.GetY());
    h.add(p.GetZ());
    h.add(q.GetX());
    h.add(q.GetY());
    h.add(q.GetZ());
    h.add(q.GetW());
    h.add(v.GetX());
    h.add(v.GetY());
    h.add(v.GetZ());
    h.add(w.GetX());
    h.add(w.GetY());
    h.add(w.GetZ());
    return h.value;
}

std::uint64_t PhysicsScene::install_hash() const {
    const JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    Hash h;
    for (const auto& [key, body] : impl_->terrain) {
        h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)));
        h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.y)));
        h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.z)));
        h.add(static_cast<std::uint64_t>(body.id.GetIndexAndSequenceNumber()));
        const JPH::RVec3 p = bodies.GetPosition(body.id);
        h.add(p.GetX());
        h.add(p.GetY());
        h.add(p.GetZ());
        h.add(body.shape_hash);
    }
    return h.value;
}

std::uint64_t PhysicsScene::jolt_state_hash() const {
    JPH::StateRecorderImpl recorder;
    impl_->system.SaveState(recorder);
    return hash_bytes(recorder.GetData());
}

std::size_t PhysicsScene::terrain_bodies() const {
    return impl_->terrain.size();
}

}  // namespace veach::sp12
