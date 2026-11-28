#include <veach/render/mesh_store.hpp>

#include <veach/core/profile.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace veach::render {

namespace {

constexpr std::uint64_t runtime_id_hi = 0x5645414348000000ULL;

std::unexpected<Error> invalid(std::string message, std::string hint) {
    return make_error(ErrorCode::render_mesh_invalid, std::move(message), std::move(hint));
}

Result<> unknown(const char* what, AssetId id) {
    return make_error(ErrorCode::render_mesh_unknown,
                      std::format("no runtime {} {} in the MeshStore", what, id.to_string()),
                      "Use the id that MeshStore::add() returned, and only until remove(); ids are never reused.");
}

/// Reorders the triangles so each slot's are one run, keeping their order within a slot (a counting sort).
void group_by_slot(MeshAsset& mesh) {
    if (std::ranges::is_sorted(mesh.triangle_slots)) {
        return;
    }
    std::vector<std::size_t> start(mesh.slot_count + 1, 0);
    for (const std::uint32_t slot : mesh.triangle_slots) {
        ++start[slot + 1];
    }
    for (std::size_t s = 1; s < start.size(); ++s) {
        start[s] += start[s - 1];
    }
    std::vector<std::uint32_t> indices(mesh.indices.size());
    std::vector<std::uint32_t> slots(mesh.triangle_slots.size());
    for (std::size_t t = 0; t < mesh.triangle_slots.size(); ++t) {
        const std::uint32_t slot = mesh.triangle_slots[t];
        const std::size_t to = start[slot]++;
        slots[to] = slot;
        std::copy_n(mesh.indices.begin() + static_cast<std::ptrdiff_t>(t * 3), 3,
                    indices.begin() + static_cast<std::ptrdiff_t>(to * 3));
    }
    mesh.indices = std::move(indices);
    mesh.triangle_slots = std::move(slots);
}

}  // namespace

Result<RuntimeMesh> build_runtime_mesh(MeshData data) {
    VEACH_ZONE("build_runtime_mesh");
    const std::size_t triangles = data.indices.size() / 3;
    if (data.vertices.empty() || data.indices.empty() || data.indices.size() % 3 != 0) {
        return invalid(std::format("a runtime mesh needs vertices and 3 indices per triangle; got {} vertices and "
                                   "{} indices",
                                   data.vertices.size(), data.indices.size()),
                       "Submit whole triangles. To draw nothing, remove the mesh (or the RuntimeMeshInstance).");
    }
    for (std::size_t i = 0; i < data.indices.size(); ++i) {
        if (data.indices[i] >= data.vertices.size()) {
            return invalid(
                std::format("index {} is {}, past the last of {} vertices", i, data.indices[i], data.vertices.size()),
                "Indices count from 0 into MeshData::vertices.");
        }
    }
    for (std::size_t v = 0; v < data.vertices.size(); ++v) {
        const auto& p = data.vertices[v].position;
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
            return invalid(std::format("vertex {} has a NaN or infinite position", v),
                           "Check the mesher's inputs (a density gradient of zero?).");
        }
    }
    if (!data.triangle_slots.empty() && data.triangle_slots.size() != triangles) {
        return invalid(std::format("{} triangle slots for {} triangles", data.triangle_slots.size(), triangles),
                       "Give one slot per triangle, or none for a single-material mesh.");
    }
    std::size_t slot_count = std::max<std::size_t>(1, data.materials.size());
    for (const std::uint32_t slot : data.triangle_slots) {
        slot_count = std::max<std::size_t>(slot_count, std::size_t{slot} + 1);
    }
    if (slot_count > runtime_mesh_max_slots) {
        return invalid(
            std::format("{} material slots; a runtime mesh has at most {}", slot_count, runtime_mesh_max_slots),
            "Split the mesh, or map its materials onto fewer slots.");
    }

    RuntimeMesh out;
    MeshAsset& mesh = out.mesh;
    mesh.vertices = std::move(data.vertices);
    mesh.indices = std::move(data.indices);
    mesh.triangle_slots = std::move(data.triangle_slots);
    if (mesh.triangle_slots.empty()) {
        mesh.triangle_slots.assign(triangles, 0);
    }
    mesh.slot_count = static_cast<std::uint32_t>(slot_count);
    group_by_slot(mesh);
    mesh.bounds_min = math::vec3(std::numeric_limits<float>::max());
    mesh.bounds_max = math::vec3(std::numeric_limits<float>::lowest());
    for (const MeshVertex& v : mesh.vertices) {
        const math::vec3 p(v.position[0], v.position[1], v.position[2]);
        mesh.bounds_min = math::min(mesh.bounds_min, p);
        mesh.bounds_max = math::max(mesh.bounds_max, p);
    }
    build_meshlets(mesh);
    out.materials = std::move(data.materials);
    return out;
}

AssetId MeshStore::next_id() {
    return AssetId{runtime_id_hi, next_++};
}

Result<AssetId> MeshStore::add(MeshData data) {
    auto built = build_runtime_mesh(std::move(data));  // outside the lock: meshlets take a while
    if (!built) {
        return std::unexpected(built.error());
    }
    auto mesh = std::make_shared<const RuntimeMesh>(std::move(*built));
    const std::scoped_lock lock(mutex_);
    const AssetId id = next_id();
    assets_.meshes.emplace(id, std::move(mesh));
    snapshot_.reset();
    return id;
}

AssetId MeshStore::add(std::shared_ptr<const RuntimeMesh> built) {
    const std::scoped_lock lock(mutex_);
    const AssetId id = next_id();
    assets_.meshes.emplace(id, std::move(built));
    snapshot_.reset();
    return id;
}

Result<> MeshStore::update(AssetId mesh, MeshData data) {
    {
        const std::scoped_lock lock(mutex_);
        if (!assets_.meshes.contains(mesh)) {
            return unknown("mesh", mesh);
        }
    }
    auto built = build_runtime_mesh(std::move(data));
    if (!built) {
        return std::unexpected(built.error());
    }
    auto shared = std::make_shared<const RuntimeMesh>(std::move(*built));
    const std::scoped_lock lock(mutex_);
    const auto it = assets_.meshes.find(mesh);
    if (it == assets_.meshes.end()) {
        return unknown("mesh", mesh);  // removed while this update was building
    }
    it->second = std::move(shared);
    snapshot_.reset();
    return {};
}

Result<> MeshStore::remove(AssetId mesh) {
    const std::scoped_lock lock(mutex_);
    if (assets_.meshes.erase(mesh) == 0) {
        return unknown("mesh", mesh);
    }
    snapshot_.reset();
    return {};
}

std::shared_ptr<const RuntimeMesh> MeshStore::find(AssetId mesh) const {
    const std::scoped_lock lock(mutex_);
    const auto it = assets_.meshes.find(mesh);
    return it == assets_.meshes.end() ? nullptr : it->second;
}

AssetId MeshStore::add_material(MaterialAsset material) {
    auto shared = std::make_shared<const MaterialAsset>(std::move(material));
    const std::scoped_lock lock(mutex_);
    const AssetId id = next_id();
    assets_.materials.emplace(id, std::move(shared));
    snapshot_.reset();
    return id;
}

Result<> MeshStore::update_material(AssetId material, MaterialAsset data) {
    auto shared = std::make_shared<const MaterialAsset>(std::move(data));
    const std::scoped_lock lock(mutex_);
    const auto it = assets_.materials.find(material);
    if (it == assets_.materials.end()) {
        return unknown("material", material);
    }
    it->second = std::move(shared);
    snapshot_.reset();
    return {};
}

Result<> MeshStore::remove_material(AssetId material) {
    const std::scoped_lock lock(mutex_);
    if (assets_.materials.erase(material) == 0) {
        return unknown("material", material);
    }
    snapshot_.reset();
    return {};
}

std::size_t MeshStore::mesh_count() const {
    const std::scoped_lock lock(mutex_);
    return assets_.meshes.size();
}

std::size_t MeshStore::material_count() const {
    const std::scoped_lock lock(mutex_);
    return assets_.materials.size();
}

std::shared_ptr<const RuntimeAssets> MeshStore::snapshot() const {
    const std::scoped_lock lock(mutex_);
    if (!snapshot_) {
        snapshot_ = std::make_shared<const RuntimeAssets>(assets_);  // copies pointers, not mesh data
    }
    return snapshot_;
}

}  // namespace veach::render
