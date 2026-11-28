#include "runtime_mesh_demo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace lab {

namespace {

/// The relief and its slope (dh/dx, dh/dz), meters.
struct Height {
    float h = 0.0f;
    float dx = 0.0f;
    float dz = 0.0f;
};

Height relief(float x, float z, float amplitude) {
    const float a = std::sin(0.9f * x) * std::cos(0.7f * z);
    const float b = std::sin(1.7f * x + 2.3f * z);
    const float c = std::cos(3.1f * x - 1.3f * z);
    Height out;
    out.h = amplitude * (0.6f * a + 0.3f * b + 0.1f * c);
    out.dx = amplitude * (0.6f * 0.9f * std::cos(0.9f * x) * std::cos(0.7f * z) +
                          0.3f * 1.7f * std::cos(1.7f * x + 2.3f * z) - 0.1f * 3.1f * std::sin(3.1f * x - 1.3f * z));
    out.dz = amplitude * (-0.6f * 0.7f * std::sin(0.9f * x) * std::sin(0.7f * z) +
                          0.3f * 2.3f * std::cos(1.7f * x + 2.3f * z) + 0.1f * 1.3f * std::sin(3.1f * x - 1.3f * z));
    return out;
}

/// The materials, made once per run.
struct DemoMaterials {
    veach::AssetId grass;
    veach::AssetId rock;
};

veach::render::MeshStore* mesh_store(veach::World world) {
    veach::Services* registry = veach::find_services(world);
    return registry ? registry->find<veach::render::MeshStore>() : nullptr;
}

}  // namespace

veach::render::MeshData runtime_mesh_demo_data(const RuntimeMeshDemo& demo, veach::AssetId grass, veach::AssetId rock) {
    const std::uint32_t n = std::clamp<std::uint32_t>(demo.cells, 1, 512);
    veach::render::MeshData data;
    data.materials = {grass, rock};
    data.vertices.reserve(std::size_t{n + 1} * (n + 1));
    std::vector<float> heights;
    for (std::uint32_t j = 0; j <= n; ++j) {
        for (std::uint32_t i = 0; i <= n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n);
            const float v = static_cast<float>(j) / static_cast<float>(n);
            const float x = (u - 0.5f) * demo.size;
            const float z = (v - 0.5f) * demo.size;
            const Height h = relief(x, z, demo.height);
            const veach::math::vec3 normal = veach::math::normalize(veach::math::vec3(-h.dx, 1.0f, -h.dz));
            const veach::math::vec3 tangent = veach::math::normalize(veach::math::vec3(1.0f, h.dx, 0.0f));
            veach::MeshVertex vertex;
            vertex.position = {x, h.h, z};
            vertex.normal = {normal.x, normal.y, normal.z};
            vertex.tangent = {tangent.x, tangent.y, tangent.z, 1.0f};
            vertex.uv = {u, v};
            data.vertices.push_back(vertex);
            heights.push_back(h.h);
        }
    }
    // Counter-clockwise seen from above (+Y), as glTF faces are.
    for (std::uint32_t j = 0; j < n; ++j) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t a = j * (n + 1) + i;
            const std::array<std::array<std::uint32_t, 3>, 2> triangles{
                {{a, a + n + 1, a + 1}, {a + 1, a + n + 1, a + n + 2}}};
            for (const auto& t : triangles) {
                data.indices.insert(data.indices.end(), t.begin(), t.end());
                const float mean = (heights[t[0]] + heights[t[1]] + heights[t[2]]) / 3.0f;
                data.triangle_slots.push_back(mean > demo.rock_above ? 1u : 0u);  // interleaved: the store groups them
            }
        }
    }
    return data;
}

void register_runtime_mesh_demo(veach::World& world) {
    world.component<RuntimeMeshDemo>()
        .member(&RuntimeMeshDemo::size, "size")
        .member(&RuntimeMeshDemo::cells, "cells")
        .member(&RuntimeMeshDemo::height, "height")
        .member(&RuntimeMeshDemo::rock_above, "rock_above")
        .add<veach::Scriptable>();

    auto materials = std::make_shared<DemoMaterials>();
    world.observer<const RuntimeMeshDemo>("MeshRuntimeMeshDemo")
        .event(veach::OnSet)
        .each([materials](veach::Entity e, const RuntimeMeshDemo& demo) {
            veach::render::MeshStore* meshes = mesh_store(e.world());
            if (meshes == nullptr) {
                return;
            }
            if (!materials->grass.is_valid()) {
                materials->grass = meshes->add_material(
                    {.name = "grass", .base_color = {0.16f, 0.32f, 0.09f, 1.0f}, .metallic = 0.0f, .roughness = 0.9f});
                materials->rock = meshes->add_material(
                    {.name = "rock", .base_color = {0.40f, 0.36f, 0.32f, 1.0f}, .metallic = 0.0f, .roughness = 0.7f});
            }
            veach::render::MeshData data = runtime_mesh_demo_data(demo, materials->grass, materials->rock);
            // A change re-meshes in place: the entity keeps its mesh id, the renderer re-uploads it once.
            if (const auto* instance = e.try_get<veach::RuntimeMeshInstance>();
                instance != nullptr && meshes->find(instance->mesh) != nullptr) {
                if (auto updated = meshes->update(instance->mesh, std::move(data)); !updated) {
                    veach::log::error("lab", updated.error());
                }
                return;
            }
            auto added = meshes->add(std::move(data));
            if (!added) {
                veach::log::error("lab", added.error());
                return;
            }
            e.set(veach::RuntimeMeshInstance{*added});
        });
    world.observer<const RuntimeMeshDemo>("FreeRuntimeMeshDemo")
        .event(veach::OnRemove)
        .each([](veach::Entity e, const RuntimeMeshDemo&) {
            veach::render::MeshStore* meshes = mesh_store(e.world());
            const auto* instance = e.try_get<veach::RuntimeMeshInstance>();
            if (meshes != nullptr && instance != nullptr && meshes->find(instance->mesh) != nullptr) {
                (void)meshes->remove(instance->mesh);
            }
        });
}

}  // namespace lab
