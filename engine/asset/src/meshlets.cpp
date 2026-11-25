#include <veach/asset/scene.hpp>
#include <veach/core/profile.hpp>

#include <meshoptimizer.h>

#include <cstddef>
#include <vector>

namespace veach {

void build_meshlets(MeshAsset& mesh) {
    VEACH_ZONE("build_meshlets");
    mesh.meshlets.clear();
    mesh.meshlet_vertices.clear();
    mesh.meshlet_triangles.clear();
    mesh.meshlet_bounds.clear();
    const std::size_t triangles = mesh.triangle_slots.size();
    const auto* positions = mesh.vertices.empty() ? nullptr : mesh.vertices.front().position.data();
    // Primitives are appended in slot order, so each slot's triangles are one contiguous run.
    std::size_t first = 0;
    while (first < triangles) {
        const std::uint32_t slot = mesh.triangle_slots[first];
        std::size_t last = first;
        while (last < triangles && mesh.triangle_slots[last] == slot) {
            ++last;
        }
        const std::uint32_t* indices = mesh.indices.data() + first * 3;
        const std::size_t index_count = (last - first) * 3;
        const std::size_t bound = meshopt_buildMeshletsBound(index_count, meshlet_max_vertices, meshlet_max_triangles);
        std::vector<meshopt_Meshlet> built(bound);
        std::vector<unsigned int> local_vertices(index_count);
        std::vector<unsigned char> local_triangles(index_count);
        built.resize(meshopt_buildMeshlets(built.data(), local_vertices.data(), local_triangles.data(), indices,
                                           index_count, positions, mesh.vertices.size(), sizeof(MeshVertex),
                                           meshlet_max_vertices, meshlet_max_triangles, 0.0f));
        for (const meshopt_Meshlet& m : built) {
            meshopt_optimizeMeshlet(&local_vertices[m.vertex_offset], &local_triangles[m.triangle_offset],
                                    m.triangle_count, m.vertex_count);
            const meshopt_Bounds b =
                meshopt_computeMeshletBounds(&local_vertices[m.vertex_offset], &local_triangles[m.triangle_offset],
                                             m.triangle_count, positions, mesh.vertices.size(), sizeof(MeshVertex));
            Meshlet out;
            out.vertex_offset = static_cast<std::uint32_t>(mesh.meshlet_vertices.size());
            out.triangle_offset = static_cast<std::uint32_t>(mesh.meshlet_triangles.size());
            out.vertex_count = m.vertex_count;
            out.triangle_count = m.triangle_count;
            out.slot = slot;
            mesh.meshlet_vertices.insert(mesh.meshlet_vertices.end(), &local_vertices[m.vertex_offset],
                                         &local_vertices[m.vertex_offset] + m.vertex_count);
            for (std::uint32_t t = 0; t < m.triangle_count; ++t) {
                const unsigned char* corner = &local_triangles[m.triangle_offset + t * 3];
                mesh.meshlet_triangles.push_back(std::uint32_t{corner[0]} | (std::uint32_t{corner[1]} << 8) |
                                                 (std::uint32_t{corner[2]} << 16));
            }
            MeshletBounds bounds;
            bounds.sphere = {b.center[0], b.center[1], b.center[2], b.radius};
            bounds.cone = {b.cone_axis[0], b.cone_axis[1], b.cone_axis[2], b.cone_cutoff};
            mesh.meshlets.push_back(out);
            mesh.meshlet_bounds.push_back(bounds);
        }
        first = last;
    }
}

}  // namespace veach
