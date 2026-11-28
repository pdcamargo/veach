// A body's terrain surface (terrain_surface.hpp): slot rules to per-vertex weights, and the TerrainMaterial record.

#include <veach/runtime/terrain_surface.hpp>

#include <veach/render/mesh_store.hpp>
#include <veach/rhi/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace veach {

namespace {

/// 0 at e0, 1 at e1, smooth in between (canonical: + - * / only).
double ramp(double x, double e0, double e1) {
    if (e1 <= e0) {
        return x >= e1 ? 1.0 : 0.0;
    }
    if (x <= e0) {
        return 0.0;
    }
    if (x >= e1) {
        return 1.0;
    }
    const double t = (x - e0) / (e1 - e0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

double TerrainWindow::at(double x) const {
    return ramp(x, lo0, lo1) * (1.0 - ramp(x, hi0, hi1));
}

Result<std::shared_ptr<const TerrainSurface>> TerrainSurface::make(std::shared_ptr<const TerrainLayerLibrary> library,
                                                                   TerrainSurfaceDesc desc) {
    if (library == nullptr || desc.slots.empty() || desc.slots.size() > terrain_surface_slots) {
        return make_error(ErrorCode::render_mesh_invalid,
                          std::format("terrain surface '{}' has {} slots{}", desc.name, desc.slots.size(),
                                      library == nullptr ? " and no layer library" : ""),
                          std::format("Give a library and 1 to {} slots: a chunk vertex carries {} weights.",
                                      terrain_surface_slots, terrain_surface_slots));
    }
    for (const TerrainSlot& s : desc.slots) {
        if (s.layer >= library->layers().size()) {
            return make_error(ErrorCode::render_mesh_invalid,
                              std::format("terrain surface '{}' uses layer {}; the library has {}", desc.name, s.layer,
                                          library->layers().size()),
                              "Find layers by name with TerrainLayerLibrary::find.");
        }
    }
    return std::make_shared<const TerrainSurface>(std::move(library), std::move(desc));
}

TerrainSurface::TerrainSurface(std::shared_ptr<const TerrainLayerLibrary> library, TerrainSurfaceDesc desc)
    : library_(std::move(library)), desc_(std::move(desc)) {}

void TerrainSurface::weights(std::span<const TerrainVertexInput> vertices, std::span<std::array<float, 6>> out) const {
    std::vector<std::array<float, 2>> fields(vertices.size(), std::array<float, 2>{0.0f, 0.0f});
    if (desc_.fields) {
        std::vector<math::dvec3> positions(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            positions[i] = vertices[i].position;
        }
        desc_.fields(positions, fields);
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const TerrainVertexInput& v = vertices[i];
        const math::dvec3& p = v.position;
        double altitude = 0.0;
        math::dvec3 up(0.0, 1.0, 0.0);
        if (desc_.up == TerrainUp::radial) {
            const double r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            altitude = r - desc_.datum_m;
            up = r > 0.0 ? p / r : up;
        } else {
            altitude = p.y - desc_.datum_m;
        }
        const math::dvec3 n(v.normal.x, v.normal.y, v.normal.z);
        const double n_length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        const double cos_up = n_length > 0.0 ? (n.x * up.x + n.y * up.y + n.z * up.z) / n_length : 1.0;
        const double slope = 1.0 - cos_up;
        std::array<double, 6> w{};
        double total = 0.0;
        for (std::size_t s = 0; s < desc_.slots.size(); ++s) {
            for (const TerrainRule& rule : desc_.slots[s].rules) {
                w[s] = std::max(w[s], rule.strength * rule.altitude.at(altitude) * rule.slope.at(slope) *
                                          rule.depth.at(v.depth) * rule.field0.at(fields[i][0]) *
                                          rule.field1.at(fields[i][1]));
            }
            total += w[s];
        }
        for (std::size_t s = 0; s < 6; ++s) {
            out[i][s] = total > 0.0 ? static_cast<float>(w[s] / total) : 0.0f;
        }
    }
}

TextureAsset TerrainSurface::table() const {
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t rows = terrain_surface_slots + 1;
    std::vector<math::vec4> texels(std::size_t{width} * rows, math::vec4(0.0f));
    const auto at = [&](std::uint32_t row, std::uint32_t column) -> math::vec4& {
        return texels[std::size_t{row} * width + column];
    };
    for (std::uint32_t s = 0; s < desc_.slots.size(); ++s) {
        const TerrainSlot& slot = desc_.slots[s];
        const TerrainLayerInfo& info = library_->layers()[slot.layer];
        const float near_tile = info.tile_m;
        const float far_tile = terrain_tile(info.tile_m * 2.7f);  // rarely a whole multiple of the near tile
        const math::vec3 mean = slot.use_palette ? slot.palette : info.mean_color;
        at(s, 0) = math::vec4(near_tile, far_tile, static_cast<float>(slot.layer), slot.use_palette ? 1.0f : 0.0f);
        at(s, 1) = math::vec4(mean, info.mean_luminance);
        at(s, 2) = math::vec4(slot.palette, slot.palette_contrast);
        at(s, 3) = math::vec4(info.mean_roughness * slot.roughness_scale, slot.roughness_scale, info.mean_height,
                              slot.normal_strength);
    }
    at(terrain_surface_slots, 0) = math::vec4(desc_.fade_start, desc_.fade_end, desc_.ao_strength, desc_.normal_scale);
    TextureAsset t;
    t.format = TextureFormat::rgba32_float;
    t.width = width;
    t.height = rows;
    t.mips.emplace_back(texels.size() * sizeof(math::vec4));
    std::memcpy(t.mips[0].data(), texels.data(), t.mips[0].size());
    return t;
}

Result<AssetId> TerrainSurface::material(render::MeshStore& store) const {
    const std::scoped_lock lock(mutex_);
    if (store_ == &store) {
        return material_;
    }
    auto arrays = library_->textures(store);
    if (!arrays) {
        return std::unexpected(arrays.error());
    }
    auto table_id = store.add_texture(table());
    if (!table_id) {
        return std::unexpected(table_id.error());
    }
    MaterialAsset m;
    m.name = desc_.name.empty() ? "terrain" : desc_.name;
    m.kind = "TerrainMaterial";
    m.metallic = 0.0f;
    m.roughness = 0.9f;
    m.normal_texture = *table_id;
    m.array_textures = {(*arrays)[0], (*arrays)[1]};
    m.custom = math::vec4(static_cast<float>(desc_.slots.size()), desc_.macro_strength, desc_.height_blend,
                          desc_.triplanar_sharpness);
    m.max_anisotropy = rhi::terrain_anisotropy;  // ADR 0090
    material_ = store.add_material(std::move(m));
    store_ = &store;
    return material_;
}

}  // namespace veach
