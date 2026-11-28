// Runtime meshes (docs/specs/runtime-meshes/spec.md): the MeshStore, extraction and the scene renderer's GPU cache on
// the null RHI.

#include "support/test_support.hpp"

#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>

using namespace veach;

namespace {

/// An n x n grid of quads in the XZ plane; every other quad is slot 1, so the slots interleave.
render::MeshData checker(std::uint32_t n, float height = 0.0f) {
    render::MeshData data;
    for (std::uint32_t z = 0; z <= n; ++z) {
        for (std::uint32_t x = 0; x <= n; ++x) {
            MeshVertex v;
            v.position = {static_cast<float>(x), height * static_cast<float>(x % 2), static_cast<float>(z)};
            v.normal = {0.0f, 1.0f, 0.0f};
            data.vertices.push_back(v);
        }
    }
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::uint32_t a = z * (n + 1) + x;
            const std::array<std::uint32_t, 6> quad{a, a + n + 1, a + 1, a + 1, a + n + 1, a + n + 2};
            data.indices.insert(data.indices.end(), quad.begin(), quad.end());
            data.triangle_slots.insert(data.triangle_slots.end(), 2, (x + z) % 2);
        }
    }
    return data;
}

/// The triangles of a mesh as sorted corner triples with their slot, to compare meshes up to triangle order.
std::multiset<std::array<std::uint32_t, 4>> triangles_of(const std::vector<std::uint32_t>& indices,
                                                         const std::vector<std::uint32_t>& slots) {
    std::multiset<std::array<std::uint32_t, 4>> out;
    for (std::size_t t = 0; t < slots.size(); ++t) {
        out.insert({indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2], slots[t]});
    }
    return out;
}

/// How many GPU resources made for runtime meshes are alive.
std::size_t runtime_resources(rhi::Device& device) {
    const rhi::MemoryUsage usage = device.memory_usage();
    return static_cast<std::size_t>(std::ranges::count_if(
        usage.resources, [](const rhi::ResourceMemory& r) { return r.name.starts_with("runtime "); }));
}

/// A world, a renderer on the null device with the scene renderer, and a camera.
struct SceneFixture {
    test::TempProject project{"runtime-mesh"};
    std::unique_ptr<rhi::Device> device = std::move(*rhi::create_device({}));
    World world;
    AssetServer assets{world, project.path()};
    render::Renderer renderer{*device};
    render::MeshStore& meshes = renderer.meshes();
    inline static const rhi::ShaderLibrary no_shaders{};  // the null device compiles nothing
    inline static const std::array<render::MaterialKind, 1> kinds{{{"StandardMaterial", false}}};

    SceneFixture() {
        world.import<CoreModule>();
        world.component<Camera>();
        world.component<RuntimeMeshInstance>();
        renderer.add_feature(std::make_unique<render::SceneRenderer>(
            render::SceneShaders{no_shaders, no_shaders, no_shaders}, kinds, assets));
        world.entity("camera")
            .set(Transform{{0.0f, 2.0f, 6.0f}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)})
            .set(Camera{math::radians(60.0f), 0.1f, true});
    }
    void frame() {
        propagate_transforms(world);
        renderer.extract(world);
        renderer.render();
    }
};

}  // namespace

TEST_CASE("[rtmesh.AC1] a submitted mesh is validated, grouped by slot and split into meshlets") {
    const render::MeshData data = checker(8);
    auto built = render::build_runtime_mesh(data);
    REQUIRE(built.has_value());
    const MeshAsset& mesh = built->mesh;
    CHECK(mesh.slot_count == 2);
    CHECK(std::ranges::is_sorted(mesh.triangle_slots));  // one run per slot: meshlets never mix materials
    CHECK(triangles_of(mesh.indices, mesh.triangle_slots) == triangles_of(data.indices, data.triangle_slots));
    CHECK(mesh.bounds_min == math::vec3(0.0f));
    CHECK(mesh.bounds_max == math::vec3(8.0f, 0.0f, 8.0f));
    // Every triangle in exactly one meshlet of its slot, within the cooker's limits.
    std::size_t covered = 0;
    REQUIRE(mesh.meshlet_bounds.size() == mesh.meshlets.size());
    for (const Meshlet& m : mesh.meshlets) {
        CHECK(m.vertex_count <= meshlet_max_vertices);
        CHECK(m.triangle_count <= meshlet_max_triangles);
        covered += m.triangle_count;
    }
    CHECK(covered == mesh.triangle_slots.size());
    CHECK(mesh.meshlets.size() >= 2);  // at least one per slot
    // No slots means one material.
    render::MeshData single = data;
    single.triangle_slots.clear();
    auto one = render::build_runtime_mesh(single);
    REQUIRE(one.has_value());
    CHECK(one->mesh.slot_count == 1);
    CHECK(std::ranges::all_of(one->mesh.triangle_slots, [](std::uint32_t s) { return s == 0; }));

    const auto rejected = [](render::MeshData bad) {
        auto result = render::build_runtime_mesh(std::move(bad));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::render_mesh_invalid);
        CHECK_FALSE(result.error().hint.empty());
    };
    rejected({});
    render::MeshData partial = data;
    partial.indices.pop_back();
    rejected(partial);
    render::MeshData out_of_range = data;
    out_of_range.indices[5] = static_cast<std::uint32_t>(data.vertices.size());
    rejected(out_of_range);
    render::MeshData nan = data;
    nan.vertices[3].position[1] = std::numeric_limits<float>::quiet_NaN();
    rejected(nan);
    render::MeshData short_slots = data;
    short_slots.triangle_slots.pop_back();
    rejected(short_slots);
    render::MeshData many_slots = data;
    many_slots.triangle_slots[0] = render::runtime_mesh_max_slots;
    rejected(many_slots);
}

TEST_CASE("[rtmesh.AC2] the store hands out deterministic ids, updates in place and forgets on remove") {
    render::MeshStore store;
    const auto empty = store.snapshot();
    CHECK(empty->meshes.empty());
    CHECK(store.snapshot() == empty);  // unchanged: the same snapshot

    auto first = store.add(checker(2));
    REQUIRE(first.has_value());
    CHECK(render::is_runtime_id(*first));
    CHECK_FALSE(render::is_runtime_id(AssetId::generate()));
    CHECK_FALSE(render::is_runtime_id(AssetId::derive(AssetId::generate(), "mesh/0")));
    const AssetId material = store.add_material({.name = "rock", .base_color = {0.4f, 0.4f, 0.4f, 1.0f}});
    CHECK(material != *first);
    render::MeshStore other;  // another run makes the same ids in the same order
    CHECK(*other.add(checker(2)) == *first);

    const auto before = store.snapshot();
    CHECK(before != empty);
    CHECK(before->meshes.size() == 1);
    CHECK(before->materials.size() == 1);
    const auto old_data = store.find(*first);
    REQUIRE(store.update(*first, checker(3)).has_value());
    const auto after = store.snapshot();
    CHECK(after != before);
    CHECK(store.find(*first) != old_data);
    CHECK(store.find(*first)->mesh.triangle_slots.size() == 18);
    CHECK(before->meshes.at(*first) == old_data);  // a snapshot never changes under its reader

    // A bad update keeps the old data.
    CHECK(store.update(*first, {}).error().code == ErrorCode::render_mesh_invalid);
    CHECK(store.find(*first)->mesh.triangle_slots.size() == 18);

    REQUIRE(store.remove(*first).has_value());
    CHECK(store.find(*first) == nullptr);
    CHECK(store.mesh_count() == 0);
    CHECK(store.remove(*first).error().code == ErrorCode::render_mesh_unknown);
    CHECK(store.update(*first, checker(2)).error().code == ErrorCode::render_mesh_unknown);
    CHECK(store.update_material(*first, {}).error().code == ErrorCode::render_mesh_unknown);
    REQUIRE(store.remove_material(material).has_value());
    CHECK(store.material_count() == 0);
    auto next = store.add(checker(2));
    REQUIRE(next.has_value());
    CHECK(*next != *first);  // ids are never reused
}

TEST_CASE("[rtmesh.AC3] an entity with a RuntimeMeshInstance is extracted as a draw with the mesh's materials") {
    SceneFixture f;
    const AssetId grass = f.meshes.add_material({.name = "grass"});
    render::MeshData data = checker(4);
    data.materials = {grass};
    const AssetId mesh = *f.meshes.add(std::move(data));
    const Entity patch = f.world.entity("patch")
                             .set(Transform{{1.0f, 2.0f, 3.0f}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)})
                             .set(RuntimeMeshInstance{mesh});
    propagate_transforms(f.world);
    f.renderer.extract(f.world);
    const render::RenderWorld& rw = f.renderer.render_world();
    REQUIRE(rw.meshes.size() == 1);
    CHECK(rw.meshes[0].mesh == mesh);
    CHECK(rw.meshes[0].entity == patch.id());
    REQUIRE(rw.meshes[0].materials.size() == 1);
    CHECK(rw.meshes[0].materials[0] == grass);
    CHECK(math::vec3(rw.meshes[0].world[3]) == math::vec3(1.0f, 2.0f, 3.0f));
    REQUIRE(rw.runtime_assets != nullptr);
    CHECK(rw.runtime_assets->meshes.contains(mesh));

    // A removed mesh draws nothing; the entity keeps its component.
    REQUIRE(f.meshes.remove(mesh).has_value());
    f.renderer.extract(f.world);
    CHECK(f.renderer.render_world().meshes.empty());
    CHECK(patch.has<RuntimeMeshInstance>());
}

TEST_CASE("[rtmesh.AC5] GPU data is uploaded once per change and freed on remove, without leaks") {
    SceneFixture f;
    const AssetId mesh = *f.meshes.add(checker(8));
    f.world.entity("patch")
        .set(Transform{{0.0f, 0.0f, 0.0f}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)})
        .set(RuntimeMeshInstance{mesh});
    CHECK(runtime_resources(*f.device) == 0);  // nothing until a frame draws it
    f.frame();
    const std::size_t per_mesh = runtime_resources(*f.device);
    CHECK(per_mesh == 6);  // vertices, indices, slots, meshlets, meshlet vertices, meshlet triangles (no BLAS on null)
    const auto count_named = [&](std::string_view name) {
        const rhi::MemoryUsage usage = f.device->memory_usage();
        return std::ranges::count_if(usage.resources, [&](const rhi::ResourceMemory& r) { return r.name == name; });
    };
    const auto vertex_bytes = [&] {
        const rhi::MemoryUsage usage = f.device->memory_usage();
        const auto it =
            std::ranges::find(usage.resources, std::string("runtime mesh vertices"), &rhi::ResourceMemory::name);
        return it == usage.resources.end() ? std::uint64_t{0} : it->bytes;
    };
    CHECK(vertex_bytes() == 81 * sizeof(MeshVertex));

    // Quiet frames upload nothing.
    for (int i = 0; i < 3; ++i) {
        f.frame();
    }
    CHECK(runtime_resources(*f.device) == per_mesh);

    // Each update replaces the buffers once (the old ones are retired) and keeps drawing.
    for (std::uint32_t n = 9; n < 12; ++n) {
        REQUIRE(f.meshes.update(mesh, checker(n, 0.5f)).has_value());
        f.frame();
        CHECK(runtime_resources(*f.device) == per_mesh);
        CHECK(count_named("runtime mesh vertices") == 1);
        CHECK(vertex_bytes() == std::uint64_t{n + 1} * (n + 1) * sizeof(MeshVertex));
    }

    // A second entity drawing the same mesh shares its buffers.
    f.world.entity("copy")
        .set(Transform{{9.0f, 0.0f, 0.0f}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)})
        .set(RuntimeMeshInstance{mesh});
    f.frame();
    CHECK(runtime_resources(*f.device) == per_mesh);

    // Removing frees everything, even though entities still point at the mesh.
    REQUIRE(f.meshes.remove(mesh).has_value());
    f.frame();
    CHECK(runtime_resources(*f.device) == 0);

    // A mesh nobody draws any more is freed too, and frames without a camera still sweep.
    const AssetId later = *f.meshes.add(checker(2));
    f.world.entity("later")
        .set(Transform{{0.0f, 0.0f, 0.0f}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)})
        .set(RuntimeMeshInstance{later});
    f.frame();
    CHECK(runtime_resources(*f.device) == per_mesh);
    f.world.lookup("camera").destruct();
    REQUIRE(f.meshes.remove(later).has_value());
    f.frame();
    CHECK(runtime_resources(*f.device) == 0);
}
