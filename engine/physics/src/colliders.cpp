// Terrain colliders (physics.hpp): cooking a ColliderMesh into a Jolt MeshShape, and the adjacency ring of ADR 0083.
// Installing them is the world's (world.cpp).

#include "jolt.hpp"

#include <veach/core/profile.hpp>

#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <algorithm>
#include <format>
#include <map>
#include <memory>

namespace veach::physics {

ColliderMesh with_adjacency_ring(const ChunkKey& key, const LatticePoint& origin, std::int64_t extent,
                                 std::span<const ChunkTriangles, 27> neighborhood, int fixed_bits) {
    ColliderMesh out;
    out.key = key;
    out.origin = origin;
    out.extent = extent;
    // A neighbor triangle joins the ring when a vertex lies on the chunk's box (on a face, edge or corner of it).
    const auto touches = [&](const std::array<std::int64_t, 3>& p) {
        bool on_face = false;
        for (std::size_t a = 0; a < 3; ++a) {
            const std::int64_t lo = origin[a] << fixed_bits, hi = (origin[a] + extent) << fixed_bits;
            if (p[a] < lo || p[a] > hi) {
                return false;
            }
            on_face = on_face || p[a] == lo || p[a] == hi;
        }
        return on_face;
    };
    std::map<std::array<std::int64_t, 3>, std::uint32_t> weld;
    const auto vertex = [&](const std::array<std::int64_t, 3>& p) {
        const auto [it, inserted] = weld.emplace(p, static_cast<std::uint32_t>(out.vertices.size()));
        if (inserted) {
            out.vertices.push_back(p);
        }
        return it->second;
    };
    for (std::size_t n = 0; n < neighborhood.size(); ++n) {
        const ChunkTriangles& c = neighborhood[n];
        const bool own = n == 13;
        for (std::size_t t = 0; t + 2 < c.indices.size(); t += 3) {
            const auto& a = c.vertices[c.indices[t]];
            const auto& b = c.vertices[c.indices[t + 1]];
            const auto& d = c.vertices[c.indices[t + 2]];
            if (own || touches(a) || touches(b) || touches(d)) {
                // One at a time: the weld numbers vertices in first-use order, a, b, then d.
                const std::uint32_t ia = vertex(a);
                const std::uint32_t ib = vertex(b);
                const std::uint32_t id = vertex(d);
                out.indices.insert(out.indices.end(), {ia, ib, id});
                out.adjacency.push_back(own ? 0 : 1);
            }
        }
    }
    return out;
}

Result<CookedCollider> cook_collider(const ColliderMesh& mesh, const Lattice& lattice) {
    VEACH_ZONE("physics cook collider");
    initialize();
    CookedCollider out;
    out.key = mesh.key;
    out.origin = mesh.origin;
    out.extent = mesh.extent;
    if (mesh.indices.empty()) {
        return out;
    }
    const std::size_t triangles = mesh.indices.size() / 3;
    const auto invalid = [&](std::string what) {
        return make_error(ErrorCode::physics_shape_invalid,
                          std::format("terrain collider ({}, {}, {}, level {}): {}", mesh.key.x, mesh.key.y, mesh.key.z,
                                      mesh.key.level, what),
                          "Hand over the mesher's ChunkMesh as it is: fixed-point vertices, three indices per "
                          "triangle, and one adjacency byte per triangle or none.");
    };
    if (mesh.indices.size() % 3 != 0) {
        return invalid(std::format("{} indices is not a whole number of triangles", mesh.indices.size()));
    }
    if (!mesh.adjacency.empty() && mesh.adjacency.size() != triangles) {
        return invalid(std::format("{} adjacency flags for {} triangles", mesh.adjacency.size(), triangles));
    }
    // Lattice units × 2^bits relative to the chunk origin, to meters: exact in double, and exact in float for any
    // vertex within 2^24 fixed steps of the corner (64 m for voxel's LOD0 lattice).
    const double scale = lattice.unit_meters / static_cast<double>(std::int64_t{1} << lattice.fixed_bits);
    JPH::VertexList vertices;
    vertices.reserve(mesh.vertices.size());
    for (const auto& p : mesh.vertices) {
        std::array<float, 3> f{};
        for (std::size_t a = 0; a < 3; ++a) {
            f[a] = static_cast<float>(static_cast<double>(p[a] - (mesh.origin[a] << lattice.fixed_bits)) * scale);
        }
        vertices.push_back(JPH::Float3(f[0], f[1], f[2]));
    }
    JPH::IndexedTriangleList list;
    list.reserve(triangles);
    for (std::size_t t = 0; t < triangles; ++t) {
        const std::uint32_t a = mesh.indices[3 * t], b = mesh.indices[3 * t + 1], c = mesh.indices[3 * t + 2];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size()) {
            return invalid(std::format("triangle {} names a vertex past the {} given", t, mesh.vertices.size()));
        }
        const JPH::uint32 material = mesh.adjacency.empty() || mesh.adjacency[t] == 0 ? 0u : 1u;
        list.push_back(JPH::IndexedTriangle(a, b, c, material));
    }
    JPH::ShapeSettings::ShapeResult result;
    if (mesh.adjacency.empty()) {
        result = JPH::MeshShapeSettings(std::move(vertices), std::move(list)).Create();
    } else {
        JPH::PhysicsMaterialList materials;
        materials.push_back(JPH::PhysicsMaterial::sDefault);
        materials.push_back(detail::ring_material());
        result = JPH::MeshShapeSettings(std::move(vertices), std::move(list), std::move(materials)).Create();
    }
    if (result.HasError()) {
        return invalid(result.GetError().c_str());
    }
    auto impl = std::make_shared<Shape::Impl>();
    impl->shape = result.Get();
    impl->has_ring = std::find(mesh.adjacency.begin(), mesh.adjacency.end(), std::uint8_t{1}) != mesh.adjacency.end();
    out.shape = Shape(std::move(impl));
    out.triangles = out.shape.triangle_count();
    return out;
}

}  // namespace veach::physics
