// Spike SP-11 (VCH-087), throwaway: see sp11_scene.hpp. Every input is built from integers and + - * / so it has the
// same bits on every platform; only Jolt's own arithmetic is under test.

#include "sp11/sp11_scene.hpp"

#include <Jolt/Jolt.h>  // Must come first.

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace veach::sp11 {
namespace {

void trace(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
    std::fprintf(stderr, "Jolt assert %s:%u: (%s) %s\n", file, line, expression, message ? message : "");
    return true;  // Break.
}
#endif

/// Jolt's process-wide state: allocator hooks, the type factory and the collision dispatch tables.
struct JoltRuntime {
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assert_failed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltRuntime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;
};

void ensure_jolt() {
    static const JoltRuntime runtime;
}

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/// A value in [-1, 1] from an integer, in steps of 1/1000 (exact integer arithmetic, one rounded division).
double unit(std::uint64_t key) {
    return static_cast<double>(static_cast<std::int64_t>(mix(key) % 2001) - 1000) / 1000.0;
}

struct Hash {
    std::uint64_t value = 14695981039346656037ULL;
    void add(std::uint64_t bits) { value = mix(value ^ bits) * 1099511628211ULL; }
    void add(double v) { add(std::bit_cast<std::uint64_t>(v)); }
    void add(float v) { add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v))); }
};

namespace layers {
constexpr JPH::ObjectLayer static_ = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
}  // namespace layers

constexpr float step_seconds = 1.0f / 60.0f;
constexpr float gravity = 9.81f;
constexpr JPH::uint32 ground_samples = 64;  ///< 64 x 64 heights, 1 m apart: a bowl 63 m across.

/// The ground: a paraboloid bowl, 15.5 m deep at the corners, centered on the scene origin. Heights are exact floats.
/// As a HeightFieldShape (8-bit compressed samples) or the same grid as a MeshShape (two triangles per cell; ADR 0058's
/// terrain colliders are meshes).
JPH::RefConst<JPH::Shape> make_ground(Ground kind) {
    std::vector<float> heights(ground_samples * ground_samples);
    for (JPH::uint32 z = 0; z < ground_samples; ++z) {
        for (JPH::uint32 x = 0; x < ground_samples; ++x) {
            const int dx = 2 * static_cast<int>(x) - 63;
            const int dz = 2 * static_cast<int>(z) - 63;
            heights[z * ground_samples + x] = static_cast<float>(dx * dx + dz * dz) / 512.0f;
        }
    }
    JPH::ShapeSettings::ShapeResult result;
    if (kind == Ground::heightfield) {
        const JPH::HeightFieldShapeSettings settings(heights.data(), JPH::Vec3(-31.5f, 0.0f, -31.5f), JPH::Vec3::sOne(),
                                                     ground_samples);
        result = settings.Create();
    } else {
        JPH::VertexList vertices;
        JPH::IndexedTriangleList triangles;
        for (JPH::uint32 z = 0; z < ground_samples; ++z) {
            for (JPH::uint32 x = 0; x < ground_samples; ++x) {
                vertices.push_back(JPH::Float3(static_cast<float>(x) - 31.5f, heights[z * ground_samples + x],
                                               static_cast<float>(z) - 31.5f));
            }
        }
        for (JPH::uint32 z = 0; z + 1 < ground_samples; ++z) {
            for (JPH::uint32 x = 0; x + 1 < ground_samples; ++x) {
                const JPH::uint32 i = z * ground_samples + x;
                // Counter-clockwise seen from above (+Y): Jolt's front face.
                triangles.push_back(JPH::IndexedTriangle(i, i + ground_samples, i + 1, 0));
                triangles.push_back(JPH::IndexedTriangle(i + 1, i + ground_samples, i + ground_samples + 1, 0));
            }
        }
        const JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
        result = settings.Create();
    }
    if (result.HasError()) {
        trace("sp11: ground: %s", result.GetError().c_str());
        return nullptr;
    }
    return result.Get();
}

std::uint64_t hash_bytes(const std::string& bytes) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const char c : bytes) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return h;
}

}  // namespace

SceneResult run_scene(const SceneConfig& config) {
    ensure_jolt();
    SceneResult out;

    JPH::ObjectLayerPairFilterTable pair_filter(layers::count);
    pair_filter.EnableCollision(layers::static_, layers::moving);
    pair_filter.EnableCollision(layers::moving, layers::moving);
    JPH::BroadPhaseLayerInterfaceTable broad_phase(layers::count, 2);
    broad_phase.MapObjectToBroadPhaseLayer(layers::static_, JPH::BroadPhaseLayer(0));
    broad_phase.MapObjectToBroadPhaseLayer(layers::moving, JPH::BroadPhaseLayer(1));
    const JPH::ObjectVsBroadPhaseLayerFilterTable object_vs_broad_phase(broad_phase, 2, pair_filter, layers::count);

    const auto body_count = static_cast<JPH::uint>(config.bodies);
    JPH::PhysicsSystem system;
    system.Init(body_count + 16, 0, 65536, 65536, broad_phase, object_vs_broad_phase, pair_filter);
    // ADR 0058: world gravity is zero; a per-body force stands in for the gravity registry.
    system.SetGravity(JPH::Vec3::sZero());

    JPH::TempAllocatorImpl temp_allocator(64 * 1024 * 1024);
    std::unique_ptr<JPH::JobSystem> jobs;
    if (config.workers <= 0) {
        jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    } else {
        jobs =
            std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, config.workers);
    }

    const JPH::RVec3 origin(config.offset[0], config.offset[1], config.offset[2]);
    JPH::BodyInterface& bodies = system.GetBodyInterface();

    const JPH::RefConst<JPH::Shape> ground = make_ground(config.ground);
    if (ground == nullptr) {
        return out;
    }
    bodies.CreateAndAddBody(
        JPH::BodyCreationSettings(ground, origin, JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::static_),
        JPH::EActivation::DontActivate);

    const JPH::RefConst<JPH::Shape> shapes[] = {
        new JPH::BoxShape(JPH::Vec3(0.4f, 0.3f, 0.5f)),
        new JPH::SphereShape(0.45f),
        new JPH::CapsuleShape(0.4f, 0.3f),
    };

    // A column of layers, side x side bodies each, 1.5 m apart, starting 4 m above the bowl's center.
    int side = 6;
    while (side * side * 10 < config.bodies) {
        ++side;
    }
    std::vector<JPH::BodyID> ids;
    std::vector<JPH::Vec3> weights;  // Per-body gravity force, m * g.
    ids.reserve(body_count);
    weights.reserve(body_count);
    for (int i = 0; i < config.bodies; ++i) {
        const auto key = static_cast<std::uint64_t>(i) * 16;
        const int layer = i / (side * side);
        const int cell = i % (side * side);
        const double half = static_cast<double>(side - 1) / 2.0;
        const JPH::RVec3 local((static_cast<double>(cell % side) - half) * 1.5 + 0.1 * unit(key + 0) + config.nudge,
                               4.0 + 1.5 * static_cast<double>(layer) + 0.1 * unit(key + 1),
                               (static_cast<double>(cell / side) - half) * 1.5 + 0.1 * unit(key + 2));
        const JPH::Quat rotation = JPH::Quat(static_cast<float>(unit(key + 3)), static_cast<float>(unit(key + 4)),
                                             static_cast<float>(unit(key + 5)), 1.5f)
                                       .Normalized();
        JPH::BodyCreationSettings settings(shapes[config.boxes_only ? 0 : i % 3], origin + local, rotation,
                                           JPH::EMotionType::Dynamic, layers::moving);
        settings.mAngularVelocity = JPH::Vec3(static_cast<float>(unit(key + 6)), static_cast<float>(unit(key + 7)),
                                              static_cast<float>(unit(key + 8)));
        settings.mFriction = 0.5f;
        settings.mRestitution = 0.1f;
        const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
        float inverse_mass = 0.0f;
        {
            const JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
            inverse_mass = lock.GetBody().GetMotionProperties()->GetInverseMass();
        }
        ids.push_back(id);
        weights.emplace_back(0.0f, -gravity / inverse_mass, 0.0f);
    }
    system.OptimizeBroadPhase();

    Hash chain;
    out.step_hashes.reserve(static_cast<std::size_t>(config.steps));
    if (config.time_steps) {
        out.step_ms.reserve(static_cast<std::size_t>(config.steps));
    }
    for (int step = 0; step < config.steps; ++step) {
        for (std::size_t b = 0; b < ids.size(); ++b) {
            if (bodies.IsActive(ids[b])) {
                bodies.AddForce(ids[b], weights[b], JPH::EActivation::DontActivate);
            }
        }
        const auto start = std::chrono::steady_clock::now();
        const JPH::EPhysicsUpdateError error = system.Update(step_seconds, 1, &temp_allocator, jobs.get());
        if (config.time_steps) {
            out.step_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        if (error != JPH::EPhysicsUpdateError::None) {
            ++out.update_errors;
        }

        Hash state;
        for (const JPH::BodyID& id : ids) {
            const JPH::RVec3 p = bodies.GetPosition(id) - origin;
            const JPH::Quat q = bodies.GetRotation(id);
            const JPH::Vec3 v = bodies.GetLinearVelocity(id);
            const JPH::Vec3 w = bodies.GetAngularVelocity(id);
            state.add(p.GetX());
            state.add(p.GetY());
            state.add(p.GetZ());
            state.add(q.GetX());
            state.add(q.GetY());
            state.add(q.GetZ());
            state.add(q.GetW());
            state.add(v.GetX());
            state.add(v.GetY());
            state.add(v.GetZ());
            state.add(w.GetX());
            state.add(w.GetY());
            state.add(w.GetZ());
        }
        out.step_hashes.push_back(state.value);
        chain.add(state.value);
    }
    out.chain = chain.value;

    JPH::StateRecorderImpl recorder;
    system.SaveState(recorder);
    out.jolt_state = hash_bytes(recorder.GetData());

    out.positions.reserve(ids.size() * 3);
    for (const JPH::BodyID& id : ids) {
        const JPH::RVec3 p = bodies.GetPosition(id) - origin;
        out.positions.push_back(p.GetX());
        out.positions.push_back(p.GetY());
        out.positions.push_back(p.GetZ());
        out.max_speed = std::max(out.max_speed, static_cast<double>(bodies.GetLinearVelocity(id).Length()));
    }
    out.active_bodies = static_cast<int>(system.GetNumActiveBodies(JPH::EBodyType::RigidBody));

    return out;
}

}  // namespace veach::sp11
