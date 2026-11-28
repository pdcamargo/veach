#pragma once

// Textures as the runtime uses them (ADR 0011): a full mip chain in one GPU
// format, stored as KTX2. Cooking turns a decoded RGBA8 image into BC7 with
// box-filtered mips (in linear light for sRGB data). A texture may be a 2D array (terrain material layers,
// docs/specs/terrain-materials/spec.md): each level holds every layer, in KTX2's order.

#include <veach/asset/loader.hpp>
#include <veach/core/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach {

class JobSystem;

/// The GPU formats a TextureAsset can hold. The renderer maps them to rhi::Format. rgba32_float holds data tables
/// (a material's parameters, loaded and never filtered), not images.
enum class TextureFormat : std::uint8_t { rgba8_unorm, rgba8_srgb, bc7_unorm, bc7_srgb, rgba32_float };

struct TextureAsset {
    TextureFormat format = TextureFormat::rgba8_unorm;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Level 0 first; each level is its layers one after another, rows (or 4x4 block rows) tightly packed.
    std::vector<std::vector<std::byte>> mips;
    std::uint32_t layers = 1;  ///< layers of a 2D array; 1 otherwise
    bool array = false;        ///< a 2D array, even of one layer (a shader's type never depends on a count, ADR 0090)
};

/// A decoded image, 8 bits per channel, RGBA, rows top to bottom.
struct ImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

/// Bytes of one mip level of `format`, one layer.
[[nodiscard]] std::size_t texture_level_size(TextureFormat format, std::uint32_t width, std::uint32_t height);

/// Stacks 2D textures of one format, size and mip count into a 2D array, layer i from textures[i]. Fails with
/// asset.load_failed, naming the first that differs.
[[nodiscard]] Result<TextureAsset> stack_layers(std::span<const TextureAsset> textures, std::string_view context);

/// KTX2, no supercompression, with a valid data format descriptor (https://registry.khronos.org/KTX/specs/2.0/).
[[nodiscard]] std::string write_ktx2(const TextureAsset& texture);
[[nodiscard]] Result<TextureAsset> read_ktx2(std::string_view bytes, std::string_view context);

struct TextureCookSettings {
    bool srgb = true;            ///< color data (base color, emissive); false for normals and metallic-roughness
    bool compress = true;        ///< BC7; false keeps RGBA8
    std::uint32_t max_size = 0;  ///< halve until both sides fit; 0 = no limit
};

/// Downsizes, rounds BC7 sizes up to multiples of 4, builds the mip chain and encodes. Deterministic: the same input
/// gives the same bytes.
[[nodiscard]] TextureAsset cook_texture(const ImageRgba8& image, const TextureCookSettings& settings);

/// BC7 blocks of one image, rows of blocks top to bottom; partial blocks repeat their edge pixels. `perceptual`
/// weights the channels for sRGB color. With `jobs`, rows of blocks are encoded in parallel, to the same bytes.
[[nodiscard]] std::vector<std::byte> encode_bc7(const ImageRgba8& image, bool perceptual, JobSystem* jobs = nullptr);

/// Halves an image with a 2x2 box filter (clamped at odd edges), in linear light for the color channels when `srgb`.
[[nodiscard]] ImageRgba8 halve_image(const ImageRgba8& image, bool srgb);

/// Bilinear resampling to `width` x `height` (texel centers aligned), in linear light for sRGB color.
[[nodiscard]] ImageRgba8 resample_image(const ImageRgba8& image, std::uint32_t width, std::uint32_t height, bool srgb);

/// Decodes PNG or JPEG (stb_image).
[[nodiscard]] Result<ImageRgba8> decode_image(std::span<const std::byte> bytes, std::string_view context);

/// Loads cooked `.ktx2` files as TextureAsset.
class Ktx2Loader final : public TypedAssetLoader<TextureAsset> {
public:
    [[nodiscard]] std::string_view name() const override { return "ktx2"; }
    [[nodiscard]] std::span<const std::string_view> extensions() const override { return extensions_; }
    [[nodiscard]] Result<TextureAsset> load(const LoadContext& ctx) const override {
        return read_ktx2(ctx.bytes, ctx.path.generic_string());
    }

private:
    static constexpr std::array<std::string_view, 1> extensions_{".ktx2"};
};

}  // namespace veach
