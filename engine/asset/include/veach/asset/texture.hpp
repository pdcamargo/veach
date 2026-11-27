#pragma once

// Textures as the runtime uses them (ADR 0011): a full mip chain in one GPU
// format, stored as KTX2. Cooking turns a decoded RGBA8 image into BC7 with
// box-filtered mips (in linear light for sRGB data).

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

/// The GPU formats a TextureAsset can hold. The renderer maps them to rhi::Format.
enum class TextureFormat : std::uint8_t { rgba8_unorm, rgba8_srgb, bc7_unorm, bc7_srgb };

struct TextureAsset {
    TextureFormat format = TextureFormat::rgba8_unorm;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::vector<std::byte>> mips;  ///< level 0 first; rows (or 4x4 block rows) tightly packed
};

/// A decoded image, 8 bits per channel, RGBA, rows top to bottom.
struct ImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

/// Bytes of one mip level of `format`.
[[nodiscard]] std::size_t texture_level_size(TextureFormat format, std::uint32_t width, std::uint32_t height);

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
