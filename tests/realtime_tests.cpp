// The real-time renderer v0 (docs/specs/realtime-renderer-v0/spec.md).

#include <veach/asset/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <vector>

using namespace veach;

namespace {

/// An n x n grid of quads in the XZ plane; the left half is slot 0, the right half slot 1.
MeshAsset grid(std::uint32_t n) {
    MeshAsset mesh;
    for (std::uint32_t z = 0; z <= n; ++z) {
        for (std::uint32_t x = 0; x <= n; ++x) {
            MeshVertex v;
            v.position = {static_cast<float>(x), 0.0f, static_cast<float>(z)};
            mesh.vertices.push_back(v);
        }
    }
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = slot * n / 2; x < (slot + 1) * n / 2; ++x) {
                const std::uint32_t a = z * (n + 1) + x;
                const std::array<std::uint32_t, 6> quad{a, a + n + 1, a + 1, a + 1, a + n + 1, a + n + 2};
                mesh.indices.insert(mesh.indices.end(), quad.begin(), quad.end());
                mesh.triangle_slots.insert(mesh.triangle_slots.end(), 2, slot);
            }
        }
    }
    mesh.slot_count = 2;
    return mesh;
}

}  // namespace

TEST_CASE("[rt.AC1] meshlets cover every triangle once, within the limits, one slot each, at level 0") {
    MeshAsset mesh = grid(40);  // 3200 triangles: many meshlets per slot
    build_meshlets(mesh);
    REQUIRE(!mesh.meshlets.empty());
    REQUIRE(mesh.meshlet_bounds.size() == mesh.meshlets.size());

    std::vector<std::array<std::uint32_t, 4>> original;  // sorted corners + slot
    for (std::size_t t = 0; t < mesh.triangle_slots.size(); ++t) {
        std::array<std::uint32_t, 4> tri{mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2],
                                         mesh.triangle_slots[t]};
        std::sort(tri.begin(), tri.begin() + 3);
        original.push_back(tri);
    }
    std::vector<std::array<std::uint32_t, 4>> rebuilt;
    for (const Meshlet& m : mesh.meshlets) {
        CHECK(m.vertex_count <= meshlet_max_vertices);
        CHECK(m.triangle_count <= meshlet_max_triangles);
        CHECK(m.lod == 0);
        CHECK(m.parent == meshlet_no_parent);
        for (std::uint32_t t = 0; t < m.triangle_count; ++t) {
            const std::uint32_t packed = mesh.meshlet_triangles[m.triangle_offset + t];
            std::array<std::uint32_t, 4> tri{};
            for (std::uint32_t c = 0; c < 3; ++c) {
                const std::uint32_t local = (packed >> (8 * c)) & 0xFFu;
                REQUIRE(local < m.vertex_count);
                tri.at(c) = mesh.meshlet_vertices[m.vertex_offset + local];
            }
            std::sort(tri.begin(), tri.begin() + 3);
            tri[3] = m.slot;
            rebuilt.push_back(tri);
        }
    }
    std::ranges::sort(original);
    std::ranges::sort(rebuilt);
    CHECK(rebuilt == original);  // every triangle exactly once, with its own slot

    // Deterministic: the cook cache relies on it.
    MeshAsset again = grid(40);
    build_meshlets(again);
    CHECK(again.meshlet_triangles == mesh.meshlet_triangles);
    CHECK(again.meshlet_vertices == mesh.meshlet_vertices);
}
