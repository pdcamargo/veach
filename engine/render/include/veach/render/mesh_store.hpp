#pragma once

// Meshes and materials made at run time (docs/specs/runtime-meshes/spec.md, ADR 0072): terrain chunks and other
// generated geometry, drawn like cooked glTF meshes but never cooked or saved. A module submits CPU data; the store
// validates it, builds meshlets (as the cooker does, ADR 0011) and keeps it as immutable snapshots; extraction hands
// the current snapshot to the renderer, which uploads a mesh only when it changed and frees its GPU buffers when it
// is removed (retired through the frame model, ADR 0030). Gameplay never sees a GPU handle (ADR 0020).
//
//   auto& meshes = veach::services(world).get<veach::render::MeshStore>();
//   const veach::AssetId rock = meshes.add_material({.name = "rock", .base_color = {0.4f, 0.35f, 0.3f, 1.0f}});
//   veach::Result<veach::AssetId> chunk = meshes.add({.vertices = v, .indices = i, .materials = {rock}});
//   entity.set(veach::RuntimeMeshInstance{*chunk});
//   meshes.update(*chunk, {...});  // a terrain edit: re-uploaded once, the old buffers retired
//   meshes.remove(*chunk);         // its GPU buffers are freed

#include <veach/asset/scene.hpp>
#include <veach/asset/texture.hpp>
#include <veach/core/error.hpp>
#include <veach/core/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace veach::render {

/// What a module submits: the cooked layout's vertices (48-byte MeshVertex), indices and per-triangle slots.
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;         ///< 3 per triangle, each < vertices.size()
    std::vector<std::uint32_t> triangle_slots;  ///< 1 per triangle; empty = every triangle in slot 0
    std::vector<AssetId> materials;  ///< per slot: a MeshStore or glTF material; missing slots draw the default
};

/// A submitted mesh as the renderer reads it. Immutable: an update makes a new one.
struct RuntimeMesh {
    MeshAsset mesh;  ///< triangles grouped by slot (stable), bounds, meshlets
    std::vector<AssetId> materials;
};

/// Everything in the store at one moment. Extraction copies the pointer into RenderWorld::runtime_assets.
struct RuntimeAssets {
    std::unordered_map<AssetId, std::shared_ptr<const RuntimeMesh>> meshes;
    std::unordered_map<AssetId, std::shared_ptr<const MaterialAsset>> materials;
    std::unordered_map<AssetId, std::shared_ptr<const TextureAsset>> textures;
};

/// The most material slots a runtime mesh may use.
inline constexpr std::uint32_t runtime_mesh_max_slots = 256;

/// True for ids minted by a MeshStore (they never collide with asset ids: their UUID version is 0).
[[nodiscard]] constexpr bool is_runtime_id(const AssetId& id) {
    return id.hi == 0x5645414348000000ULL && id.lo != 0;  // "VEACH" + version 0
}

/// Validates `data` and builds what the renderer needs: triangles stably grouped by slot, bounds and meshlets.
/// Pure and thread-safe, so a mesher may call it on a worker. Errors are render.mesh_invalid.
[[nodiscard]] Result<RuntimeMesh> build_runtime_mesh(MeshData data);

/// The renderer's store of run-time meshes and materials. Owned by the Renderer, a service of the engine. Every
/// method is thread-safe. Ids are deterministic: the n-th mesh or material made in a run has the same id every run.
class MeshStore {
public:
    /// Builds the mesh (build_runtime_mesh) and stores it under a new id.
    [[nodiscard]] Result<AssetId> add(MeshData data);
    /// Stores a mesh already built by build_runtime_mesh (on a worker: terrain chunks build theirs on the chunk's
    /// job, then add them on the main thread in a fixed order, so ids stay deterministic).
    [[nodiscard]] AssetId add(std::shared_ptr<const RuntimeMesh> built);
    /// Replaces a mesh. Entities drawing it draw the new data next frame; the old GPU buffers are retired.
    [[nodiscard]] Result<> update(AssetId mesh, MeshData data);
    /// Forgets a mesh; the renderer frees its GPU data. Entities still pointing at it draw nothing.
    [[nodiscard]] Result<> remove(AssetId mesh);
    /// The mesh as submitted and built (for colliders and tools), or null.
    [[nodiscard]] std::shared_ptr<const RuntimeMesh> find(AssetId mesh) const;

    [[nodiscard]] AssetId add_material(MaterialAsset material);
    [[nodiscard]] Result<> update_material(AssetId material, MaterialAsset data);
    [[nodiscard]] Result<> remove_material(AssetId material);

    /// A texture made at run time (2D or a 2D array; terrain layers), for materials to reference by id. Uploaded
    /// once, when a drawn material first uses it; freed when removed. Errors are render.mesh_invalid.
    [[nodiscard]] Result<AssetId> add_texture(TextureAsset texture);
    [[nodiscard]] Result<> remove_texture(AssetId texture);

    [[nodiscard]] std::size_t mesh_count() const;
    [[nodiscard]] std::size_t material_count() const;
    /// The current contents. The same pointer until something changes, so extraction is O(1) on quiet frames.
    [[nodiscard]] std::shared_ptr<const RuntimeAssets> snapshot() const;

private:
    [[nodiscard]] AssetId next_id();

    mutable std::mutex mutex_;
    RuntimeAssets assets_;
    mutable std::shared_ptr<const RuntimeAssets> snapshot_;  ///< null when assets_ changed since
    std::uint64_t next_ = 1;
};

}  // namespace veach::render
