#pragma once

// Terrain materials (docs/specs/terrain-materials/spec.md, ADR 0098; guide: docs/guides/terrain-materials.md).
//
// A TerrainLayerLibrary is a game's set of surface layers (grass, rock, sand, snow...): each a scanned PBR set from a
// folder (Quixel Megascans and the like: color, normal, roughness, AO, displacement) cooked once into a cache, or,
// when the folder is absent, a small procedural layer made from its fallback description. Every layer goes into two
// 2D texture arrays (albedo and height; normal, roughness and AO), BC7 with mips.
//
// A TerrainSurface is one body's look: up to six slots, each a library layer with a placement rule (slope, altitude,
// depth below the generated surface, and two climate fields the game supplies) and a color mode (the scan's color,
// or the opt-in palette color with the scan's detail). TerrainModule gives every chunk vertex the six slot weights
// (on the chunk's job: canonical, + - * / sqrt only) and draws the chunks with the surface's TerrainMaterial, which
// blends the layers per pixel by height, triplanar, at two scales, fading to each layer's mean color with distance.
//
//   auto library = veach::load_terrain_library(veach::read_terrain_library_config(json, dir).value(), jobs);
//   veach::TerrainSurfaceDesc desc{.up = veach::TerrainUp::radial, .datum_m = radius};
//   desc.slots.push_back({.layer = *library->find("grass"), .rules = {{.slope = {-1, -1, 0.25f, 0.4f}}}});
//   auto surface = std::make_shared<veach::TerrainSurface>(library, desc);
//   terrain.add(frame, body, {.materials = {surface->material(store)}, .surface = surface});

#include <veach/asset/texture.hpp>
#include <veach/core/error.hpp>
#include <veach/core/ids.hpp>
#include <veach/core/math.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach {

class JobSystem;
namespace render {
class MeshStore;
}
namespace voxel {
struct ChunkRecord;
class TerrainBody;
}  // namespace voxel

/// The most slots a TerrainSurface has: a chunk vertex carries six weights (its tangent and uv).
inline constexpr std::uint32_t terrain_surface_slots = 6;
/// SurfaceInput::material_origin's period for terrain chunks: their origin in the body frame modulo this.
inline constexpr double terrain_material_period_m = 65536.0;

// ---- the layer library ----------------------------------------------------------------------------------------------

/// What a layer looks like without its scan: a procedural stand-in (CI, and anyone without the scans).
struct TerrainFallback {
    math::vec3 color{0.4f, 0.38f, 0.35f};  ///< sRGB
    float roughness = 0.85f;
    float relief = 0.5f;   ///< 0 flat to 1 rough: the bump's depth
    float grain_m = 0.25;  ///< the size of its features (stones, clods), meters
};

/// One layer of a library.
struct TerrainLayerSource {
    std::string name;  ///< what surfaces call it ("grass", "rock")
    /// A folder with a scanned set: files whose names end in _BaseColor/_Albedo, _Normal, _Roughness, _AO and
    /// _Displacement (or _Bump), JPEG or PNG, and optionally a Megascans JSON (its scanArea gives the tile). Empty or
    /// missing: the procedural fallback.
    std::filesystem::path folder;
    float tile_m = 0.0f;  ///< the near tile; 0: the scan's area, else 2 m. Rounded to 16 / n m (terrain_tile)
    TerrainFallback fallback;
};

struct TerrainLibrarySettings {
    std::uint32_t size = 1024;  ///< scanned layers' resolution (power of two); procedural ones are 256 when all are
    /// Cooked layers are kept here (KTX2, keyed by the source files' sizes and times and the settings); empty: no
    /// cache, cook every run.
    std::filesystem::path cache;
};

/// A layer's averages, for distance fading and orbit views (the far terrain shows them, not the textures).
struct TerrainLayerInfo {
    std::string name;
    bool scanned = false;  ///< false: the procedural fallback
    float tile_m = 2.0f;
    math::vec3 mean_color{0.5f};  ///< linear
    float mean_luminance = 0.5f;
    float mean_roughness = 0.8f;
    float mean_height = 0.5f;
};

/// The cooked layers: two 2D arrays, a layer each. Immutable once loaded; shared by every surface that uses it.
class TerrainLayerLibrary {
public:
    TerrainLayerLibrary(std::vector<TerrainLayerInfo> layers, TextureAsset albedo_height, TextureAsset detail);

    [[nodiscard]] std::span<const TerrainLayerInfo> layers() const { return layers_; }
    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view name) const;
    [[nodiscard]] const TextureAsset& albedo_height() const { return albedo_height_; }
    [[nodiscard]] const TextureAsset& detail() const { return detail_; }
    /// True if any layer is a scan (the prototype's local assets were found).
    [[nodiscard]] bool scanned() const;
    /// The two arrays in `store`, added on first call. Thread-safe.
    [[nodiscard]] Result<std::array<AssetId, 2>> textures(render::MeshStore& store) const;

private:
    std::vector<TerrainLayerInfo> layers_;
    TextureAsset albedo_height_;  ///< bc7_srgb (or rgba8_srgb): albedo, height in alpha
    TextureAsset detail_;         ///< bc7_unorm (or rgba8_unorm): normal xy, roughness, AO
    mutable std::mutex mutex_;
    mutable render::MeshStore* store_ = nullptr;
    mutable std::array<AssetId, 2> ids_{};
};

/// Loads (from the cache) or cooks every layer, on `jobs`. A layer whose folder is missing or incomplete becomes its
/// procedural fallback (logged once at info level, never an error). Fails only for an empty list.
[[nodiscard]] Result<std::shared_ptr<const TerrainLayerLibrary>> load_terrain_library(
    std::span<const TerrainLayerSource> layers, const TerrainLibrarySettings& settings, JobSystem& jobs);

/// A library config (JSON): {"scans": dir, "cache": dir, "size": n, "layers": [{"name", "scan", "tile_m",
/// "fallback": {"color": [r, g, b], "roughness", "relief", "grain_m"}}]}. Relative paths are from `base`; a layer
/// without "fallback" takes the one of the layer of its name in `defaults`. Errors are data.parse_failed.
struct TerrainLibraryConfig {
    std::vector<TerrainLayerSource> layers;
    TerrainLibrarySettings settings;
};
[[nodiscard]] Result<TerrainLibraryConfig> read_terrain_library_config(
    std::string_view json, const std::filesystem::path& base, std::span<const TerrainLayerSource> defaults = {});

/// The tile a library uses for `tile_m`: 16 / n m for the nearest whole n (16, 8, 5.33, 4, 3.2 ... 0.25 m), so a
/// tile repeats exactly across chunk origins (multiples of 16 m).
[[nodiscard]] float terrain_tile(float tile_m);

/// A game's layer library as a service (services(world).provide<TerrainLayers>(...)): modules that make surfaces
/// (the space toolbox's UniverseModule) use it, or a procedural library of their own without it.
struct TerrainLayers {
    std::shared_ptr<const TerrainLayerLibrary> library;
};

// ---- a body's surface -----------------------------------------------------------------------------------------------

/// A smooth window over one input: 0 at or below lo0, rising to 1 at lo1, 1 to hi0, falling to 0 at hi1. The
/// defaults let everything through.
struct TerrainWindow {
    float lo0 = -std::numeric_limits<float>::max();
    float lo1 = -std::numeric_limits<float>::max();
    float hi0 = std::numeric_limits<float>::max();
    float hi1 = std::numeric_limits<float>::max();
    [[nodiscard]] double at(double x) const;
};

/// Where a slot shows: the product of its windows, times `strength`.
struct TerrainRule {
    TerrainWindow altitude;  ///< meters above the surface's datum
    TerrainWindow slope;     ///< 1 - cos(angle from up): 0 flat, 0.29 at 45 degrees, 1 vertical
    TerrainWindow depth;     ///< meters below the generated surface (the walls of a dug hole); 0 on untouched ground
    TerrainWindow field0;    ///< TerrainSurfaceDesc::fields output 0 (the space toolbox: temperature, C)
    TerrainWindow field1;    ///< output 1 (the space toolbox: moisture, 0 to 1)
    float strength = 1.0f;
};

struct TerrainSlot {
    std::uint32_t layer = 0;         ///< in the library
    std::vector<TerrainRule> rules;  ///< where it shows: the largest of its rules (a cliff, or deep in a dig)
    /// The opt-in procedural albedo: paint `palette` (linear rgb) with the scan's light and shade, instead of the
    /// scan's own color.
    bool use_palette = false;
    math::vec3 palette{0.5f};
    float palette_contrast = 1.0f;  ///< how strongly the scan's luminance shows through the palette color
    float roughness_scale = 1.0f;
    float normal_strength = 1.0f;
};

enum class TerrainUp : std::uint8_t {
    radial,  ///< a body centered on its frame's origin: up is the position's direction, altitude |p| - datum
    y,       ///< a flat world: +Y up, altitude y - datum
};

/// The vertex positions (body frame, meters) in, two fields per vertex out. Called on chunk jobs: thread-safe.
using TerrainFields = std::function<void(std::span<const math::dvec3> positions, std::span<std::array<float, 2>> out)>;

struct TerrainSurfaceDesc {
    std::string name;
    std::vector<TerrainSlot> slots;  ///< 1 to terrain_surface_slots
    TerrainUp up = TerrainUp::radial;
    double datum_m = 0.0;          ///< the radius (radial) or height (y) altitude counts from
    TerrainFields fields;          ///< empty: both fields are 0
    float macro_strength = 0.12f;  ///< large-scale brightness variation against tiling
    float height_blend = 0.6f;     ///< how much the layers' heights decide their borders
    float triplanar_sharpness = 6.0f;
    float fade_start = 0.006f;  ///< a scale's detail fades from footprint / tile = fade_start ...
    float fade_end = 0.03f;     ///< ... to fade_end, then the layer's mean stands in
    float ao_strength = 0.6f;
    float normal_scale = 1.0f;
};

/// One terrain vertex as the rules see it.
struct TerrainVertexInput {
    math::dvec3 position{0.0};  ///< body frame, meters
    math::vec3 normal{0.0f, 1.0f, 0.0f};
    double depth = 0.0;  ///< below the generated surface, meters
};

/// A chunk's vertices as the rules see them: body-frame positions (exact, from the lattice), normals, and the depth
/// below the generated surface where an edit of `body` touches the chunk (its source's generated_distance); 0
/// elsewhere, or with no body. What TerrainModule's chunk jobs pass to TerrainSurface::weights.
[[nodiscard]] std::vector<TerrainVertexInput> terrain_vertex_inputs(const voxel::ChunkRecord& record,
                                                                    const voxel::TerrainBody* body);

/// A body's look: its slots, rules and the TerrainMaterial that draws them.
class TerrainSurface {
public:
    /// Fails with render.mesh_invalid for no slots, more than six, or a layer the library lacks.
    [[nodiscard]] static Result<std::shared_ptr<const TerrainSurface>> make(
        std::shared_ptr<const TerrainLayerLibrary> library, TerrainSurfaceDesc desc);

    /// The slot weights of each vertex, summing to 1 (all 0 when no rule lets it through: the shader shows grey).
    /// Canonical: the same bits on every platform for the same inputs and fields.
    void weights(std::span<const TerrainVertexInput> vertices, std::span<std::array<float, 6>> out) const;

    /// The TerrainMaterial for this surface in `store` (its layer table and the library's arrays), made on first call.
    [[nodiscard]] Result<AssetId> material(render::MeshStore& store) const;

    [[nodiscard]] const TerrainSurfaceDesc& desc() const { return desc_; }
    [[nodiscard]] const TerrainLayerLibrary& library() const { return *library_; }
    /// The layer table TerrainMaterial reads (rgba32f, 4 x 7): rows of slots, then the globals.
    [[nodiscard]] TextureAsset table() const;

    TerrainSurface(std::shared_ptr<const TerrainLayerLibrary> library, TerrainSurfaceDesc desc);

private:
    std::shared_ptr<const TerrainLayerLibrary> library_;
    TerrainSurfaceDesc desc_;
    mutable std::mutex mutex_;
    mutable render::MeshStore* store_ = nullptr;
    mutable AssetId material_{};
};

}  // namespace veach
