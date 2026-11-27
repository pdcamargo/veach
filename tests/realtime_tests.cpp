// The real-time renderer (docs/specs/realtime-renderer/spec.md).

#include <veach/asset/scene.hpp>
#include <veach/render/camera.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

TEST_CASE("[rt.AC4] exposure follows the physical camera") {
    Camera camera;
    CHECK(ev100_of(camera) == doctest::Approx(0.0));  // f/1, 1 s, ISO 100: existing scenes keep their exposure
    camera.aperture = 16.0f;
    camera.shutter = 1.0f / 125.0f;
    camera.iso = 100.0f;
    CHECK(ev100_of(camera) == doctest::Approx(std::log2(16.0 * 16.0 * 125.0)));  // sunny 16: about EV 15
    const float before = ev100_of(camera);
    camera.shutter *= 2.0f;  // twice the light reaches the sensor: one stop less EV, twice the image value
    CHECK(ev100_of(camera) == doctest::Approx(before - 1.0f));
    const auto exposure = [](float ev100) {
        return 1.0f / (1.2f * std::exp2(ev100));
    };
    CHECK(exposure(ev100_of(camera)) == doctest::Approx(2.0f * exposure(before)));
    camera.ev100 = 9.0f;  // an explicit EV100 wins
    CHECK(ev100_of(camera) == doctest::Approx(9.0));
}
