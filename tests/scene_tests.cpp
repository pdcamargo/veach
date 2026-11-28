// First real scene: the asset pipeline (docs/specs/first-real-scene/spec.md).

#include "support/test_support.hpp"

#include <veach/asset/scene.hpp>
#include <veach/asset/texture.hpp>

#include <bc7decomp.h>
#include <doctest/doctest.h>

#include <algorithm>
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
    // BC7 sizes round up to multiples of 4 (rhi.invalid_texture); RGBA8 keeps 5x3 above.
    const TextureAsset rounded = cook_texture(test_image(5, 3), {.srgb = true, .compress = true});
    CHECK(rounded.width == 8);
    CHECK(rounded.height == 4);
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

TEST_CASE("[scene.AC2] an unaligned BC7 texture is resampled to whole blocks, so UVs still map to the image") {
    // 255x129 is a multiple of 4 in neither direction: the cooker rounds it to 256x132 by resampling. Padding
    // would shift every UV (the image would end at u = 255/256); resampling keeps u = 1 at the image's edge.
    const std::uint32_t w = 255;
    const std::uint32_t h = 129;
    ImageRgba8 image{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = &image.pixels[(std::size_t{y} * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(std::lround(255.0 * x / (w - 1)));  // u ramp
            p[1] = static_cast<std::uint8_t>(std::lround(255.0 * y / (h - 1)));  // v ramp
            p[2] = 128;
            p[3] = 255;
        }
    }
    const TextureAsset cooked = cook_texture(image, {.srgb = false, .compress = true});
    REQUIRE(cooked.width == 256);
    REQUIRE(cooked.height == 132);
    const std::uint32_t bw = cooked.width / 4;
    const auto texel = [&](std::uint32_t x, std::uint32_t y) {
        std::array<bc7decomp::color_rgba, 16> block{};
        REQUIRE(bc7decomp::unpack_bc7(&cooked.mips[0][(std::size_t{y / 4} * bw + x / 4) * 16], block.data()));
        return block[(y % 4) * 4 + x % 4];
    };
    // At every texel center of the cooked image, the ramps must read what the source has at the same UV.
    double worst = 0.0;
    for (std::uint32_t y = 0; y < cooked.height; ++y) {
        for (std::uint32_t x = 0; x < cooked.width; ++x) {
            const double u = (x + 0.5) / cooked.width;
            const double v = (y + 0.5) / cooked.height;
            const double expected_r = std::clamp((u * w - 0.5) / (w - 1), 0.0, 1.0) * 255.0;
            const double expected_g = std::clamp((v * h - 0.5) / (h - 1), 0.0, 1.0) * 255.0;
            const bc7decomp::color_rgba t = texel(x, y);
            worst = std::max({worst, std::abs(t.m_comps[0] - expected_r), std::abs(t.m_comps[1] - expected_g)});
        }
    }
    MESSAGE("worst ramp error: ", worst, " levels");
    CHECK(worst <= 4.0);  // BC7 quantization; a padded image is off by up to 255 at its far edge
}

namespace {

/// An AssetServer on a temp copy of the lab's Cornell box, with the glTF and KTX2 loaders.
struct CornellBox {
    test::TempProject project{"cornell"};
    World world;
    std::unique_ptr<AssetServer> assets;
    CornellBox() {
        const auto dir = test::source_dir() / "apps/lab/assets/cornell_box";
        for (const char* file : {"cornell_box.gltf", "cornell_box.bin", "checker.png"}) {
            project.write(std::string("box/") + file, *fs::read_file(dir / file));
        }
        reopen();
    }
    void reopen() {
        world = World();
        world.import<CoreModule>();
        world.import<AssetModule>();
        assets = std::make_unique<AssetServer>(world, project.path());
        assets->add_loader(std::make_unique<GltfLoader>());
    }
    const SceneAsset& load() {
        auto scene = assets->wait(assets->load<SceneAsset>("box/cornell_box.gltf"));
        REQUIRE_MESSAGE(scene.has_value(), (scene ? std::string() : scene.error().message));
        return **scene;
    }
};

}  // namespace

TEST_CASE("[scene.AC1] glTF imports into scenes, meshes and materials with stable ids and a cook cache") {
    // RFC 9562 name-based ids, checked against Python's uuid.uuid5.
    CHECK(AssetId::derive(*AssetId::parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8"), "mesh/3").to_string() ==
          "63f0bac7-4de5-50de-8470-2097d5b50170");

    CornellBox box;
    const SceneAsset& first = box.load();
    CHECK_FALSE(first.from_cache);
    CHECK(first.nodes.size() == 6);
    CHECK(first.meshes.size() == 5);
    CHECK(first.materials.size() == 7);  // six, plus the default for primitives without one
    CHECK(first.textures.size() == 1);   // the cutout panel's checker, cooked to BC7
    CHECK(first.lights.size() == 1);
    CHECK(first.lights[0].kind == LightKind::point);
    const std::vector<AssetId> meshes = first.meshes;

    const MeshAsset* walls = box.assets->get(Handle<MeshAsset>{first.meshes[0]});
    REQUIRE(walls != nullptr);
    CHECK(walls->slot_count == 3);  // white, red, green
    CHECK(walls->indices.size() == 5 * 6);
    CHECK(walls->triangle_slots.size() == 10);
    // Tangents were generated: unit length and perpendicular to the normal.
    const MeshVertex& v = walls->vertices[0];
    CHECK(std::abs(v.tangent[0] * v.normal[0] + v.tangent[1] * v.normal[1] + v.tangent[2] * v.normal[2]) < 1e-5f);

    const auto glow = std::ranges::find_if(first.materials, [&](AssetId id) {
        const MaterialAsset* m = box.assets->get(Handle<MaterialAsset>{id});
        return m != nullptr && m->name == "glow";
    });
    REQUIRE(glow != first.materials.end());
    const MaterialAsset* glow_material = box.assets->get(Handle<MaterialAsset>{*glow});
    CHECK(glow_material->kind == "GlowMaterial");
    CHECK(glow_material->emissive_strength == doctest::Approx(3.0f));

    // A second import (a fresh server) reuses the cooked data and derives the same ids.
    box.reopen();
    const SceneAsset& second = box.load();
    CHECK(second.from_cache);
    CHECK(second.meshes == meshes);
    // Deleting the cache only costs a re-cook.
    std::filesystem::remove_all(box.project.path() / ".veach");
    box.reopen();
    const SceneAsset& third = box.load();
    CHECK_FALSE(third.from_cache);
    CHECK(third.meshes == meshes);
}

TEST_CASE("[scene.AC1] a buffer shorter than declared, such as a Git LFS pointer, is an error, not a read past its end") {
    CornellBox box;
    box.project.write("box/cornell_box.bin",
                      "version https://git-lfs.github.com/spec/v1\noid sha256:0000\nsize 4242\n");
    box.reopen();
    auto scene = box.assets->wait(box.assets->load<SceneAsset>("box/cornell_box.gltf"));
    REQUIRE_FALSE(scene.has_value());
    CHECK(scene.error().code == ErrorCode::asset_load_failed);
    CHECK(scene.error().message.find("buffer 0 has") != std::string::npos);
    CHECK(scene.error().hint.find("git lfs pull") != std::string::npos);
}
