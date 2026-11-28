// Spike SP-12 (VCH-088), throwaway: Jolt MeshShape colliders cooked from LOD0 chunk meshes, installed in a physics
// scene near its own origin (ADR 0045, 0058), and a ball (docs/research/spikes/sp-12-dig-collide.md). Jolt types stay
// in sp12_physics.cpp.
#pragma once

#include <veach/voxel/voxel.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace veach::sp12 {

struct CookedShape;  ///< a Jolt MeshShape

struct CookOptions {
    bool favor_build_speed = false;  ///< MeshShapeSettings::EBuildQuality::FavorBuildSpeed
};

/// A collider cooked from one chunk's mesh, ready to install. `shape` is null for a chunk without triangles.
struct Collider {
    voxel::ChunkKey key;
    std::shared_ptr<const CookedShape> shape;
    std::uint64_t hash = 0;  ///< of Jolt's binary state of the shape (its cooked tree), 0 when empty
    std::uint32_t triangles = 0;
    double cook_ms = 0.0;
};

/// Cooks `mesh` (chunk-relative float positions, exact multiples of 2^-17 m) into a MeshShape. Thread-safe: run it
/// on job threads, one chunk per call.
Collider cook_collider(const voxel::ChunkMesh& mesh, const CookOptions& options = {});

/// The ball's state, relative to the scene origin.
struct BallState {
    std::array<double, 3> position{};
    std::array<double, 3> velocity{};
    bool active = false;
};

/// One body-fixed surface scene (ADR 0058): its origin is a LOD0 lattice point near the bodies, its axes are the
/// body-fixed axes (only translated, so a chunk's position in the scene is an exact integer difference). World gravity
/// is zero; before each step the ball gets m·g toward the planet's center (radial, from double positions).
class PhysicsScene {
public:
    /// `origin` in LOD0 lattice units, body-local (the planet's center is the body-local origin).
    explicit PhysicsScene(const std::array<std::int64_t, 3>& origin);
    ~PhysicsScene();
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    /// Installs colliders in the order given (key order): a new key adds a static body at the chunk's origin, a known
    /// key swaps its shape (and wakes the bodies over it), a null shape removes the body.
    void install(const std::vector<Collider>& colliders);
    /// Adds the ball (radius in meters) at `position`, relative to the scene origin, at rest.
    void add_ball(const std::array<double, 3>& position, float radius);
    /// One fixed step of 1/60 s.
    void step();

    BallState ball() const;
    /// The ball's position (double bits), rotation, linear and angular velocity (float bits): the per-step hash.
    std::uint64_t ball_hash() const;
    /// Every terrain body in key order: key, body id, position bits, shape hash. Equal after equal installs.
    std::uint64_t install_hash() const;
    /// Jolt's own PhysicsSystem::SaveState (bodies, contacts, warm-start impulses).
    std::uint64_t jolt_state_hash() const;
    std::size_t terrain_bodies() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::sp12
