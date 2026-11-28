#pragma once

// engine/physics's private Jolt side: the includes, the layer tables, conversions and the shared pieces of the
// sources. Nothing outside engine/physics/src includes this (ADR 0065, check_repo.py).

#include <veach/physics/physics.hpp>

#include <Jolt/Jolt.h>  // Must come first.

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <cstdint>
#include <string>

namespace veach {
class JobSystem;
class TaskGroup;
}  // namespace veach

namespace veach::physics {

struct Shape::Impl {
    JPH::RefConst<JPH::Shape> shape;
    bool has_ring = false;  ///< a terrain collider with adjacency-ring triangles (contacts with them are rejected)
};

namespace detail {

namespace layers {
inline constexpr JPH::ObjectLayer fixed = 0;   ///< static terrain and structures
inline constexpr JPH::ObjectLayer moving = 1;  ///< kinematic and dynamic bodies, characters' queries
inline constexpr JPH::uint count = 2;
}  // namespace layers

inline constexpr std::uint64_t fnv_basis = 14695981039346656037ULL;
inline constexpr std::uint64_t fnv_prime = 1099511628211ULL;

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

[[nodiscard]] inline std::uint64_t hash_bytes(const std::string& bytes) {
    std::uint64_t h = fnv_basis;
    for (const char c : bytes) {
        h = (h ^ static_cast<unsigned char>(c)) * fnv_prime;
    }
    return h;
}

/// The material of adjacency-ring triangles (ADR 0083): contacts with it are rejected, for rigid bodies and
/// characters alike, so ring triangles only tell Jolt which border edges are internal.
[[nodiscard]] const JPH::PhysicsMaterial* ring_material();

[[nodiscard]] inline JPH::Vec3 fvec(const math::dvec3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
[[nodiscard]] inline JPH::Vec3 fvec(const math::vec3& v) {
    return {v.x, v.y, v.z};
}
[[nodiscard]] inline JPH::RVec3 rvec(const math::dvec3& v) {
    return {v.x, v.y, v.z};
}
[[nodiscard]] inline math::dvec3 dvec(JPH::RVec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
[[nodiscard]] inline math::dvec3 dvec(JPH::Vec3Arg v) {
    return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())};
}
[[nodiscard]] inline math::vec3 mvec(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
/// A double quaternion rounded to float32 and normalized: every rotation that enters Jolt goes through here.
[[nodiscard]] inline JPH::Quat fquat(const math::dquat& q) {
    return JPH::Quat(static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w))
        .Normalized();
}
[[nodiscard]] inline math::quat mquat(JPH::QuatArg q) {
    return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()};
}
[[nodiscard]] inline math::dquat dquat(JPH::QuatArg q) {
    return {static_cast<double>(q.GetW()), static_cast<double>(q.GetX()), static_cast<double>(q.GetY()),
            static_cast<double>(q.GetZ())};
}

/// Jolt's JobSystemWithBarrier over veach::JobSystem (ADR 0073, 0075 decision 4): Jolt's jobs run as TaskGroup
/// jobs on the engine's workers; the thread waiting at a barrier runs ready jobs itself, so one thread works too.
class JoltJobs final : public JPH::JobSystemWithBarrier {
public:
    JoltJobs(veach::JobSystem* jobs, std::uint32_t max_concurrency);
    ~JoltJobs() override;
    JoltJobs(const JoltJobs&) = delete;
    JoltJobs& operator=(const JoltJobs&) = delete;
    JoltJobs(JoltJobs&&) = delete;
    JoltJobs& operator=(JoltJobs&&) = delete;

    /// Opens the group the next jobs run in (one per PhysicsSystem::Update) and waits for it.
    void begin();
    void end();

    int GetMaxConcurrency() const override;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 dependencies = 0) override;
    void CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& function, JobHandle* out,
                    JPH::uint32 count, JPH::uint32 dependencies = 0) override;
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

private:
    veach::JobSystem* jobs_;
    std::uint32_t max_concurrency_;
    std::unique_ptr<veach::TaskGroup> group_;
    JPH::FixedSizeFreeList<Job> free_;
};

}  // namespace detail
}  // namespace veach::physics
