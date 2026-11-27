#include <veach/asset/texture.hpp>

#include <veach/core/profile.hpp>

#include <bc7enc.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace veach {

namespace {

// ---- KTX2 constants (KTX 2.0 spec; Khronos Data Format 1.3) ---------------------------------------------

constexpr std::array<std::uint8_t, 12> ktx2_identifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                       0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::uint32_t vk_r8g8b8a8_unorm = 37;
constexpr std::uint32_t vk_r8g8b8a8_srgb = 43;
constexpr std::uint32_t vk_bc7_unorm_block = 145;
constexpr std::uint32_t vk_bc7_srgb_block = 146;

constexpr std::uint32_t khr_df_model_rgbsda = 1;
constexpr std::uint32_t khr_df_model_bc7 = 136;
constexpr std::uint32_t khr_df_primaries_bt709 = 1;
constexpr std::uint32_t khr_df_transfer_linear = 1;
constexpr std::uint32_t khr_df_transfer_srgb = 2;
constexpr std::uint32_t khr_df_sample_linear = 0x10;  ///< qualifier: this channel is linear even in an sRGB format
constexpr std::uint32_t khr_df_channel_alpha = 15;

constexpr std::size_t header_size = 12 + 9 * 4 + 4 * 4 + 2 * 8;  // identifier, header, index
constexpr std::size_t level_entry_size = 3 * 8;

bool is_bc7(TextureFormat f) {
    return f == TextureFormat::bc7_unorm || f == TextureFormat::bc7_srgb;
}
bool is_srgb(TextureFormat f) {
    return f == TextureFormat::rgba8_srgb || f == TextureFormat::bc7_srgb;
}

std::uint32_t vk_format(TextureFormat f) {
    switch (f) {
        case TextureFormat::rgba8_unorm: return vk_r8g8b8a8_unorm;
        case TextureFormat::rgba8_srgb: return vk_r8g8b8a8_srgb;
        case TextureFormat::bc7_unorm: return vk_bc7_unorm_block;
        case TextureFormat::bc7_srgb: return vk_bc7_srgb_block;
    }
    return 0;
}

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}
void put64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}
void set64(std::string& out, std::size_t at, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    }
}
std::uint32_t get32(std::string_view in, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= std::uint32_t{static_cast<std::uint8_t>(in[at + i])} << (8 * i);
    }
    return v;
}
std::uint64_t get64(std::string_view in, std::size_t at) {
    return std::uint64_t{get32(in, at)} | (std::uint64_t{get32(in, at + 4)} << 32);
}

/// The data format descriptor: one basic block, with one sample for BC7 or four (R, G, B, A) for RGBA8.
std::string data_format_descriptor(TextureFormat format) {
    const bool bc7 = is_bc7(format);
    const std::uint32_t samples = bc7 ? 1 : 4;
    const std::uint32_t block_size = 24 + 16 * samples;
    std::string out;
    put32(out, 4 + block_size);          // dfdTotalSize
    put32(out, 0);                       // vendorId KHR, descriptorType basic
    put32(out, 2 | (block_size << 16));  // versionNumber 2, descriptorBlockSize
    const std::uint32_t transfer = is_srgb(format) ? khr_df_transfer_srgb : khr_df_transfer_linear;
    put32(out, (bc7 ? khr_df_model_bc7 : khr_df_model_rgbsda) | (khr_df_primaries_bt709 << 8) | (transfer << 16));
    put32(out, bc7 ? (3u | (3u << 8)) : 0u);  // texelBlockDimension0..3 (minus one)
    put32(out, bc7 ? 16u : 4u);               // bytesPlane0..3
    put32(out, 0);                            // bytesPlane4..7
    if (bc7) {
        put32(out, 0 | (127u << 16));  // 128 bits of BC7 color
        put32(out, 0);
        put32(out, 0);
        put32(out, 0xFFFFFFFFu);
        return out;
    }
    constexpr std::array<std::uint32_t, 4> channels{0, 1, 2, khr_df_channel_alpha};
    for (std::uint32_t c = 0; c < 4; ++c) {
        std::uint32_t type = channels.at(c);
        if (c == 3 && is_srgb(format)) {
            type |= khr_df_sample_linear;  // alpha is never sRGB-encoded
        }
        put32(out, (c * 8) | (7u << 16) | (type << 24));
        put32(out, 0);
        put32(out, 0);
        put32(out, 255);
    }
    return out;
}

// ---- cooking ----------------------------------------------------------------------------------------------

float srgb_to_linear(std::uint8_t v) {
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(s * 255.0f));
}

/// Halves an image with a 2x2 box filter (clamped at odd edges), in linear light when `srgb`.
ImageRgba8 halve(const ImageRgba8& in, bool srgb) {
    ImageRgba8 out;
    out.width = std::max(1u, in.width / 2);
    out.height = std::max(1u, in.height / 2);
    out.pixels.resize(std::size_t{out.width} * out.height * 4);
    const auto at = [&](std::uint32_t x, std::uint32_t y, std::uint32_t c) {
        x = std::min(x, in.width - 1);
        y = std::min(y, in.height - 1);
        return in.pixels[(std::size_t{y} * in.width + x) * 4 + c];
    };
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                const std::array<std::uint8_t, 4> v{at(2 * x, 2 * y, c), at(2 * x + 1, 2 * y, c),
                                                    at(2 * x, 2 * y + 1, c), at(2 * x + 1, 2 * y + 1, c)};
                std::uint8_t result = 0;
                if (srgb && c < 3) {
                    float sum = 0.0f;
                    for (const std::uint8_t s : v) {
                        sum += srgb_to_linear(s);
                    }
                    result = linear_to_srgb(sum / 4.0f);
                } else {
                    const unsigned sum = unsigned{v[0]} + v[1] + v[2] + v[3];
                    result = static_cast<std::uint8_t>((sum + 2) / 4);
                }
                out.pixels[(std::size_t{y} * out.width + x) * 4 + c] = result;
            }
        }
    }
    return out;
}

std::vector<std::byte> encode_bc7(const ImageRgba8& image, bool perceptual) {
    static std::once_flag init;
    std::call_once(init, [] { bc7enc_compress_block_init(); });
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (perceptual) {
        bc7enc_compress_block_params_init_perceptual_weights(&params);
    } else {
        bc7enc_compress_block_params_init_linear_weights(&params);
    }
    params.m_uber_level = 0;  // the fast end: cooking is on the load path (ADR 0011)

    const std::uint32_t blocks_x = (image.width + 3) / 4;
    const std::uint32_t blocks_y = (image.height + 3) / 4;
    std::vector<std::byte> out(std::size_t{blocks_x} * blocks_y * 16);
    std::array<std::uint8_t, 64> block{};
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    // Partial blocks repeat their edge pixels.
                    const std::uint32_t sx = std::min(bx * 4 + x, image.width - 1);
                    const std::uint32_t sy = std::min(by * 4 + y, image.height - 1);
                    std::memcpy(&block.at((y * 4 + x) * 4), &image.pixels[(std::size_t{sy} * image.width + sx) * 4], 4);
                }
            }
            bc7enc_compress_block(&out[(std::size_t{by} * blocks_x + bx) * 16], block.data(), &params);
        }
    }
    return out;
}

}  // namespace

std::size_t texture_level_size(TextureFormat format, std::uint32_t width, std::uint32_t height) {
    if (is_bc7(format)) {
        return std::size_t{(width + 3) / 4} * ((height + 3) / 4) * 16;
    }
    return std::size_t{width} * height * 4;
}

std::string write_ktx2(const TextureAsset& texture) {
    const auto levels = static_cast<std::uint32_t>(texture.mips.size());
    const std::string dfd = data_format_descriptor(texture.format);
    std::string out(ktx2_identifier.begin(), ktx2_identifier.end());
    put32(out, vk_format(texture.format));
    put32(out, 1);  // typeSize
    put32(out, texture.width);
    put32(out, texture.height);
    put32(out, 0);  // pixelDepth: 2D
    put32(out, 0);  // layerCount: not an array
    put32(out, 1);  // faceCount
    put32(out, levels);
    put32(out, 0);  // supercompressionScheme: none
    const std::size_t dfd_offset = header_size + level_entry_size * levels;
    put32(out, static_cast<std::uint32_t>(dfd_offset));
    put32(out, static_cast<std::uint32_t>(dfd.size()));
    put32(out, 0);  // no key/value data
    put32(out, 0);
    put64(out, 0);  // no supercompression global data
    put64(out, 0);
    const std::size_t level_index = out.size();
    out.resize(out.size() + level_entry_size * levels, '\0');
    out += dfd;
    // Levels are stored smallest first, each aligned to 16 (lcm of the texel block size and 4).
    for (std::uint32_t i = levels; i-- > 0;) {
        out.resize((out.size() + 15) / 16 * 16, '\0');
        const std::size_t at = level_index + level_entry_size * i;
        set64(out, at, out.size());
        set64(out, at + 8, texture.mips[i].size());
        set64(out, at + 16, texture.mips[i].size());
        out.append(reinterpret_cast<const char*>(texture.mips[i].data()), texture.mips[i].size());
    }
    return out;
}

Result<TextureAsset> read_ktx2(std::string_view in, std::string_view context) {
    const auto invalid = [&](std::string_view why) {
        return make_error(ErrorCode::asset_load_failed,
                          std::format("{}: not a KTX2 texture Veach reads: {}", context, why),
                          "Veach reads uncompressed-container KTX2 in RGBA8 or BC7, as its importer writes them.");
    };
    if (in.size() < header_size || std::memcmp(in.data(), ktx2_identifier.data(), ktx2_identifier.size()) != 0) {
        return invalid("bad identifier");
    }
    TextureAsset texture;
    const std::uint32_t format = get32(in, 12);
    if (format == vk_r8g8b8a8_unorm) {
        texture.format = TextureFormat::rgba8_unorm;
    } else if (format == vk_r8g8b8a8_srgb) {
        texture.format = TextureFormat::rgba8_srgb;
    } else if (format == vk_bc7_unorm_block) {
        texture.format = TextureFormat::bc7_unorm;
    } else if (format == vk_bc7_srgb_block) {
        texture.format = TextureFormat::bc7_srgb;
    } else {
        return invalid(std::format("vkFormat {}", format));
    }
    texture.width = get32(in, 20);
    texture.height = get32(in, 24);
    const std::uint32_t depth = get32(in, 28);
    const std::uint32_t layers = get32(in, 32);
    const std::uint32_t faces = get32(in, 36);
    const std::uint32_t levels = std::max(1u, get32(in, 40));
    if (get32(in, 44) != 0) {
        return invalid("supercompressed");
    }
    if (depth > 1 || layers > 1 || faces != 1 || texture.width == 0 || texture.height == 0) {
        return invalid("not a single 2D image");
    }
    if (in.size() < header_size + level_entry_size * levels) {
        return invalid("truncated level index");
    }
    for (std::uint32_t i = 0; i < levels; ++i) {
        const std::size_t at = header_size + level_entry_size * i;
        const std::uint64_t offset = get64(in, at);
        const std::uint64_t length = get64(in, at + 8);
        const std::size_t expected =
            texture_level_size(texture.format, std::max(1u, texture.width >> i), std::max(1u, texture.height >> i));
        if (length != expected || offset + length > in.size()) {
            return invalid(std::format("level {} has {} bytes at {}, expected {}", i, length, offset, expected));
        }
        const auto* begin = reinterpret_cast<const std::byte*>(in.data() + offset);
        texture.mips.emplace_back(begin, begin + length);
    }
    return texture;
}

namespace {

/// Bilinear resampling to `width` x `height` (texel centers aligned), in linear light for sRGB color.
ImageRgba8 resample(const ImageRgba8& in, std::uint32_t width, std::uint32_t height, bool srgb) {
    ImageRgba8 out{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
    const auto texel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t c) {
        const std::uint8_t v = in.pixels[(std::size_t{y} * in.width + x) * 4 + c];
        return srgb && c < 3 ? srgb_to_linear(v) : static_cast<float>(v) / 255.0f;
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        const float sy = std::clamp(
            (static_cast<float>(y) + 0.5f) * static_cast<float>(in.height) / static_cast<float>(height) - 0.5f, 0.0f,
            static_cast<float>(in.height - 1));
        const auto y0 = static_cast<std::uint32_t>(sy);
        const std::uint32_t y1 = std::min(y0 + 1, in.height - 1);
        const float fy = sy - static_cast<float>(y0);
        for (std::uint32_t x = 0; x < width; ++x) {
            const float sx = std::clamp(
                (static_cast<float>(x) + 0.5f) * static_cast<float>(in.width) / static_cast<float>(width) - 0.5f, 0.0f,
                static_cast<float>(in.width - 1));
            const auto x0 = static_cast<std::uint32_t>(sx);
            const std::uint32_t x1 = std::min(x0 + 1, in.width - 1);
            const float fx = sx - static_cast<float>(x0);
            for (std::uint32_t c = 0; c < 4; ++c) {
                const float top = texel(x0, y0, c) * (1.0f - fx) + texel(x1, y0, c) * fx;
                const float bottom = texel(x0, y1, c) * (1.0f - fx) + texel(x1, y1, c) * fx;
                const float v = top * (1.0f - fy) + bottom * fy;
                out.pixels[(std::size_t{y} * width + x) * 4 + c] =
                    srgb && c < 3 ? linear_to_srgb(v)
                                  : static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    return out;
}

}  // namespace

TextureAsset cook_texture(const ImageRgba8& source, const TextureCookSettings& settings) {
    VEACH_ZONE("cook_texture");
    ImageRgba8 image = source;
    while (settings.max_size != 0 && (image.width > settings.max_size || image.height > settings.max_size)) {
        image = halve(image, settings.srgb);
    }
    if (settings.compress && (image.width % 4 != 0 || image.height % 4 != 0)) {
        // Block-compressed sizes are multiples of 4 on every backend (rhi.invalid_texture).
        image = resample(image, (image.width + 3) / 4 * 4, (image.height + 3) / 4 * 4, settings.srgb);
    }
    TextureAsset texture;
    texture.format = settings.compress ? (settings.srgb ? TextureFormat::bc7_srgb : TextureFormat::bc7_unorm)
                                       : (settings.srgb ? TextureFormat::rgba8_srgb : TextureFormat::rgba8_unorm);
    texture.width = image.width;
    texture.height = image.height;
    while (true) {
        if (settings.compress) {
            texture.mips.push_back(encode_bc7(image, settings.srgb));
        } else {
            const auto* begin = reinterpret_cast<const std::byte*>(image.pixels.data());
            texture.mips.emplace_back(begin, begin + image.pixels.size());
        }
        if (image.width == 1 && image.height == 1) {
            break;
        }
        image = halve(image, settings.srgb);
    }
    return texture;
}

Result<ImageRgba8> decode_image(std::span<const std::byte> bytes, std::string_view context) {
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                            static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        return make_error(ErrorCode::asset_load_failed,
                          std::format("{}: cannot decode the image: {}", context, stbi_failure_reason()),
                          "glTF images must be PNG or JPEG.");
    }
    ImageRgba8 image{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                     std::vector<std::uint8_t>(pixels, pixels + std::size_t(w) * std::size_t(h) * 4)};
    stbi_image_free(pixels);
    return image;
}

}  // namespace veach
