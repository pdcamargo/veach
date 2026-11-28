// The voxel-to-physics collider bridge (veach/runtime/terrain_colliders.hpp).

#include <veach/core/log.hpp>
#include <veach/runtime/terrain_colliders.hpp>

#include <format>

namespace veach {

physics::ColliderMesh physics_collider(const voxel::ColliderMesh& mesh) {
    physics::ColliderMesh out;
    out.key = {.level = mesh.key.level, .x = mesh.key.x, .y = mesh.key.y, .z = mesh.key.z};
    out.origin = voxel::chunk_origin(mesh.key);
    out.extent = voxel::chunk_units(mesh.key.level);
    out.vertices = mesh.positions;
    out.indices = mesh.indices;
    const std::size_t triangles = mesh.indices.size() / 3;
    if (triangles > mesh.own_triangles) {
        out.adjacency.assign(triangles, std::uint8_t{0});
        for (std::size_t t = mesh.own_triangles; t < triangles; ++t) {
            out.adjacency[t] = 1;  // the neighbors' border triangles: adjacency only (ADR 0083)
        }
    }
    return out;
}

void TerrainColliders::submit(voxel::ColliderUpdate update) {
    const std::int64_t expected = world_.tick() + physics::collider_install_delay;
    if (static_cast<std::int64_t>(update.install_tick) != expected) {
        log::error("physics",
                   Error{ErrorCode::physics_collider_tick,
                         std::format("terrain colliders for tick {} arrived at physics tick {}; they install "
                                     "before step {}",
                                     update.install_tick, world_.tick(), expected),
                         "Call TerrainBody::update_colliders with the physics world's tick()."});
    }
    std::vector<physics::ColliderMesh> meshes;
    meshes.reserve(update.install.size() + update.remove.size());
    for (const voxel::ColliderMesh& m : update.install) {
        meshes.push_back(physics_collider(m));
    }
    for (const voxel::ChunkKey& k : update.remove) {
        meshes.push_back({.key = {.level = k.level, .x = k.x, .y = k.y, .z = k.z},
                          .origin = voxel::chunk_origin(k),
                          .extent = voxel::chunk_units(k.level)});
    }
    if (auto requested = world_.request_colliders(scene_, std::move(meshes)); !requested) {
        log::error("physics", requested.error());
    }
}

}  // namespace veach
