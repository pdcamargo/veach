#pragma once

// Scenes imported from glTF 2.0 and cooked into runtime formats (ADR 0032).
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

/// All primitives of one glTF mesh. Each triangle names its material slot (the primitive it came from).
struct MeshAsset {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;         ///< 3 per triangle
    std::vector<std::uint32_t> triangle_slots;  ///< 1 per triangle
    std::uint32_t slot_count = 0;
    math::vec3 bounds_min{0.0f};
    math::vec3 bounds_max{0.0f};
};

enum class AlphaMode : std::uint8_t { opaque, mask, blend };

/// glTF metallic-roughness with emissive strength (ADR 0033). `kind` picks the Slang material type.
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
};

enum class LightKind : std::uint8_t { directional, point, spot };

/// KHR_lights_punctual: lux for directional, candela for point and spot (ADR 0034).
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

/// Imports .gltf and .glb, cooking through the cache in `<project>/.veach/cache` (ADR 0032).
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
