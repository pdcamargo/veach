#pragma once

// Scenes imported from glTF 2.0 and cooked into runtime formats (ADR 0011).
// Loading a .gltf/.glb gives a Handle<SceneAsset>. Its meshes, materials and
// textures are sub-assets with derived ids (AssetId::derive(scene id,
// "mesh/3")), loaded with it and reachable through their own typed handles.

#include <veach/asset/handle.hpp>
#include <veach/asset/loader.hpp>
#include <veach/asset/texture.hpp>
#include <veach/core/math.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace veach {

/// The one vertex layout every renderer reads: 48 bytes.
struct MeshVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    std::array<float, 4> tangent{1.0f, 0.0f, 0.0f, 1.0f};  ///< xyz, w = bitangent sign
    std::array<float, 2> uv{};
};
static_assert(sizeof(MeshVertex) == 48);

inline constexpr std::uint32_t meshlet_max_vertices = 64;
inline constexpr std::uint32_t meshlet_max_triangles = 124;
inline constexpr std::uint32_t meshlet_no_parent = 0xFFFFFFFFu;

/// A cluster of at most 64 vertices and 124 triangles from one material slot (ADR 0011). `lod`, `parent` and the
/// bounds' `error` are reserved for a cluster hierarchy; today every meshlet is level 0 and a root.
struct Meshlet {
    std::uint32_t vertex_offset = 0;    ///< into MeshAsset::meshlet_vertices
    std::uint32_t triangle_offset = 0;  ///< into MeshAsset::meshlet_triangles
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
    std::uint32_t slot = 0;
    std::uint32_t lod = 0;
    std::uint32_t parent = meshlet_no_parent;
    std::uint32_t flags = 0;
};
static_assert(sizeof(Meshlet) == 32);

struct MeshletBounds {
    std::array<float, 4> sphere{};  ///< center xyz, radius (object space)
    std::array<float, 4> cone{};    ///< normal cone axis xyz, cutoff (cosine; >= 1 means no cone)
    float error = 0.0f;             ///< simplification error of this level; 0 at level 0
    std::array<float, 3> pad{};
};
static_assert(sizeof(MeshletBounds) == 48);

/// All primitives of one glTF mesh. Each triangle names its material slot (the primitive it came from).
struct MeshAsset {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;         ///< 3 per triangle (ray tracing builds its BLAS from these)
    std::vector<std::uint32_t> triangle_slots;  ///< 1 per triangle
    std::uint32_t slot_count = 0;
    math::vec3 bounds_min{0.0f};
    math::vec3 bounds_max{0.0f};
    // Meshlets (ADR 0040): every triangle exactly once, grouped by slot.
    std::vector<Meshlet> meshlets;
    std::vector<std::uint32_t> meshlet_vertices;   ///< a mesh vertex index per meshlet vertex
    std::vector<std::uint32_t> meshlet_triangles;  ///< per triangle: 3 meshlet-local vertex indices, a byte each
    std::vector<MeshletBounds> meshlet_bounds;     ///< one per meshlet
};

/// Splits `mesh`'s triangles into meshlets (meshoptimizer), slot by slot. Deterministic.
void build_meshlets(MeshAsset& mesh);

enum class AlphaMode : std::uint8_t { opaque, mask, blend };

/// glTF metallic-roughness with emissive strength (ADR 0035). `kind` picks the Slang material type.
struct MaterialAsset {
    std::string name;
    std::string kind = "StandardMaterial";  ///< from extras.veach_material; the renderer checks the name
    math::vec4 base_color{1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    math::vec3 emissive{0.0f};
    float emissive_strength = 1.0f;
    float normal_scale = 1.0f;
    AlphaMode alpha_mode = AlphaMode::opaque;
    float alpha_cutoff = 0.5f;
    bool double_sided = false;
    std::optional<AssetId> base_color_texture;          ///< bc7_srgb
    std::optional<AssetId> metallic_roughness_texture;  ///< bc7_unorm, G = roughness, B = metallic
    std::optional<AssetId> normal_texture;              ///< bc7_unorm, tangent space
    std::optional<AssetId> emissive_texture;            ///< bc7_srgb
    /// What a material kind of its own reads (MaterialRecord::custom0). A kind may also give the texture slots its
    /// own meaning (TerrainMaterial keeps a data table in normal_texture).
    math::vec4 custom{0.0f};
    /// 2D arrays for kinds that sample them (MaterialRecord::array_texture0 and 1; TerrainMaterial: its layers).
    std::array<std::optional<AssetId>, 2> array_textures;
    /// 1: the scene's sampler. 2 to 16: its textures are filtered anisotropically (terrain: rhi::terrain_anisotropy).
    std::uint32_t max_anisotropy = 1;
};

enum class LightKind : std::uint8_t { directional, point, spot };

/// KHR_lights_punctual: lux for directional, candela for point and spot (ADR 0036).
struct SceneLight {
    std::string name;
    LightKind kind = LightKind::point;
    math::vec3 color{1.0f};
    float intensity = 1.0f;
    float inner_cone = 0.0f;
    float outer_cone = 0.7853982f;
};

struct SceneNode {
    std::string name;
    math::vec3 translation{0.0f};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    math::vec3 scale{1.0f};
    std::optional<std::uint32_t> mesh;     ///< index into SceneAsset::meshes
    std::vector<std::uint32_t> materials;  ///< per mesh slot: index into SceneAsset::materials
    std::optional<std::uint32_t> light;    ///< index into SceneAsset::lights
    std::vector<std::uint32_t> children;
};

struct SceneAsset {
    std::vector<SceneNode> nodes;
    std::vector<std::uint32_t> roots;
    std::vector<SceneLight> lights;
    std::vector<AssetId> meshes;  ///< Handle<MeshAsset> ids
    std::vector<AssetId> materials;
    std::vector<AssetId> textures;
    std::vector<SubAsset> sub_assets;
    bool from_cache = false;  ///< the cooked data came from .veach/cache
};

/// Imports .gltf and .glb, cooking through the cache in `<project>/.veach/cache` (ADR 0011).
class GltfLoader final : public TypedAssetLoader<SceneAsset> {
public:
    [[nodiscard]] std::string_view name() const override { return "gltf"; }
    [[nodiscard]] std::span<const std::string_view> extensions() const override { return extensions_; }
    [[nodiscard]] Result<SceneAsset> load(const LoadContext& ctx) const override;
    [[nodiscard]] std::span<const SubAsset> sub_assets(const void* data) const override {
        return static_cast<const SceneAsset*>(data)->sub_assets;
    }

private:
    static constexpr std::array<std::string_view, 2> extensions_{".gltf", ".glb"};
};

}  // namespace veach
