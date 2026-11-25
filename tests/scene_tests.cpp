// First real scene: the asset pipeline (docs/specs/first-real-scene/spec.md).

#include "support/test_support.hpp"

#include <veach/asset/texture.hpp>

#include <bc7decomp.h>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>

using namespace veach;

namespace {

/// Smooth gradients plus a hard-edged checker: what textures look like, and what BC7 has to get right.
ImageRgba8 test_image(std::uint32_t w, std::uint32_t h) {
    ImageRgba8 image{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = &image.pixels[(std::size_t{y} * w + x) * 4];
            const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
            p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            p[2] = checker ? 200 : 40;
            p[3] = 255;
        }
    }
    return image;
}

double psnr_bc7(const ImageRgba8& source, const std::vector<std::byte>& blocks) {
    const std::uint32_t bw = (source.width + 3) / 4;
    double squared = 0.0;
    std::size_t count = 0;
    for (std::uint32_t by = 0; by < (source.height + 3) / 4; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            std::array<bc7decomp::color_rgba, 16> decoded{};
            REQUIRE(bc7decomp::unpack_bc7(&blocks[(std::size_t{by} * bw + bx) * 16], decoded.data()));
            for (std::uint32_t i = 0; i < 16; ++i) {
                const std::uint32_t x = bx * 4 + i % 4;
                const std::uint32_t y = by * 4 + i / 4;
                if (x >= source.width || y >= source.height) {
                    continue;
                }
                for (std::uint32_t c = 0; c < 3; ++c) {
                    const double d =
                        double(decoded[i].m_comps[c]) - source.pixels[(std::size_t{y} * source.width + x) * 4 + c];
                    squared += d * d;
                    ++count;
                }
            }
        }
    }
    const double mse = squared / double(count);
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

TEST_CASE("[scene.AC1] cooked textures round-trip through KTX2") {
    const TextureAsset cooked = cook_texture(test_image(64, 48), {.srgb = true, .compress = true});
    CHECK(cooked.format == TextureFormat::bc7_srgb);
    REQUIRE(cooked.mips.size() == 7);  // 64x48 down to 1x1
    CHECK(cooked.mips[0].size() == texture_level_size(TextureFormat::bc7_srgb, 64, 48));
    CHECK(cooked.mips[6].size() == 16);  // a 1x1 level is one block

    const std::string file = write_ktx2(cooked);
    CHECK(std::memcmp(file.data(), "\xABKTX 20\xBB\r\n\x1A\n", 12) == 0);
    auto read = read_ktx2(file, "test.ktx2");
    REQUIRE_MESSAGE(read.has_value(), (read ? std::string() : read.error().message));
    CHECK(read->format == cooked.format);
    CHECK(read->width == 64);
    CHECK(read->height == 48);
    CHECK(read->mips == cooked.mips);

    const TextureAsset plain = cook_texture(test_image(5, 3), {.srgb = false, .compress = false});
    auto plain_read = read_ktx2(write_ktx2(plain), "plain.ktx2");
    REQUIRE(plain_read.has_value());
    CHECK(plain_read->format == TextureFormat::rgba8_unorm);
    CHECK(plain_read->mips == plain.mips);

    CHECK(cook_texture(test_image(64, 48), {.compress = false, .max_size = 16}).width == 16);
    CHECK_FALSE(read_ktx2(file.substr(0, 40), "cut.ktx2").has_value());
    CHECK_FALSE(read_ktx2(std::string(80, 'x'), "junk.ktx2").has_value());
}

TEST_CASE("[scene.AC2] BC7 cooking keeps the image (PSNR >= 35 dB)") {
    const ImageRgba8 image = test_image(64, 48);
    const TextureAsset cooked = cook_texture(image, {.srgb = false, .compress = true});
    const double psnr = psnr_bc7(image, cooked.mips[0]);
    MESSAGE("BC7 PSNR: ", psnr, " dB");
    CHECK(psnr >= 35.0);
    // Deterministic: cooking twice gives the same bytes (the cook cache relies on it).
    CHECK(cook_texture(image, {.srgb = false, .compress = true}).mips == cooked.mips);
}
