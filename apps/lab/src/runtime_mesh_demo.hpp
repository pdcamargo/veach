#pragma once

// Runtime meshes (docs/specs/runtime-meshes/spec.md, VCH-102): a heightfield patch meshed at run time, the shape of a
// terrain chunk, drawn through the engine's MeshStore with two materials made at run time (grass below, rock above).
// Built only on the public veach:: API, like a terrain module would be. Scripts: scripts/runtime_mesh_realtime.luau,
// scripts/runtime_mesh.luau (photo mode).

#include <veach/veach.hpp>

namespace lab {

/// Setting it meshes the patch (again, after a change: an update, not a new mesh); removing it frees the mesh.
/// Scriptable.
struct RuntimeMeshDemo {
    float size = 8.0f;         ///< meters along X and Z, centered on the entity
    std::uint32_t cells = 64;  ///< quads along each side (1 to 512)
    float height = 0.6f;       ///< meters: the relief's amplitude
    float rock_above = 0.15f;  ///< meters: triangles higher than this are rock (slot 1), the rest grass (slot 0)
};

/// The patch's mesh: (cells + 1)^2 vertices with analytic normals, two triangles per cell. Deterministic.
[[nodiscard]] veach::render::MeshData runtime_mesh_demo_data(const RuntimeMeshDemo& demo, veach::AssetId grass,
                                                             veach::AssetId rock);

void register_runtime_mesh_demo(veach::World& world);

}  // namespace lab
