#pragma once

// The bridge between terrain bodies (engine/voxel, VCH-114) and physics (engine/physics, VCH-113): a voxel
// ColliderSink that hands a body's collider updates to one physics scene. Neither area knows the other (ADR 0065), so
// the bridge lives in runtime, which links both (docs/specs/physics/spec.md, "Terrain colliders").
//
//   const auto scene = *physics.create_scene({.name = "pad", .lattice_origin = voxel::chunk_origin(center)});
//   veach::TerrainColliders colliders(physics, scene);
//   body.set_collider_sink(&colliders);
//   body.update_colliders(probes, physics.tick(), jobs);   // the physics tick: installed before step tick + 2
//
// The scene's lattice is the body's: a body-fixed surface scene on a LOD0 grid point with the frame's axes.

#include <veach/physics/physics.hpp>
#include <veach/voxel/terrain.hpp>

namespace veach {

/// A voxel collider mesh as physics takes it: the same lattice positions and triangles, the ring marked adjacency.
[[nodiscard]] physics::ColliderMesh physics_collider(const voxel::ColliderMesh& mesh);

class TerrainColliders final : public voxel::ColliderSink {
public:
    TerrainColliders(physics::PhysicsWorld& world, physics::SceneId scene) : world_(world), scene_(scene) {}

    /// Requests the update's installs and removals in one batch. Its install tick must be the physics tick + 2
    /// (update_colliders called with physics.tick()); a mismatch is logged as physics.collider_tick and the
    /// colliders install at physics tick + 2 anyway.
    void submit(voxel::ColliderUpdate update) override;

private:
    physics::PhysicsWorld& world_;
    physics::SceneId scene_;
};

}  // namespace veach
