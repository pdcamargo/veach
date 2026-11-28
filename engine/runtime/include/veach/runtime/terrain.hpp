#pragma once

// Voxel terrain in a world (docs/specs/voxel-terrain/spec.md, guide: docs/guides/voxel-terrain.md). The thin
// binding between veach::voxel and the rest of the engine: it lives in engine/runtime because it needs both the ECS
// (frames, viewers) and the renderer (MeshStore), and engine/voxel links neither render nor physics (ADR 0065).
//
//   engine.import<veach::TerrainModule>();
//   auto& terrain = veach::services(world).get<veach::TerrainBodies>();
//   terrain.add(planet_frame, std::make_shared<veach::voxel::TerrainBody>(density, settings),
//               {.materials = {grass, rock, snow}});
//   camera.add<veach::TerrainViewer>();
//
// Every frame (PreExtract), each body's viewers are the TerrainViewer entities, their FrameTransform converted into
// the body's frame in double (frame_pose, ADR 0045). The body streams; chunks meshed this update were built into
// render meshes on their own jobs and are added to the MeshStore here, on the main thread, in a fixed order; evicted
// chunks are removed. Extraction draws the drawn set in render space: each chunk's origin goes from lattice
// coordinates to the body's frame in double, then through RenderSpaceMap relative to the render origin
// (ADR 0074 rule 3), and only then to float; vertices are chunk-relative floats.

#include <veach/core/ids.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/voxel/terrain.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace veach {

/// Terrain streams around this entity (a camera, a player). Scriptable.
struct TerrainViewer {};

/// How a body's chunks are drawn.
struct TerrainLook {
    /// MeshStore materials by voxel material id (a triangle takes its vertices' highest id); ids past the end use the
    /// last one, and none draws the default material.
    std::vector<AssetId> materials;
};

/// The terrain bodies of a world, each in its reference frame. A service of TerrainModule.
class TerrainBodies {
public:
    /// Puts `body` in `frame` (a ReferenceFrame entity, or a null Entity for the root world): body-local meters are
    /// the frame's coordinates. Replaces a body already there.
    voxel::TerrainBody& add(Entity frame, std::shared_ptr<voxel::TerrainBody> body, TerrainLook look = {});
    void remove(Entity frame);
    [[nodiscard]] voxel::TerrainBody* find(Entity frame) const;
    /// The last update's numbers for the body in `frame`.
    [[nodiscard]] const voxel::StreamStats* stats(Entity frame) const;
    /// Bodies in frame-id order.
    [[nodiscard]] std::vector<std::uint64_t> frames() const;

    struct Body;
    /// The binding's own state (the module's systems use it).
    [[nodiscard]] std::map<std::uint64_t, std::unique_ptr<Body>>& bodies() { return bodies_; }

    TerrainBodies();
    ~TerrainBodies();
    TerrainBodies(const TerrainBodies&) = delete;
    TerrainBodies& operator=(const TerrainBodies&) = delete;

private:
    std::map<std::uint64_t, std::unique_ptr<Body>> bodies_;
};

/// Registers TerrainViewer, provides TerrainBodies, and streams and draws every body each frame.
struct TerrainModule {
    explicit TerrainModule(World& world);
};

}  // namespace veach
