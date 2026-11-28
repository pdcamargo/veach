#include "sp16_spike.hpp"

#include "lab_shaders.hpp"

#include <veach/core/env.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lab {

namespace {

namespace rhi = veach::rhi;
namespace render = veach::render;
namespace math = veach::math;

// ---- configuration --------------------------------------------------------------------------------------------

enum class Path : std::uint8_t { vis, forward, prepass };

struct Config {
    Path path = Path::vis;
    std::uint32_t msaa = 1;
    std::string view = "ground";
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t lights = 16;
    bool hiz = true;
    float grass_range = 150.0f;
    std::uint32_t timings = 0;
    std::uint32_t warmup = 120;
    bool stats = false;
    float pan = 0.0f;
    float strafe = 0.0f;     ///< engine: meters a frame to the view's right
    bool taa_off = false;    ///< engine: AntiAliasing{temporal = false} on the camera
    std::uint32_t ssaa = 1;  ///< render this many times wider and higher, box-filter down (a reference image)
};

double number(const char* name, double fallback) {
    const std::optional<std::string> text = veach::env(name);
    return text ? std::strtod(text->c_str(), nullptr) : fallback;
}

Config config_from_env(const std::string& mode) {
    Config c;
    c.path = mode == "forward" ? Path::forward : (mode == "prepass" ? Path::prepass : Path::vis);
    c.msaa = static_cast<std::uint32_t>(number("VEACH_SP16_MSAA", 1.0));
    if (c.path == Path::vis) {
        c.msaa = 1;
    }
    c.view = veach::env("VEACH_SP16_VIEW").value_or("ground");
    if (const std::optional<std::string> size = veach::env("VEACH_SP16_SIZE")) {
        char* end = nullptr;
        c.width = static_cast<std::uint32_t>(std::strtoul(size->c_str(), &end, 10));
        c.height =
            static_cast<std::uint32_t>(std::strtoul(end != nullptr && *end == 'x' ? end + 1 : "1080", nullptr, 10));
    }
    c.lights = std::min(64u, static_cast<std::uint32_t>(number("VEACH_SP16_LIGHTS", 16.0)));
    c.hiz = veach::env("VEACH_SP16_HIZ").value_or("1") != "0";
    c.grass_range = static_cast<float>(number("VEACH_SP16_GRASS_RANGE", 150.0));
    c.timings = static_cast<std::uint32_t>(number("VEACH_SP16_TIMINGS", 0.0));
    c.warmup = static_cast<std::uint32_t>(number("VEACH_SP16_WARMUP", 120.0));
    c.stats = veach::env_is_one("VEACH_SP16_STATS");
    c.pan = static_cast<float>(number("VEACH_SP16_PAN", 0.0));
    c.strafe = static_cast<float>(number("VEACH_SP16_STRAFE", 0.0));
    c.taa_off = veach::env("VEACH_SP16_TAA").value_or("1") == "0";
    c.ssaa = std::max(1u, static_cast<std::uint32_t>(number("VEACH_SP16_SSAA", 1.0)));
    return c;
}

std::string_view path_name(const Config& c) {
    switch (c.path) {
        case Path::vis: return "vis";
        case Path::forward: return c.msaa > 1 ? "forward_msaa" : "forward";
        case Path::prepass: return c.msaa > 1 ? "prepass_msaa" : "prepass";
    }
    return "?";
}

// ---- noise (visual only: placement and textures, never canonical) ---------------------------------------------------

std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

std::uint32_t hash(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    return hash(static_cast<std::uint32_t>(x) * 0x9E3779B1u ^ hash(static_cast<std::uint32_t>(y) + seed * 0x85EBCA77u));
}

float unit(std::uint32_t h) {
    return static_cast<float>(h >> 8) / 16777216.0f;
}

/// Value noise in [0, 1], periodic with `period` cells (0: not periodic).
float value_noise(float x, float y, std::uint32_t seed, std::int32_t period = 0) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const float tx = x - fx;
    const float ty = y - fy;
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    const auto wrap = [period](std::int32_t v) {
        return period > 0 ? ((v % period) + period) % period : v;
    };
    const auto at = [&](std::int32_t a, std::int32_t b) {
        return unit(hash(wrap(a), wrap(b), seed));
    };
    const float a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * sx;
    const float b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * sx;
    return a + (b - a) * sy;
}

float fbm(float x, float y, std::uint32_t seed, int octaves = 4, std::int32_t period = 0) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * value_noise(x, y, seed + static_cast<std::uint32_t>(o) * 101u, period);
        total += amplitude;
        x *= 2.0f;
        y *= 2.0f;
        period *= 2;
        amplitude *= 0.5f;
    }
    return sum / total;
}

float terrain_height(float x, float z) {
    return 14.0f * (fbm(x / 160.0f, z / 160.0f, 7u, 4) - 0.5f) + 2.0f * (fbm(x / 23.0f, z / 23.0f, 9u, 3) - 0.5f);
}

// ---- textures -------------------------------------------------------------------------------------------------

struct Rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

struct Image {
    std::uint32_t size = 0;
    std::vector<Rgba> texels;
    Rgba& at(std::uint32_t x, std::uint32_t y) { return texels[(y % size) * size + (x % size)]; }
    [[nodiscard]] const Rgba& at(std::uint32_t x, std::uint32_t y) const {
        return texels[(y % size) * size + (x % size)];
    }
};

Image blank(std::uint32_t size) {
    return {size, std::vector<Rgba>(static_cast<std::size_t>(size) * size)};
}

Rgba mix(Rgba a, Rgba b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

/// A tiling ground: grass and dirt patches.
Image ground_texture(std::uint32_t n) {
    Image im = blank(n);
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(n);
            const float v = static_cast<float>(y) / static_cast<float>(n);
            const float patch = fbm(u * 4.0f, v * 4.0f, 21u, 4, 4);
            const float fine = fbm(u * 64.0f, v * 64.0f, 22u, 2, 64);
            const Rgba grass{0.10f + 0.05f * fine, 0.20f + 0.08f * fine, 0.05f, 1.0f};
            const Rgba dirt{0.22f + 0.06f * fine, 0.17f + 0.04f * fine, 0.10f, 1.0f};
            im.at(x, y) = mix(grass, dirt, std::clamp((patch - 0.55f) * 6.0f, 0.0f, 1.0f));
        }
    }
    return im;
}

Image rock_texture(std::uint32_t n) {
    Image im = blank(n);
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(n);
            const float v = static_cast<float>(y) / static_cast<float>(n);
            const float g = 0.25f + 0.2f * fbm(u * 8.0f, v * 8.0f, 31u, 5, 8);
            const float moss = std::clamp((fbm(u * 3.0f, v * 3.0f, 32u, 3, 3) - 0.6f) * 4.0f, 0.0f, 1.0f);
            im.at(x, y) = mix({g, g * 0.97f, g * 0.92f, 1.0f}, {0.12f, 0.18f, 0.06f, 1.0f}, moss);
        }
    }
    return im;
}

Image bark_texture(std::uint32_t n) {
    Image im = blank(n);
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(n);
            const float v = static_cast<float>(y) / static_cast<float>(n);
            const float ridge = fbm(u * 24.0f, v * 3.0f, 41u, 4, 24);
            const float g = 0.08f + 0.14f * ridge;
            im.at(x, y) = {g * 1.3f, g, g * 0.75f, 1.0f};
        }
    }
    return im;
}

/// Grass blades on a transparent card, bases on the bottom row (v = 1).
Image grass_texture(std::uint32_t n) {
    Image im = blank(n);
    const float size = static_cast<float>(n);
    for (std::uint32_t blade = 0; blade < 56; ++blade) {
        const float x0 = 0.04f + 0.92f * unit(hash(blade * 3u + 1u));
        const float height = 0.45f + 0.53f * unit(hash(blade * 3u + 2u));
        const float lean = (unit(hash(blade * 3u + 3u)) - 0.5f) * 0.35f;
        const float width = 0.012f + 0.012f * unit(hash(blade * 7u + 5u));
        const float hue = unit(hash(blade * 11u + 9u));
        for (std::uint32_t y = 0; y < n; ++y) {
            const float t = 1.0f - (static_cast<float>(y) + 0.5f) / size;  // 0 at the base, 1 at the top
            if (t > height) {
                continue;
            }
            const float s = t / height;
            const float cx = x0 + lean * s * s;
            const float w = width * (1.0f - s * s);
            const auto lo = static_cast<std::int32_t>((cx - w) * size);
            const auto hi = static_cast<std::int32_t>((cx + w) * size) + 1;
            for (std::int32_t x = std::max(0, lo); x < std::min(static_cast<std::int32_t>(n), hi); ++x) {
                const float d = std::abs((static_cast<float>(x) + 0.5f) / size - cx) / std::max(w, 1e-4f);
                if (d > 1.0f) {
                    continue;
                }
                const Rgba base{0.04f, 0.10f + 0.03f * hue, 0.02f, 1.0f};
                const Rgba tip{0.30f + 0.12f * hue, 0.42f + 0.06f * hue, 0.10f, 1.0f};
                Rgba c = mix(base, tip, s);
                c.r *= 1.0f - 0.25f * d;
                c.g *= 1.0f - 0.25f * d;
                im.at(static_cast<std::uint32_t>(x), y) = c;
            }
        }
    }
    return im;
}

/// A cluster of leaves and twigs on a transparent card.
Image leaves_texture(std::uint32_t n, std::uint32_t seed) {
    Image im = blank(n);
    const float size = static_cast<float>(n);
    for (std::uint32_t leaf = 0; leaf < 34; ++leaf) {
        const float r = 0.42f * std::sqrt(unit(hash(seed + leaf * 5u + 1u)));
        const float phi = 6.2831853f * unit(hash(seed + leaf * 5u + 2u));
        const float cx = 0.5f + r * std::cos(phi);
        const float cy = 0.5f + r * std::sin(phi);
        const float angle = phi + (unit(hash(seed + leaf * 5u + 3u)) - 0.5f) * 1.2f;
        const float len = 0.09f + 0.05f * unit(hash(seed + leaf * 5u + 4u));
        const float wid = len * 0.42f;
        const float hue = unit(hash(seed + leaf * 5u + 5u));
        const float ca = std::cos(angle);
        const float sa = std::sin(angle);
        // the twig from the center to the leaf's base
        for (int k = 0; k < 64; ++k) {
            const float t = static_cast<float>(k) / 63.0f;
            const float px = 0.5f + (cx - len * ca - 0.5f) * t;
            const float py = 0.5f + (cy - len * sa - 0.5f) * t;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    im.at(static_cast<std::uint32_t>(px * size + static_cast<float>(dx)),
                          static_cast<std::uint32_t>(py * size + static_cast<float>(dy))) = {0.10f, 0.07f, 0.04f, 1.0f};
                }
            }
        }
        const auto x0 = static_cast<std::int32_t>((cx - len) * size);
        const auto y0 = static_cast<std::int32_t>((cy - len) * size);
        const auto x1 = static_cast<std::int32_t>((cx + len) * size) + 1;
        const auto y1 = static_cast<std::int32_t>((cy + len) * size) + 1;
        for (std::int32_t y = std::max(0, y0); y < std::min(static_cast<std::int32_t>(n), y1); ++y) {
            for (std::int32_t x = std::max(0, x0); x < std::min(static_cast<std::int32_t>(n), x1); ++x) {
                const float px = (static_cast<float>(x) + 0.5f) / size - cx;
                const float py = (static_cast<float>(y) + 0.5f) / size - cy;
                const float a = (px * ca + py * sa) / len;   // along the leaf, -1..1
                const float b = (-px * sa + py * ca) / wid;  // across
                const float half = std::sqrt(std::max(0.0f, 1.0f - a * a)) * (1.0f - 0.3f * a);
                if (std::abs(a) > 1.0f || std::abs(b) > half) {
                    continue;
                }
                const float vein = std::abs(b) < 0.08f ? 0.75f : 1.0f;
                const Rgba c{(0.07f + 0.08f * hue) * vein, (0.16f + 0.10f * hue) * vein, 0.04f * vein, 1.0f};
                im.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) = c;
            }
        }
    }
    return im;
}

/// Tangent-space normals from the image's luminance (and alpha), as a bump.
Image normal_map(const Image& im, float strength) {
    Image out = blank(im.size);
    const auto height = [&](std::uint32_t x, std::uint32_t y) {
        const Rgba& c = im.at(x, y);
        return (0.3f * c.r + 0.6f * c.g + 0.1f * c.b) * 4.0f + c.a;
    };
    for (std::uint32_t y = 0; y < im.size; ++y) {
        for (std::uint32_t x = 0; x < im.size; ++x) {
            const float dx = (height(x + 1, y) - height(x + im.size - 1, y)) * strength;
            const float dy = (height(x, y + 1) - height(x, y + im.size - 1)) * strength;
            const math::vec3 n = math::normalize(math::vec3(-dx, -dy, 1.0f));
            out.at(x, y) = {n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, n.z * 0.5f + 0.5f, 1.0f};
        }
    }
    return out;
}

/// Transparent texels take their opaque neighbors' color, so filtering does not darken edges.
void dilate(Image& im, int passes) {
    for (int p = 0; p < passes; ++p) {
        Image next = im;
        for (std::uint32_t y = 0; y < im.size; ++y) {
            for (std::uint32_t x = 0; x < im.size; ++x) {
                if (im.at(x, y).a > 0.0f) {
                    continue;
                }
                Rgba sum;
                float count = 0.0f;
                for (std::uint32_t k = 0; k < 4; ++k) {
                    const Rgba& c = im.at(x + (k == 0 ? 1 : (k == 1 ? im.size - 1 : 0)),
                                          y + (k == 2 ? 1 : (k == 3 ? im.size - 1 : 0)));
                    if (c.r + c.g + c.b > 0.0f) {
                        sum = {sum.r + c.r, sum.g + c.g, sum.b + c.b, 0.0f};
                        count += 1.0f;
                    }
                }
                if (count > 0.0f) {
                    next.at(x, y) = {sum.r / count, sum.g / count, sum.b / count, 0.0f};
                }
            }
        }
        im = std::move(next);
    }
}

float coverage(const Image& im, float cutoff, float scale) {
    std::size_t kept = 0;
    for (const Rgba& c : im.texels) {
        kept += c.a * scale >= cutoff ? 1 : 0;
    }
    return static_cast<float>(kept) / static_cast<float>(im.texels.size());
}

/// The mip chain; for alpha-tested images each level's alpha is scaled to keep level 0's coverage at the cutoff
/// (otherwise foliage thins out with distance).
std::vector<Image> mip_chain(Image level0, bool masked) {
    std::vector<Image> mips;
    const float target = coverage(level0, 0.5f, 1.0f);
    mips.push_back(std::move(level0));
    while (mips.back().size > 1) {
        const Image& src = mips.back();
        Image dst = blank(src.size / 2);
        for (std::uint32_t y = 0; y < dst.size; ++y) {
            for (std::uint32_t x = 0; x < dst.size; ++x) {
                Rgba sum;
                for (std::uint32_t k = 0; k < 4; ++k) {
                    const Rgba& c = src.at(2 * x + (k & 1), 2 * y + (k >> 1));
                    sum = {sum.r + c.r, sum.g + c.g, sum.b + c.b, sum.a + c.a};
                }
                dst.at(x, y) = {sum.r / 4.0f, sum.g / 4.0f, sum.b / 4.0f, sum.a / 4.0f};
            }
        }
        if (masked && target > 0.0f) {
            float lo = 0.25f;
            float hi = 16.0f;
            for (int i = 0; i < 20; ++i) {
                const float mid = 0.5f * (lo + hi);
                (coverage(dst, 0.5f, mid) < target ? lo : hi) = mid;
            }
            for (Rgba& c : dst.texels) {
                c.a = std::min(1.0f, c.a * hi);
            }
        }
        mips.push_back(std::move(dst));
    }
    return mips;
}

std::vector<std::byte> to_rgba8(const Image& im, bool srgb) {
    std::vector<std::byte> bytes(im.texels.size() * 4);
    const auto encode = [srgb](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        if (srgb) {
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        }
        return static_cast<std::byte>(static_cast<std::uint8_t>(std::lround(v * 255.0f)));
    };
    for (std::size_t i = 0; i < im.texels.size(); ++i) {
        bytes[i * 4 + 0] = encode(im.texels[i].r);
        bytes[i * 4 + 1] = encode(im.texels[i].g);
        bytes[i * 4 + 2] = encode(im.texels[i].b);
        bytes[i * 4 + 3] = encode(im.texels[i].a);
        if (srgb) {
            bytes[i * 4 + 3] = static_cast<std::byte>(
                static_cast<std::uint8_t>(std::lround(std::clamp(im.texels[i].a, 0.0f, 1.0f) * 255.0f)));
        }
    }
    return bytes;
}

// ---- GPU records (mirror apps/lab/shaders/sp16/common.slang) -----------------------------------------------------

struct Vertex {
    math::vec4 position_u;
    math::vec4 normal_v;
};
static_assert(sizeof(Vertex) == 32);

struct Instance {
    std::array<math::vec4, 3> rows;
    math::vec4 sphere;
    std::uint32_t model = 0;
    std::array<std::uint32_t, 3> pad{};
};
static_assert(sizeof(Instance) == 80);

struct Model {
    std::uint32_t first_part = 0;
    std::uint32_t part_count = 0;
    float max_distance = 1e30f;
    std::uint32_t pad = 0;
};
static_assert(sizeof(Model) == 16);

constexpr std::uint32_t part_masked = 1;

struct Part {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0;
    std::uint32_t flags = 0;
    std::uint32_t list_offset = 0;
    std::uint32_t capacity = 0;
    std::array<std::uint32_t, 2> pad{};
};
static_assert(sizeof(Part) == 32);

constexpr std::uint32_t material_foliage = 1;
constexpr std::uint32_t material_ground_ao = 2;
constexpr std::uint32_t material_world_uv = 4;

struct Material {
    math::vec4 tint;  ///< a: alpha cutoff
    float roughness = 0.8f;
    float metallic = 0.0f;
    float translucency = 0.0f;
    float normal_strength = 0.5f;
    std::uint32_t base_mips = 0;
    std::uint32_t normal_mips = 0;
    std::uint32_t mip_count = 1;
    std::uint32_t flags = 0;
    float uv_scale = 1.0f;
    float texture_size = 1.0f;
    std::array<std::uint32_t, 2> pad{};
};
static_assert(sizeof(Material) == 64);

struct Light {
    math::vec4 position_range;
    math::vec4 color;
};
static_assert(sizeof(Light) == 32);

struct FrameData {
    std::array<math::vec4, 4> view_projection;
    std::array<math::vec4, 4> inverse_view_projection;
    std::array<math::vec4, 4> previous_view_projection;
    std::array<math::vec4, 5> planes;
    math::vec4 camera;
    math::vec4 sun_direction;
    math::vec4 sun_color;
    math::vec4 sky_color;
    math::vec4 horizon_color;
    math::vec4 ground_color;
    std::array<std::uint32_t, 4> size{};
    std::array<std::uint32_t, 4> hiz{};
    std::array<std::uint32_t, 16> hiz_offsets{};
};
static_assert(sizeof(FrameData) == 464);

std::array<math::vec4, 4> columns(const math::mat4& m) {
    return {m[0], m[1], m[2], m[3]};
}

// ---- parameter blocks -------------------------------------------------------------------------------------------

struct SceneHandles {
    rhi::DescriptorHandle frame;
    rhi::DescriptorHandle vertices;
    rhi::DescriptorHandle indices;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle parts;
    rhi::DescriptorHandle lists;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle textures;
    rhi::DescriptorHandle sampler;
    rhi::DescriptorHandle clamp_sampler;
    rhi::DescriptorHandle lights;
};
static_assert(sizeof(SceneHandles) == 88);

struct alignas(16) RasterParams {  // Metal rounds a block with a 16-byte member up to 16
    SceneHandles scene;
    rhi::DescriptorHandle counters;
    std::uint32_t part = 0;
    std::uint32_t flags = 0;
};
static_assert(rhi::parameters_match<RasterParams>(lab_shaders::layout::sp16_raster,
                                                  {VEACH_PARAM(RasterParams, scene),
                                                   VEACH_PARAM(RasterParams, counters), VEACH_PARAM(RasterParams, part),
                                                   VEACH_PARAM(RasterParams, flags)}));

struct CullParams {
    rhi::DescriptorHandle frame;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle models;
    rhi::DescriptorHandle parts;
    rhi::DescriptorHandle counts;
    rhi::DescriptorHandle lists;
    rhi::DescriptorHandle args;
    rhi::DescriptorHandle hiz;
    rhi::DescriptorHandle depth;
    std::uint32_t instance_count = 0;
    std::uint32_t part_count = 0;
    std::array<std::uint32_t, 2> src_size{};
    std::array<std::uint32_t, 2> dst_size{};
    std::uint32_t src_offset = 0;
    std::uint32_t dst_offset = 0;
    std::uint32_t flags = 0;
    std::uint32_t pad = 0;
};
static_assert(rhi::parameters_match<CullParams>(
    lab_shaders::layout::sp16_cull,
    {VEACH_PARAM(CullParams, frame), VEACH_PARAM(CullParams, instances), VEACH_PARAM(CullParams, models),
     VEACH_PARAM(CullParams, parts), VEACH_PARAM(CullParams, counts), VEACH_PARAM(CullParams, lists),
     VEACH_PARAM(CullParams, args), VEACH_PARAM(CullParams, hiz), VEACH_PARAM(CullParams, depth),
     VEACH_PARAM(CullParams, instance_count), VEACH_PARAM(CullParams, part_count), VEACH_PARAM(CullParams, src_size),
     VEACH_PARAM(CullParams, dst_size), VEACH_PARAM(CullParams, src_offset), VEACH_PARAM(CullParams, dst_offset),
     VEACH_PARAM(CullParams, flags), VEACH_PARAM(CullParams, pad)}));

struct alignas(16) ShadeParams {
    SceneHandles scene;
    rhi::DescriptorHandle visibility;
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle output;
    rhi::DescriptorHandle counters;
    std::array<std::uint32_t, 2> size{};
    std::uint32_t flags = 0;
    std::uint32_t pad = 0;
};
static_assert(rhi::parameters_match<ShadeParams>(lab_shaders::layout::sp16_shade,
                                                 {VEACH_PARAM(ShadeParams, scene), VEACH_PARAM(ShadeParams, visibility),
                                                  VEACH_PARAM(ShadeParams, radiance), VEACH_PARAM(ShadeParams, output),
                                                  VEACH_PARAM(ShadeParams, counters), VEACH_PARAM(ShadeParams, size),
                                                  VEACH_PARAM(ShadeParams, flags), VEACH_PARAM(ShadeParams, pad)}));

// ---- the field --------------------------------------------------------------------------------------------------

constexpr float field_half = 256.0f;

enum PartId : std::uint32_t { terrain, rock_a, rock_b, trunk, canopy, bush, grass, part_count };
enum ModelId : std::uint32_t { m_terrain, m_rock_a, m_rock_b, m_tree, m_bush, m_grass, model_count };
enum MaterialId : std::uint32_t { mat_ground, mat_rock, mat_bark, mat_leaves, mat_bush, mat_grass, material_count };

struct MeshBuilder {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::array<Part, part_count> parts{};
    std::uint32_t current = 0;

    void begin(PartId id, MaterialId material, bool masked) {
        current = id;
        parts[id].first_index = static_cast<std::uint32_t>(indices.size());
        parts[id].material = material;
        parts[id].flags = masked ? part_masked : 0;
    }
    std::uint32_t vertex(math::vec3 p, math::vec3 n, math::vec2 uv) {
        vertices.push_back({math::vec4(p, uv.x), math::vec4(math::normalize(n), uv.y)});
        return static_cast<std::uint32_t>(vertices.size() - 1);
    }
    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        indices.insert(indices.end(), {a, b, c});
        parts[current].index_count += 3;
    }
    /// A card: corners counter-clockwise seen from the side `n` points to.
    void card(math::vec3 center, math::vec3 t, math::vec3 b, std::array<math::vec3, 4> normals) {
        const std::uint32_t v0 = vertex(center - t - b, normals[0], {0.0f, 1.0f});
        const std::uint32_t v1 = vertex(center + t - b, normals[1], {1.0f, 1.0f});
        const std::uint32_t v2 = vertex(center + t + b, normals[2], {1.0f, 0.0f});
        const std::uint32_t v3 = vertex(center - t + b, normals[3], {0.0f, 0.0f});
        triangle(v0, v1, v2);
        triangle(v0, v2, v3);
    }
};

void build_terrain(MeshBuilder& m) {
    m.begin(terrain, mat_ground, false);
    constexpr std::uint32_t n = 256;
    const float step = 2.0f * field_half / static_cast<float>(n);
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    for (std::uint32_t j = 0; j <= n; ++j) {
        for (std::uint32_t i = 0; i <= n; ++i) {
            const float x = -field_half + step * static_cast<float>(i);
            const float z = -field_half + step * static_cast<float>(j);
            const float e = 0.5f;
            const math::vec3 normal(terrain_height(x - e, z) - terrain_height(x + e, z), 2.0f * e,
                                    terrain_height(x, z - e) - terrain_height(x, z + e));
            m.vertex({x, terrain_height(x, z), z}, normal, {0.0f, 0.0f});
        }
    }
    for (std::uint32_t j = 0; j < n; ++j) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t a = base + j * (n + 1) + i;
            m.triangle(a, a + n + 1, a + 1);  // counter-clockwise from above
            m.triangle(a + 1, a + n + 1, a + n + 2);
        }
    }
}

void build_rock(MeshBuilder& m, PartId id, int subdivisions, std::uint32_t seed) {
    m.begin(id, mat_rock, false);
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<math::vec3> pos = {{-1, t, 0},  {1, t, 0},  {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
                                   {0, -1, -t}, {0, 1, -t}, {t, 0, -1},  {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<std::uint32_t, 3>> tris = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                                      {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                                      {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                                      {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    for (math::vec3& p : pos) {
        p = math::normalize(p);
    }
    for (int s = 0; s < subdivisions; ++s) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            if (const auto it = mid.find(key); it != mid.end()) {
                return it->second;
            }
            pos.push_back(math::normalize(pos[a] + pos[b]));
            const auto index = static_cast<std::uint32_t>(pos.size() - 1);
            mid.emplace(key, index);
            return index;
        };
        std::vector<std::array<std::uint32_t, 3>> next;
        for (const auto& tri : tris) {
            const std::uint32_t ab = midpoint(tri[0], tri[1]);
            const std::uint32_t bc = midpoint(tri[1], tri[2]);
            const std::uint32_t ca = midpoint(tri[2], tri[0]);
            next.push_back({tri[0], ab, ca});
            next.push_back({tri[1], bc, ab});
            next.push_back({tri[2], ca, bc});
            next.push_back({ab, bc, ca});
        }
        tris = std::move(next);
    }
    for (math::vec3& p : pos) {
        const float r = 0.75f + 0.5f * fbm(p.x * 1.7f + 5.0f, p.z * 1.7f + p.y * 1.3f, seed, 4);
        p *= r;
        p.y = p.y * 0.65f + 0.25f;
    }
    std::vector<math::vec3> normals(pos.size(), math::vec3(0.0f));
    for (const auto& tri : tris) {
        const math::vec3 n = math::cross(pos[tri[1]] - pos[tri[0]], pos[tri[2]] - pos[tri[0]]);
        for (const std::uint32_t v : tri) {
            normals[v] += n;
        }
    }
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    for (std::size_t i = 0; i < pos.size(); ++i) {
        m.vertex(pos[i], normals[i], {(pos[i].x + pos[i].z) * 0.5f, pos[i].y * 0.5f});
    }
    for (const auto& tri : tris) {
        m.triangle(base + tri[0], base + tri[1], base + tri[2]);
    }
}

void build_trunk(MeshBuilder& m) {
    m.begin(trunk, mat_bark, false);
    constexpr std::uint32_t sides = 10;
    constexpr std::uint32_t rings = 6;
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float t = static_cast<float>(r) / static_cast<float>(rings);
        const float radius = 0.30f * (1.0f - t) + 0.10f * t;
        for (std::uint32_t s = 0; s <= sides; ++s) {
            const float a = 6.2831853f * static_cast<float>(s) / static_cast<float>(sides);
            const math::vec3 dir(std::cos(a), 0.0f, -std::sin(a));
            m.vertex(dir * radius + math::vec3(0.0f, 7.5f * t - 0.2f, 0.0f), dir + math::vec3(0.0f, 0.03f, 0.0f),
                     {2.0f * static_cast<float>(s) / static_cast<float>(sides), 4.0f * t});
        }
    }
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < sides; ++s) {
            const std::uint32_t a = base + r * (sides + 1) + s;
            const std::uint32_t b = a + sides + 1;
            m.triangle(a, a + 1, b + 1);
            m.triangle(a, b + 1, b);
        }
    }
}

/// Cards scattered over an ellipsoid, each facing outward, normals bent radially (the usual foliage trick).
void build_cards(MeshBuilder& m, PartId id, MaterialId material, std::uint32_t count, math::vec3 center,
                 math::vec3 radii, float card_size, std::uint32_t seed) {
    m.begin(id, material, true);
    for (std::uint32_t i = 0; i < count; ++i) {
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float ring = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = static_cast<float>(i) * 2.39996323f;  // golden angle
        const math::vec3 dir(ring * std::cos(phi), y, ring * std::sin(phi));
        const float depth = 0.55f + 0.4f * unit(hash(seed + i * 7u + 1u));
        const math::vec3 c = center + dir * radii * depth;
        math::vec3 facing = math::normalize(dir + 0.6f * math::vec3(unit(hash(seed + i * 7u + 2u)) - 0.5f,
                                                                    unit(hash(seed + i * 7u + 3u)) - 0.5f,
                                                                    unit(hash(seed + i * 7u + 4u)) - 0.5f));
        const math::vec3 helper = std::abs(facing.y) < 0.9f ? math::vec3(0, 1, 0) : math::vec3(1, 0, 0);
        math::vec3 t = math::normalize(math::cross(helper, facing));
        math::vec3 b = math::cross(facing, t);
        const float roll = 6.2831853f * unit(hash(seed + i * 7u + 5u));
        const math::vec3 tr = t * std::cos(roll) + b * std::sin(roll);
        const math::vec3 br = math::cross(facing, tr);
        const float s = card_size * (0.8f + 0.4f * unit(hash(seed + i * 7u + 6u))) * 0.5f;
        const std::array<math::vec3, 4> corners{c - tr * s - br * s, c + tr * s - br * s, c + tr * s + br * s,
                                                c - tr * s + br * s};
        std::array<math::vec3, 4> normals{};
        for (std::size_t k = 0; k < 4; ++k) {
            normals[k] = math::normalize(corners[k] - center + math::vec3(0.0f, 0.3f * radii.y, 0.0f));
        }
        m.card(c, tr * s, br * s, normals);
    }
}

/// Three crossed cards bent outward toward the top, two segments high, normals mostly up.
void build_grass(MeshBuilder& m) {
    m.begin(grass, mat_grass, true);
    constexpr float width = 1.3f;
    constexpr float height = 0.9f;
    constexpr float bend = 0.18f;
    for (std::uint32_t k = 0; k < 3; ++k) {
        const float a = 3.14159265f * (static_cast<float>(k) / 3.0f + 0.1f * static_cast<float>(k));
        const math::vec3 dir(std::cos(a), 0.0f, std::sin(a));
        const math::vec3 side(-dir.z, 0.0f, dir.x);
        std::array<std::uint32_t, 6> v{};
        for (std::uint32_t row = 0; row <= 2; ++row) {
            const float t = static_cast<float>(row) / 2.0f;
            const math::vec3 up(0.0f, height * t, 0.0f);
            const math::vec3 offset = side * (bend * t * t);
            const math::vec3 normal = math::normalize(math::vec3(0.0f, 1.0f, 0.0f) + side * 0.35f);
            v[row * 2 + 0] = m.vertex(up + offset - dir * (width * 0.5f), normal, {0.0f, 1.0f - t});
            v[row * 2 + 1] = m.vertex(up + offset + dir * (width * 0.5f), normal, {1.0f, 1.0f - t});
        }
        for (std::uint32_t row = 0; row < 2; ++row) {
            m.triangle(v[row * 2], v[row * 2 + 1], v[row * 2 + 3]);
            m.triangle(v[row * 2], v[row * 2 + 3], v[row * 2 + 2]);
        }
    }
}

struct Field {
    MeshBuilder mesh;
    std::array<Model, model_count> models{};
    std::vector<Instance> instances;
    std::array<math::vec4, part_count> part_spheres{};  ///< object-space bounds of each model
    std::uint32_t list_size = 0;
};

math::vec4 bounds_of(const MeshBuilder& m, std::initializer_list<PartId> parts) {
    math::vec3 lo(1e30f);
    math::vec3 hi(-1e30f);
    for (const PartId p : parts) {
        for (std::uint32_t i = 0; i < m.parts[p].index_count; ++i) {
            const math::vec3 v(m.vertices[m.indices[m.parts[p].first_index + i]].position_u);
            lo = math::min(lo, v);
            hi = math::max(hi, v);
        }
    }
    const math::vec3 c = 0.5f * (lo + hi);
    return {c, math::length(hi - c)};
}

Field build_field(const Config& config) {
    Field f;
    build_terrain(f.mesh);
    build_rock(f.mesh, rock_a, 3, 51u);
    build_rock(f.mesh, rock_b, 2, 52u);
    build_trunk(f.mesh);
    build_cards(f.mesh, canopy, mat_leaves, 72, {0.0f, 6.6f, 0.0f}, {2.9f, 2.6f, 2.9f}, 1.9f, 61u);
    build_cards(f.mesh, bush, mat_bush, 30, {0.0f, 0.55f, 0.0f}, {0.95f, 0.6f, 0.95f}, 1.1f, 71u);
    build_grass(f.mesh);

    f.models[m_terrain] = {terrain, 1, 1e30f, 0};
    f.models[m_rock_a] = {rock_a, 1, 1e30f, 0};
    f.models[m_rock_b] = {rock_b, 1, 1e30f, 0};
    f.models[m_tree] = {trunk, 2, 1e30f, 0};
    f.models[m_bush] = {bush, 1, 400.0f, 0};
    f.models[m_grass] = {grass, 1, config.grass_range, 0};
    const std::array<math::vec4, model_count> bounds{bounds_of(f.mesh, {terrain}), bounds_of(f.mesh, {rock_a}),
                                                     bounds_of(f.mesh, {rock_b}),  bounds_of(f.mesh, {trunk, canopy}),
                                                     bounds_of(f.mesh, {bush}),    bounds_of(f.mesh, {grass})};

    const auto place = [&](ModelId model, float x, float z, float yaw, float scale, float sink) {
        const float c = std::cos(yaw) * scale;
        const float s = std::sin(yaw) * scale;
        const float y = terrain_height(x, z) - sink;
        Instance inst;
        inst.rows = {math::vec4(c, 0.0f, s, x), math::vec4(0.0f, scale, 0.0f, y), math::vec4(-s, 0.0f, c, z)};
        const math::vec3 local(bounds[model]);
        inst.sphere = math::vec4(math::vec3(math::dot(inst.rows[0], math::vec4(local, 1.0f)),
                                            math::dot(inst.rows[1], math::vec4(local, 1.0f)),
                                            math::dot(inst.rows[2], math::vec4(local, 1.0f))),
                                 bounds[model].w * scale);
        inst.model = model;
        f.instances.push_back(inst);
    };
    {
        Instance t;
        t.rows = {math::vec4(1, 0, 0, 0), math::vec4(0, 1, 0, 0), math::vec4(0, 0, 1, 0)};
        t.sphere = bounds[m_terrain];
        t.model = m_terrain;
        f.instances.push_back(t);
    }
    const auto scatter = [&](ModelId model, float spacing, std::uint32_t seed, const auto& density, float smin,
                             float smax, float sink) {
        const auto n = static_cast<std::int32_t>(2.0f * field_half / spacing);
        for (std::int32_t j = 0; j < n; ++j) {
            for (std::int32_t i = 0; i < n; ++i) {
                const std::uint32_t h = hash(i, j, seed);
                const float x = -field_half + (static_cast<float>(i) + unit(hash(h + 1u))) * spacing;
                const float z = -field_half + (static_cast<float>(j) + unit(hash(h + 2u))) * spacing;
                if (unit(hash(h + 3u)) >= density(x, z)) {
                    continue;
                }
                const float scale = smin + (smax - smin) * unit(hash(h + 4u));
                place(model, x, z, 6.2831853f * unit(hash(h + 5u)), scale, sink * scale);
            }
        }
    };
    const auto forest = [](float x, float z) {
        return std::clamp((fbm(x / 110.0f + 3.0f, z / 110.0f, 81u, 3) - 0.52f) * 5.0f, 0.0f, 1.0f);
    };
    scatter(m_tree, 6.0f, 101u, [&](float x, float z) { return 0.02f + 0.8f * forest(x, z); }, 0.8f, 1.35f, 0.1f);
    scatter(m_bush, 4.0f, 102u, [&](float x, float z) { return 0.15f + 0.4f * forest(x, z); }, 0.7f, 1.4f, 0.1f);
    scatter(m_rock_a, 9.0f, 103u, [](float, float) { return 0.35f; }, 0.4f, 2.4f, 0.25f);
    scatter(m_rock_b, 6.0f, 104u, [](float, float) { return 0.3f; }, 0.2f, 0.8f, 0.2f);
    scatter(m_grass, 0.7f, 105u, [&](float x, float z) { return 0.95f - 0.6f * forest(x, z); }, 0.7f, 1.3f, 0.03f);

    std::array<std::uint32_t, part_count> capacity{};
    for (const Instance& inst : f.instances) {
        const Model& model = f.models[inst.model];
        for (std::uint32_t p = 0; p < model.part_count; ++p) {
            ++capacity[model.first_part + p];
        }
    }
    for (std::uint32_t p = 0; p < part_count; ++p) {
        f.mesh.parts[p].list_offset = f.list_size;
        f.mesh.parts[p].capacity = capacity[p];
        f.list_size += capacity[p];
    }
    return f;
}

struct View {
    math::vec3 eye{0.0f};
    float yaw = 0.0f;    ///< radians, 0 looks down -Z
    float pitch = 0.0f;  ///< radians, negative looks down
};

View view_named(const std::string& name) {
    if (name == "mid") {
        return {{8.0f, terrain_height(8.0f, 150.0f) + 18.0f, 150.0f}, 0.12f, -0.30f};
    }
    if (name == "far") {
        return {{0.0f, terrain_height(0.0f, 250.0f) + 90.0f, 250.0f}, 0.0f, -0.36f};
    }
    return {{4.0f, terrain_height(4.0f, 70.0f) + 1.0f, 70.0f}, 0.1f, -0.06f};
}

// ---- the feature ------------------------------------------------------------------------------------------------

constexpr rhi::Format hdr_format = rhi::Format::rgba16_float;
constexpr std::uint32_t args_stride = sizeof(rhi::DrawIndexedIndirectArgs);
constexpr std::uint32_t texture_size = 512;

class Sp16Feature final : public render::RenderFeature {
public:
    explicit Sp16Feature(Config config) : config_(std::move(config)) {}

    [[nodiscard]] std::string_view name() const override { return "lab.sp16"; }

    [[nodiscard]] veach::Result<> prepare(rhi::Device& device) override {
        field_ = std::make_unique<Field>(build_field(config_));
        std::size_t triangles = 0;
        std::array<std::size_t, part_count> per_part{};
        for (const Instance& inst : field_->instances) {
            const Model& model = field_->models[inst.model];
            for (std::uint32_t p = 0; p < model.part_count; ++p) {
                per_part[model.first_part + p] += field_->mesh.parts[model.first_part + p].index_count / 3;
            }
        }
        for (const std::size_t t : per_part) {
            triangles += t;
        }
        veach::log::info("sp16",
                         "field: {} instances, {} triangles (terrain {}, rocks {}, trunks {}, canopies {}, bushes {}, "
                         "grass {}); path {}, {}x{}, view {}, {} lights, HiZ {}",
                         field_->instances.size(), triangles, per_part[terrain], per_part[rock_a] + per_part[rock_b],
                         per_part[trunk], per_part[canopy], per_part[bush], per_part[grass], path_name(config_),
                         config_.width, config_.height, config_.view, config_.lights, config_.hiz ? "on" : "off");

        if (auto r = make_textures(device); !r) {
            return r;
        }
        if (auto r = make_buffers(device); !r) {
            return r;
        }
        return make_pipelines(device);
    }

    void record(render::RenderGraph& graph, const render::FrameContext& frame) override;

private:
    struct TextureSet {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    veach::Result<TextureSet> upload_mips(rhi::Device& device, const std::vector<Image>& mips, bool srgb,
                                          std::string_view name) {
        TextureSet set{static_cast<std::uint32_t>(table_.size()), static_cast<std::uint32_t>(mips.size())};
        for (std::size_t level = 0; level < mips.size(); ++level) {
            const std::vector<std::byte> bytes = to_rgba8(mips[level], srgb);
            const std::array<std::span<const std::byte>, 1> data{bytes};
            rhi::TextureDesc desc;
            desc.width = mips[level].size;
            desc.height = mips[level].size;
            desc.format = srgb ? rhi::Format::rgba8_srgb : rhi::Format::rgba8_unorm;
            desc.usage = rhi::TextureUsage::sampled;
            desc.debug_name = std::format("sp16 {} mip {}", name, level);
            auto texture = device.create_texture(desc, data);
            if (!texture) {
                return std::unexpected(texture.error());
            }
            textures_.push_back(*texture);
            table_.push_back(device.descriptor(*texture));
        }
        return set;
    }

    veach::Result<> make_textures(rhi::Device& device) {
        struct Source {
            MaterialId material;
            Image image;
            bool masked;
            float bump;
        };
        std::vector<Source> sources;
        sources.push_back({mat_ground, ground_texture(texture_size), false, 2.0f});
        sources.push_back({mat_rock, rock_texture(texture_size), false, 3.0f});
        sources.push_back({mat_bark, bark_texture(texture_size), false, 3.0f});
        sources.push_back({mat_leaves, leaves_texture(texture_size, 900u), true, 1.0f});
        sources.push_back({mat_bush, leaves_texture(texture_size, 1900u), true, 1.0f});
        sources.push_back({mat_grass, grass_texture(texture_size), true, 1.0f});
        materials_.resize(material_count);
        for (Source& s : sources) {
            Image normals = normal_map(s.image, s.bump);
            if (s.masked) {
                dilate(s.image, 12);
            }
            auto base = upload_mips(device, mip_chain(s.image, s.masked), true, "base color");
            auto bump = upload_mips(device, mip_chain(normals, false), false, "normals");
            if (!base || !bump) {
                return std::unexpected(!base ? base.error() : bump.error());
            }
            Material& m = materials_[s.material];
            m.base_mips = base->first;
            m.normal_mips = bump->first;
            m.mip_count = base->count;
            m.texture_size = static_cast<float>(texture_size);
            m.tint = {1.0f, 1.0f, 1.0f, 0.5f};
        }
        materials_[mat_ground].flags = material_world_uv;
        materials_[mat_ground].uv_scale = 0.25f;
        materials_[mat_ground].roughness = 0.95f;
        materials_[mat_ground].normal_strength = 0.4f;
        materials_[mat_rock].roughness = 0.7f;
        materials_[mat_rock].normal_strength = 0.6f;
        materials_[mat_bark].roughness = 0.85f;
        materials_[mat_bark].normal_strength = 0.6f;
        for (const MaterialId m : {mat_leaves, mat_bush, mat_grass}) {
            materials_[m].flags = material_foliage;
            materials_[m].roughness = 0.8f;
            materials_[m].translucency = 0.6f;
            materials_[m].normal_strength = 0.25f;
        }
        materials_[mat_bush].tint = {0.8f, 0.9f, 0.7f, 0.5f};
        materials_[mat_grass].flags |= material_ground_ao;
        auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::repeat});
        if (!sampler) {
            return std::unexpected(sampler.error());
        }
        sampler_ = *sampler;
        auto clamped = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::clamp});
        if (!clamped) {
            return std::unexpected(clamped.error());
        }
        clamp_sampler_ = *clamped;
        return {};
    }

    template <class T>
    veach::Result<rhi::BufferHandle> buffer(rhi::Device& device, std::span<const T> data, rhi::BufferUsage usage,
                                            std::string name) {
        auto b = device.create_buffer({std::max<std::uint64_t>(data.size_bytes(), 16), usage, rhi::MemoryType::gpu,
                                       std::move(name), static_cast<std::uint32_t>(sizeof(T))},
                                      std::as_bytes(data));
        if (b) {
            owned_.push_back(*b);
        }
        return b;
    }

    veach::Result<> make_buffers(rhi::Device& device) {
        using rhi::BufferUsage;
        auto vertices =
            buffer(device, std::span<const Vertex>(field_->mesh.vertices), BufferUsage::storage, "sp16 vertices");
        auto indices = buffer(device, std::span<const std::uint32_t>(field_->mesh.indices),
                              BufferUsage::storage | BufferUsage::index, "sp16 indices");
        auto instances =
            buffer(device, std::span<const Instance>(field_->instances), BufferUsage::storage, "sp16 instances");
        auto models = buffer(device, std::span<const Model>(field_->models), BufferUsage::storage, "sp16 models");
        auto parts = buffer(device, std::span<const Part>(field_->mesh.parts), BufferUsage::storage, "sp16 parts");
        auto materials = buffer(device, std::span<const Material>(materials_), BufferUsage::storage, "sp16 materials");
        auto table =
            buffer(device, std::span<const rhi::DescriptorHandle>(table_), BufferUsage::storage, "sp16 textures");
        std::vector<Light> lights = make_lights();
        auto light_buffer = buffer(device, std::span<const Light>(lights), BufferUsage::storage, "sp16 lights");
        const std::vector<std::uint32_t> zeros_lists(field_->list_size, 0);
        auto lists = buffer(device, std::span<const std::uint32_t>(zeros_lists), BufferUsage::storage, "sp16 lists");
        const std::vector<std::uint32_t> zeros_counts(part_count, 0);
        auto counts = buffer(device, std::span<const std::uint32_t>(zeros_counts), BufferUsage::storage, "sp16 counts");
        const std::vector<std::uint32_t> zeros_args(part_count * 5, 0);
        auto args = buffer(device, std::span<const std::uint32_t>(zeros_args),
                           BufferUsage::storage | BufferUsage::indirect | BufferUsage::copy_src, "sp16 args");
        const std::vector<std::uint32_t> zeros_counters(8, 0);
        auto counters = buffer(device, std::span<const std::uint32_t>(zeros_counters),
                               BufferUsage::storage | BufferUsage::copy_src, "sp16 counters");
        // HiZ: every level for the largest size this run uses.
        std::uint32_t w = (config_.width * config_.ssaa + 1) / 2;
        std::uint32_t h = (config_.height * config_.ssaa + 1) / 2;
        std::uint32_t total = 0;
        hiz_levels_.clear();
        while (true) {
            hiz_levels_.push_back({w, h, total});
            total += w * h;
            if ((w == 1 && h == 1) || hiz_levels_.size() == 16) {
                break;
            }
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        const std::vector<float> zeros_hiz(total, 0.0f);
        auto hiz = buffer(device, std::span<const float>(zeros_hiz), BufferUsage::storage, "sp16 hiz");
        for (auto* r : {&vertices, &indices, &instances, &models, &parts, &materials, &table, &light_buffer, &lists,
                        &counts, &args, &counters, &hiz}) {
            if (!*r) {
                return std::unexpected(r->error());
            }
        }
        vertices_ = *vertices;
        indices_ = *indices;
        instances_ = *instances;
        models_ = *models;
        parts_ = *parts;
        materials_buffer_ = *materials;
        table_buffer_ = *table;
        lights_ = *light_buffer;
        lists_ = *lists;
        counts_ = *counts;
        args_ = *args;
        counters_ = *counters;
        hiz_ = *hiz;
        for (std::uint32_t i = 0; i < device.frames_in_flight(); ++i) {
            auto b = device.create_buffer({sizeof(FrameData), BufferUsage::storage, rhi::MemoryType::upload,
                                           std::format("sp16 frame {}", i), sizeof(FrameData)});
            if (!b) {
                return std::unexpected(b.error());
            }
            frame_buffers_.push_back(*b);
            owned_.push_back(*b);
        }
        return {};
    }

    std::vector<Light> make_lights() const {
        std::vector<Light> lights;
        const View v = view_named(config_.view);
        const math::vec3 forward(-std::sin(v.yaw), 0.0f, -std::cos(v.yaw));
        const math::vec3 right(std::cos(v.yaw), 0.0f, -std::sin(v.yaw));
        const float spread = config_.view == "far" ? 120.0f : (config_.view == "mid" ? 50.0f : 25.0f);
        for (std::uint32_t i = 0; i < 64; ++i) {
            const float along = spread * (0.2f + 1.6f * unit(hash(700u + i)));
            const float across = spread * (unit(hash(800u + i)) - 0.5f) * 1.6f;
            const math::vec3 p = v.eye + forward * along + right * across;
            const float y = terrain_height(p.x, p.z) + 1.5f;
            lights.push_back({math::vec4(p.x, y, p.z, spread * 0.6f), math::vec4(4000.0f, 2600.0f, 1500.0f, 0.0f)});
        }
        return lights;
    }

    veach::Result<rhi::PipelineHandle> graphics(rhi::Device& device, std::string_view vs, std::string_view fs,
                                                rhi::Format color, bool masked, rhi::CompareOp compare,
                                                bool depth_write, bool a2c, std::uint32_t samples) {
        rhi::GraphicsPipelineDesc d;
        d.library = &lab_shaders::sp16_raster();
        d.vertex_entry = vs;
        d.fragment_entry = fs;
        d.color_formats = {color};
        d.depth_format = rhi::Format::depth32_float;
        d.depth_test = true;
        d.depth_write = depth_write;
        d.depth_compare = compare;
        d.cull = masked ? rhi::CullMode::none : rhi::CullMode::back;
        d.sample_count = samples;
        d.alpha_to_coverage = a2c;
        d.debug_name = std::format("sp16 {} {}", vs, fs);
        auto p = device.create_graphics_pipeline(d);
        if (p) {
            pipelines_.push_back(*p);
        }
        return p;
    }

    veach::Result<rhi::PipelineHandle> compute(rhi::Device& device, const rhi::ShaderLibrary& lib,
                                               std::string_view entry, std::array<std::uint32_t, 3> group) {
        auto p = device.create_compute_pipeline({&lib, entry, group, std::format("sp16 {}", entry)});
        if (p) {
            pipelines_.push_back(*p);
        }
        return p;
    }

    veach::Result<> make_pipelines(rhi::Device& device) {
        using rhi::CompareOp;
        const std::uint32_t s = config_.msaa;
        const bool a2c = s > 1;
        std::vector<std::pair<rhi::PipelineHandle*, veach::Result<rhi::PipelineHandle>>> made;
        made.emplace_back(&cull_, compute(device, lab_shaders::sp16_cull(), "cull", {64, 1, 1}));
        made.emplace_back(&write_args_, compute(device, lab_shaders::sp16_cull(), "write_args", {64, 1, 1}));
        made.emplace_back(&hiz_first_, compute(device, lab_shaders::sp16_cull(), "hiz_first", {8, 8, 1}));
        made.emplace_back(&hiz_down_, compute(device, lab_shaders::sp16_cull(), "hiz_down", {8, 8, 1}));
        made.emplace_back(&display_, compute(device, lab_shaders::sp16_shade(), "display", {8, 8, 1}));
        if (config_.path == Path::vis || config_.stats) {
            made.emplace_back(&shade_, compute(device, lab_shaders::sp16_shade(), "shade", {8, 8, 1}));
            made.emplace_back(&quad_stats_, compute(device, lab_shaders::sp16_shade(), "quad_stats", {8, 8, 1}));
            made.emplace_back(&vis_opaque_, graphics(device, "vs_vis", "vis_opaque_fs", rhi::Format::rg32_uint, false,
                                                     CompareOp::greater_equal, true, false, 1));
            made.emplace_back(&vis_masked_, graphics(device, "vs_vis", "vis_masked_fs", rhi::Format::rg32_uint, true,
                                                     CompareOp::greater_equal, true, false, 1));
            made.emplace_back(&count_, graphics(device, "vs_vis", "count_fs", rhi::Format::rg32_uint, true,
                                                CompareOp::greater_equal, true, false, 1));
        }
        if (config_.path != Path::vis) {
            made.emplace_back(&fwd_opaque_, graphics(device, "vs_forward", "forward_fs", hdr_format, false,
                                                     CompareOp::greater_equal, true, false, s));
            made.emplace_back(&fwd_masked_, graphics(device, "vs_forward", a2c ? "forward_a2c_fs" : "forward_masked_fs",
                                                     hdr_format, true, CompareOp::greater_equal, true, a2c, s));
            made.emplace_back(&pre_opaque_, graphics(device, "vs_vis", "prepass_fs", hdr_format, false,
                                                     CompareOp::greater_equal, true, false, s));
            made.emplace_back(&pre_masked_, graphics(device, "vs_vis", a2c ? "prepass_a2c_fs" : "prepass_masked_fs",
                                                     hdr_format, true, CompareOp::greater_equal, true, a2c, s));
            made.emplace_back(&eq_opaque_, graphics(device, "vs_forward", "forward_fs", hdr_format, false,
                                                    CompareOp::equal, false, false, s));
            made.emplace_back(&eq_masked_, graphics(device, "vs_forward", "forward_fs", hdr_format, true,
                                                    CompareOp::equal, false, false, s));
            rhi::GraphicsPipelineDesc sky;
            sky.library = &lab_shaders::sp16_raster();
            sky.vertex_entry = "sky_vs";
            sky.fragment_entry = "sky_fs";
            sky.color_formats = {hdr_format};
            sky.depth_format = rhi::Format::depth32_float;
            sky.depth_test = true;
            sky.depth_compare = CompareOp::greater_equal;
            sky.sample_count = s;
            sky.debug_name = "sp16 sky";
            made.emplace_back(&sky_, device.create_graphics_pipeline(sky));
        }
        for (auto& [slot, result] : made) {
            if (!result) {
                return std::unexpected(result.error());
            }
            *slot = *result;
        }
        return {};
    }

    SceneHandles scene_handles(rhi::Device& device, rhi::BufferHandle frame_buffer) const {
        return {device.descriptor(frame_buffer),      device.descriptor(vertices_),     device.descriptor(indices_),
                device.descriptor(instances_),        device.descriptor(parts_),        device.descriptor(lists_),
                device.descriptor(materials_buffer_), device.descriptor(table_buffer_), device.descriptor(sampler_),
                device.descriptor(clamp_sampler_),    device.descriptor(lights_)};
    }

    FrameData frame_data(std::uint32_t width, std::uint32_t height);
    void collect_timings(rhi::Device& device);

    Config config_;
    std::unique_ptr<Field> field_;
    std::vector<Material> materials_;
    std::vector<rhi::TextureHandle> textures_;
    std::vector<rhi::DescriptorHandle> table_;
    std::vector<rhi::BufferHandle> owned_;
    std::vector<rhi::PipelineHandle> pipelines_;
    std::vector<rhi::BufferHandle> frame_buffers_;
    rhi::SamplerHandle sampler_;
    rhi::SamplerHandle clamp_sampler_;

    rhi::BufferHandle vertices_, indices_, instances_, models_, parts_, materials_buffer_, table_buffer_, lights_,
        lists_, counts_, args_, counters_, hiz_;
    struct HizLevel {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t offset = 0;
    };
    std::vector<HizLevel> hiz_levels_;

    rhi::PipelineHandle cull_, write_args_, hiz_first_, hiz_down_, display_, shade_, quad_stats_, vis_opaque_,
        vis_masked_, count_, fwd_opaque_, fwd_masked_, pre_opaque_, pre_masked_, eq_opaque_, eq_masked_, sky_;

    std::uint64_t frames_ = 0;
    bool hiz_valid_ = false;
    math::mat4 previous_view_projection_{1.0f};
    std::uint64_t last_timed_frame_ = 0;
    std::map<std::string, std::vector<double>> samples_;
    std::vector<double> totals_;
    bool timings_reported_ = false;
    bool stats_requested_ = false;
};

FrameData Sp16Feature::frame_data(std::uint32_t width, std::uint32_t height) {
    View v = view_named(config_.view);
    v.yaw += math::radians(config_.pan) * static_cast<float>(frames_);
    const math::vec3 forward(-std::sin(v.yaw) * std::cos(v.pitch), std::sin(v.pitch),
                             -std::cos(v.yaw) * std::cos(v.pitch));
    const math::mat4 view = math::lookAt(v.eye, v.eye + forward, math::vec3(0.0f, 1.0f, 0.0f));
    const float near_plane = 0.1f;
    const math::mat4 projection = math::perspective_reverse_z(
        math::radians(60.0f), static_cast<float>(width) / static_cast<float>(height), near_plane);
    const math::mat4 vp = projection * view;
    FrameData f;
    f.view_projection = columns(vp);
    f.inverse_view_projection = columns(math::inverse(vp));
    f.previous_view_projection = columns(hiz_valid_ ? previous_view_projection_ : vp);
    const auto row = [&](int i) {
        return math::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]);
    };
    const std::array<math::vec4, 5> planes{row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1),
                                           row(3) - row(2)};
    for (std::size_t i = 0; i < planes.size(); ++i) {
        f.planes[i] = planes[i] / math::length(math::vec3(planes[i]));
    }
    const float ev100 = 14.3f;
    f.camera = math::vec4(v.eye, 1.0f / (1.2f * std::exp2(ev100)));
    f.sun_direction = math::vec4(math::normalize(math::vec3(-0.45f, 0.55f, -0.65f)), near_plane);
    f.sun_color = math::vec4(90000.0f * math::vec3(1.0f, 0.95f, 0.86f), 0.0f);
    f.sky_color = math::vec4(2600.0f, 4300.0f, 7600.0f, 0.0007f);
    f.horizon_color = math::vec4(6500.0f, 7200.0f, 8200.0f, 0.04f);
    f.ground_color = math::vec4(900.0f, 1050.0f, 650.0f, 0.0f);
    f.size = {width, height, config_.lights, 0};
    f.hiz = {hiz_levels_.front().width, hiz_levels_.front().height, static_cast<std::uint32_t>(hiz_levels_.size()),
             hiz_valid_ ? 1u : 0u};
    for (std::size_t i = 0; i < hiz_levels_.size(); ++i) {
        f.hiz_offsets[i] = hiz_levels_[i].offset;
    }
    previous_view_projection_ = vp;
    return f;
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::ranges::sort(v);
    return v[v.size() / 2];
}

void Sp16Feature::collect_timings(rhi::Device& device) {
    if (config_.timings == 0 || timings_reported_) {
        return;
    }
    const rhi::FrameTimings t = device.last_frame_timings();
    if (t.frame == 0 || t.frame == last_timed_frame_ || t.frame < config_.warmup) {
        return;
    }
    last_timed_frame_ = t.frame;
    double total = 0.0;
    std::map<std::string, double> frame;
    for (const rhi::PassTiming& p : t.passes) {
        if (p.name.starts_with("sp16 ")) {
            std::string key = p.name.starts_with("sp16 hiz") ? "sp16 hiz" : p.name;
            frame[key] += p.gpu_ms;
            total += p.gpu_ms;
        }
    }
    for (const auto& [name, ms] : frame) {
        samples_[name].push_back(ms);
    }
    totals_.push_back(total);
    if (totals_.size() < config_.timings) {
        return;
    }
    std::string passes;
    for (const auto& [name, values] : samples_) {
        passes += std::format("{}\"{}\": {:.4f}", passes.empty() ? "" : ", ", name.substr(5), median(values));
    }
    veach::log::info("sp16",
                     "SP16_TIMINGS {{\"path\": \"{}\", \"view\": \"{}\", \"size\": \"{}x{}\", \"lights\": {}, "
                     "\"hiz\": {}, \"frames\": {}, \"total\": {:.4f}, \"passes\": {{{}}}}}",
                     path_name(config_), config_.view, config_.width, config_.height, config_.lights,
                     config_.hiz ? "true" : "false", totals_.size(), median(totals_), passes);
    timings_reported_ = true;
}

void Sp16Feature::record(render::RenderGraph& graph, const render::FrameContext& frame) {
    rhi::Device& device = frame.device;
    collect_timings(device);
    const std::uint32_t out_width = frame.width;
    const std::uint32_t out_height = frame.height;
    const std::uint32_t width = frame.width * config_.ssaa;
    const std::uint32_t height = frame.height * config_.ssaa;
    const rhi::BufferHandle frame_buffer = frame_buffers_[device.current_frame().index % frame_buffers_.size()];
    const FrameData data = frame_data(width, height);
    std::memcpy(device.map(frame_buffer), &data, sizeof(data));
    const SceneHandles scene = scene_handles(device, frame_buffer);
    ++frames_;

    using rhi::ResourceState;
    const ResourceState was = frames_ == 1 ? ResourceState::undefined : ResourceState::shader_read;
    const auto imp = [&](rhi::BufferHandle b, const char* name, ResourceState current) {
        return graph.import(b, name, current, ResourceState::shader_read);
    };
    const render::GraphResource g_frame = imp(frame_buffer, "sp16 frame", ResourceState::shader_read);
    const render::GraphResource g_vertices = imp(vertices_, "sp16 vertices", ResourceState::shader_read);
    const render::GraphResource g_indices = graph.import(indices_, "sp16 indices", ResourceState::shader_read);
    const render::GraphResource g_instances = imp(instances_, "sp16 instances", ResourceState::shader_read);
    const render::GraphResource g_models = imp(models_, "sp16 models", ResourceState::shader_read);
    const render::GraphResource g_parts = imp(parts_, "sp16 parts", ResourceState::shader_read);
    const render::GraphResource g_materials = imp(materials_buffer_, "sp16 materials", ResourceState::shader_read);
    const render::GraphResource g_table = imp(table_buffer_, "sp16 textures", ResourceState::shader_read);
    const render::GraphResource g_lights = imp(lights_, "sp16 lights", ResourceState::shader_read);
    const render::GraphResource g_lists = imp(lists_, "sp16 lists", was);
    const render::GraphResource g_counts = imp(counts_, "sp16 counts", was);
    const render::GraphResource g_args =
        graph.import(args_, "sp16 args", frames_ == 1 ? ResourceState::undefined : ResourceState::shader_read,
                     ResourceState::shader_read);
    const render::GraphResource g_hiz = imp(hiz_, "sp16 hiz", was);
    const render::GraphResource g_counters = imp(counters_, "sp16 counters", was);

    const auto handles = [this, frame_buffer](render::PassContext& ctx) {
        CullParams p;
        p.frame = ctx.device.descriptor(frame_buffer);
        p.instances = ctx.device.descriptor(instances_);
        p.models = ctx.device.descriptor(models_);
        p.parts = ctx.device.descriptor(parts_);
        p.counts = ctx.device.descriptor(counts_, rhi::View::storage);
        p.lists = ctx.device.descriptor(lists_, rhi::View::storage);
        p.args = ctx.device.descriptor(args_, rhi::View::storage);
        p.hiz = ctx.device.descriptor(hiz_, rhi::View::storage);
        p.depth = ctx.device.null_descriptor(rhi::DescriptorKind::texture);
        p.instance_count = static_cast<std::uint32_t>(field_->instances.size());
        p.part_count = part_count;
        p.flags = config_.hiz ? 1u : 0u;
        return p;
    };

    // Culling: every instance, then one indexed indirect draw per part.
    graph.add_compute_pass(
        "sp16 cull",
        [&](render::PassBuilder& b) {
            for (const render::GraphResource r : {g_frame, g_instances, g_models, g_parts}) {
                b.read(r);
            }
            b.read(g_hiz);
            b.write(g_counts);
            b.write(g_lists);
        },
        [this, handles](render::PassContext& ctx) {
            CullParams p = handles(ctx);
            ctx.cmd.set_pipeline(cull_);
            ctx.cmd.set_parameters(p);
            ctx.cmd.dispatch((p.instance_count + 63) / 64, 1);
        });
    graph.add_compute_pass(
        "sp16 args",
        [&](render::PassBuilder& b) {
            b.read(g_parts);
            b.write(g_counts);
            b.write(g_args);
        },
        [this, handles](render::PassContext& ctx) {
            ctx.cmd.set_pipeline(write_args_);
            ctx.cmd.set_parameters(handles(ctx));
            ctx.cmd.dispatch(1, 1);
        });

    const auto declare_draw_inputs = [&](render::PassBuilder& b) {
        b.read(g_args, ResourceState::indirect_argument);
        b.read(g_indices, ResourceState::index_read);
        for (const render::GraphResource r :
             {g_frame, g_vertices, g_instances, g_parts, g_lists, g_materials, g_table, g_lights}) {
            b.read(r);
        }
    };
    const auto draw_parts = [this, scene](render::PassContext& ctx, rhi::PipelineHandle opaque,
                                          rhi::PipelineHandle masked, bool counting) {
        for (const bool want_masked : {false, true}) {
            ctx.cmd.set_pipeline(want_masked ? masked : opaque);
            for (std::uint32_t p = 0; p < part_count; ++p) {
                if (((field_->mesh.parts[p].flags & part_masked) != 0) != want_masked) {
                    continue;
                }
                RasterParams params{scene,
                                    counting ? ctx.device.descriptor(counters_, rhi::View::storage)
                                             : ctx.device.null_descriptor(rhi::DescriptorKind::storage_buffer),
                                    p, 0};
                ctx.cmd.set_parameters(params);
                ctx.cmd.draw_indexed_indirect(indices_, args_, std::uint64_t{p} * args_stride, 1, args_stride);
            }
        }
    };

    rhi::TextureDesc depth_desc;
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.format = rhi::Format::depth32_float;
    depth_desc.usage = rhi::TextureUsage::depth_target | rhi::TextureUsage::sampled;
    depth_desc.debug_name = "sp16 depth";
    const render::GraphResource depth = graph.create_texture(depth_desc);
    rhi::TextureDesc hdr_desc;
    hdr_desc.width = width;
    hdr_desc.height = height;
    hdr_desc.format = hdr_format;
    hdr_desc.usage = rhi::TextureUsage::color_target | rhi::TextureUsage::storage;
    hdr_desc.debug_name = "sp16 radiance";
    const render::GraphResource hdr = graph.create_texture(hdr_desc);
    render::GraphResource vis;
    const bool need_vis = config_.path == Path::vis || config_.stats;
    if (need_vis) {
        rhi::TextureDesc vis_desc;
        vis_desc.width = width;
        vis_desc.height = height;
        vis_desc.format = rhi::Format::rg32_uint;
        vis_desc.usage = rhi::TextureUsage::color_target | rhi::TextureUsage::sampled;
        vis_desc.debug_name = "sp16 visibility";
        vis = graph.create_texture(vis_desc);
    }

    const auto shade_params = [this, scene, width, height](render::PassContext& ctx, render::GraphResource v,
                                                           render::GraphResource radiance) {
        ShadeParams p;
        p.scene = scene;
        p.visibility = v.is_valid() ? ctx.device.descriptor(ctx.texture(v))
                                    : ctx.device.null_descriptor(rhi::DescriptorKind::texture);
        p.radiance = ctx.device.descriptor(ctx.texture(radiance), rhi::View::storage);
        p.output = ctx.device.null_descriptor(rhi::DescriptorKind::storage_texture);
        p.counters = ctx.device.descriptor(counters_, rhi::View::storage);
        p.size = {width, height};
        return p;
    };

    if (config_.path == Path::vis) {
        // A: ids of the visible triangles, opaque then masked, in one render pass.
        graph.add_graphics_pass(
            "sp16 visibility",
            [&](render::PassBuilder& b) {
                declare_draw_inputs(b);
                b.color_target_uint(vis, rhi::LoadOp::clear, {0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0});
                b.depth_target(depth, rhi::LoadOp::clear, 0.0f);
            },
            [this, draw_parts](render::PassContext& ctx) { draw_parts(ctx, vis_opaque_, vis_masked_, false); });
        graph.add_compute_pass(
            "sp16 shade",
            [&](render::PassBuilder& b) {
                b.read(vis);
                for (const render::GraphResource r :
                     {g_frame, g_vertices, g_indices, g_instances, g_parts, g_materials, g_table, g_lights}) {
                    b.read(r);
                }
                b.write(hdr);
            },
            [this, shade_params, vis, hdr, width, height](render::PassContext& ctx) {
                ctx.cmd.set_pipeline(shade_);
                ctx.cmd.set_parameters(shade_params(ctx, vis, hdr));
                ctx.cmd.dispatch((width + 7) / 8, (height + 7) / 8);
            });
    } else {
        // B: forward, in one render pass so depth (and, with MSAA, every sample) stays in tile memory.
        render::GraphResource color_target = hdr;
        render::GraphResource depth_target = depth;
        if (config_.msaa > 1) {
            rhi::TextureDesc ms = hdr_desc;
            ms.usage = rhi::TextureUsage::color_target;
            ms.sample_count = config_.msaa;
            ms.debug_name = "sp16 radiance msaa";
            color_target = graph.create_texture(ms);
            rhi::TextureDesc msd = depth_desc;
            msd.usage = rhi::TextureUsage::depth_target;
            msd.sample_count = config_.msaa;
            msd.debug_name = "sp16 depth msaa";
            depth_target = graph.create_texture(msd);
        }
        const bool prepass = config_.path == Path::prepass;
        graph.add_graphics_pass(
            "sp16 forward",
            [&](render::PassBuilder& b) {
                declare_draw_inputs(b);
                b.color_target(color_target, rhi::LoadOp::clear, {0.0f, 0.0f, 0.0f, 1.0f});
                b.depth_target(depth_target, rhi::LoadOp::clear, 0.0f);
                if (config_.msaa > 1) {
                    b.resolve(color_target, hdr);
                    b.resolve(depth_target, depth);
                }
            },
            [this, draw_parts, prepass, scene](render::PassContext& ctx) {
                if (prepass) {
                    draw_parts(ctx, pre_opaque_, pre_masked_, false);
                    draw_parts(ctx, eq_opaque_, eq_masked_, false);
                } else {
                    draw_parts(ctx, fwd_opaque_, fwd_masked_, false);
                }
                ctx.cmd.set_pipeline(sky_);
                ctx.cmd.set_parameters(
                    RasterParams{scene, ctx.device.null_descriptor(rhi::DescriptorKind::storage_buffer), 0, 0});
                ctx.cmd.draw(3);
            });
    }

    // HiZ for the next frame's occlusion test, from this frame's depth.
    if (config_.hiz) {
        for (std::size_t level = 0; level < hiz_levels_.size(); ++level) {
            const HizLevel dst = hiz_levels_[level];
            const HizLevel src = level == 0 ? HizLevel{width, height, 0} : hiz_levels_[level - 1];
            graph.add_compute_pass(
                std::format("sp16 hiz {}", level),
                [&](render::PassBuilder& b) {
                    if (level == 0) {
                        b.read(depth);
                    }
                    b.write(g_hiz);
                },
                [this, handles, level, src, dst, depth](render::PassContext& ctx) {
                    CullParams p = handles(ctx);
                    if (level == 0) {
                        p.depth = ctx.device.descriptor(ctx.texture(depth));
                    }
                    p.src_size = {src.width, src.height};
                    p.dst_size = {dst.width, dst.height};
                    p.src_offset = src.offset;
                    p.dst_offset = dst.offset;
                    ctx.cmd.set_pipeline(level == 0 ? hiz_first_ : hiz_down_);
                    ctx.cmd.set_parameters(p);
                    ctx.cmd.dispatch((dst.width + 7) / 8, (dst.height + 7) / 8);
                });
        }
        hiz_valid_ = true;
    }

    graph.add_compute_pass(
        "sp16 display",
        [&](render::PassBuilder& b) {
            b.write(hdr);
            b.write(frame.scene_color);
        },
        [this, shade_params, hdr, color = frame.scene_color, out_width, out_height](render::PassContext& ctx) {
            ShadeParams p = shade_params(ctx, {}, hdr);
            p.output = ctx.device.descriptor(ctx.texture(color), rhi::View::storage);
            p.size = {out_width, out_height};
            p.flags = config_.ssaa;
            ctx.cmd.set_pipeline(display_);
            ctx.cmd.set_parameters(p);
            ctx.cmd.dispatch((out_width + 7) / 8, (out_height + 7) / 8);
        });

    // Stats, once (a few frames in, so HiZ is warm): visible instances per part, masked fragments per pixel, quads.
    if (config_.stats && !stats_requested_ && frames_ == 8) {
        stats_requested_ = true;
        graph.add_graphics_pass(
            "stats overdraw",
            [&](render::PassBuilder& b) {
                declare_draw_inputs(b);
                b.write(g_counters);
                b.color_target_uint(vis, rhi::LoadOp::clear, {0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0});
                rhi::TextureDesc d2 = depth_desc;
                d2.debug_name = "sp16 stats depth";
                b.depth_target(graph.create_texture(d2), rhi::LoadOp::clear, 0.0f);
            },
            [this, draw_parts](render::PassContext& ctx) { draw_parts(ctx, vis_opaque_, count_, true); });
        graph.add_compute_pass(
            "stats quads",
            [&](render::PassBuilder& b) {
                b.read(vis);
                b.write(g_counters);
            },
            [this, shade_params, vis, hdr, width, height](render::PassContext& ctx) {
                ctx.cmd.set_pipeline(quad_stats_);
                ShadeParams p = shade_params(ctx, vis, hdr);
                p.counters = ctx.device.descriptor(counters_, rhi::View::storage);
                ctx.cmd.set_parameters(p);
                ctx.cmd.dispatch((width / 2 + 7) / 8, (height / 2 + 7) / 8);
            });
        const double pixels = static_cast<double>(width) * static_cast<double>(height);
        frame.readbacks->read(
            graph, g_counters, 8 * sizeof(std::uint32_t),
            [this, pixels](veach::Result<render::ReadbackData> d) {
                if (!d) {
                    return;
                }
                std::array<std::uint32_t, 8> c{};
                std::memcpy(c.data(), d->bytes.data(), sizeof(c));
                // [0] masked fragments shaded, [1] of them kept by the alpha test, [2] covered pixels,
                // [3] (quad, triangle) pairs.
                veach::log::info("sp16",
                                 "SP16_STATS {{\"path\": \"{}\", \"view\": \"{}\", \"masked_fragments_per_pixel\": "
                                 "{:.3f}, \"masked_kept_per_pixel\": {:.3f}, \"covered\": {:.4f}, "
                                 "\"quad_lane_efficiency\": {:.4f}}}",
                                 path_name(config_), config_.view, static_cast<double>(c[0]) / pixels,
                                 static_cast<double>(c[1]) / pixels, static_cast<double>(c[2]) / pixels,
                                 c[3] > 0 ? static_cast<double>(c[2]) / (4.0 * c[3]) : 0.0);
            },
            "sp16 stats readback");
        frame.readbacks->read(
            graph, g_args, part_count * args_stride,
            [this](veach::Result<render::ReadbackData> d) {
                if (!d) {
                    return;
                }
                std::array<std::uint32_t, part_count * 5> a{};
                std::memcpy(a.data(), d->bytes.data(), sizeof(a));
                std::string parts;
                std::uint64_t triangles = 0;
                constexpr std::array<const char*, part_count> names{"terrain", "rock_a", "rock_b", "trunk",
                                                                    "canopy",  "bush",   "grass"};
                for (std::uint32_t p = 0; p < part_count; ++p) {
                    parts += std::format("{}\"{}\": {}", p == 0 ? "" : ", ", names[p], a[p * 5 + 1]);
                    triangles += std::uint64_t{a[p * 5 + 1]} * (a[p * 5] / 3);
                }
                veach::log::info("sp16",
                                 "SP16_CULL {{\"view\": \"{}\", \"hiz\": {}, \"visible_triangles\": {}, "
                                 "\"instances\": {{{}}}}}",
                                 config_.view, config_.hiz ? "true" : "false", triangles, parts);
            },
            "sp16 args readback");
    }
}

}  // namespace

/// VEACH_SP16=engine: the same field drawn by the engine's real-time renderer (ADR 0093), to measure it on the
/// content the spike measured: each model a runtime mesh (MeshStore), each instance an entity, the cards in the
/// lab's FieldGrass and FieldLeaves materials (coverage from UV, shaders/field_materials.slang), the sun and the
/// physical sky. No draw distance: the engine has no LOD yet, so the far view draws every grass card in view.
void spawn_engine_field(veach::World& world, const Config& config) {
    const Field f = build_field(config);
    render::MeshStore& store = veach::services(world).get<render::MeshStore>();
    const auto material = [&](const char* name, const char* kind, math::vec3 color, bool masked) {
        veach::MaterialAsset m{
            .name = name, .kind = kind, .base_color = math::vec4(color, 1.0f), .metallic = 0.0f, .roughness = 0.85f};
        if (masked) {
            m.alpha_mode = veach::AlphaMode::mask;
            m.double_sided = true;
        }
        return store.add_material(m);
    };
    const std::array<veach::AssetId, material_count> materials{
        material("ground", "StandardMaterial", {0.30f, 0.28f, 0.18f}, false),
        material("rock", "StandardMaterial", {0.42f, 0.40f, 0.38f}, false),
        material("bark", "StandardMaterial", {0.25f, 0.18f, 0.12f}, false),
        material("leaves", "FieldLeaves", {0.20f, 0.38f, 0.12f}, true),
        material("bush", "FieldLeaves", {0.22f, 0.36f, 0.14f}, true),
        material("grass", "FieldGrass", {0.28f, 0.45f, 0.15f}, true)};
    std::array<veach::AssetId, model_count> meshes{};
    for (std::uint32_t model = 0; model < model_count; ++model) {
        render::MeshData data;
        std::map<std::uint32_t, std::uint32_t> remap;
        for (std::uint32_t slot = 0; slot < f.models[model].part_count; ++slot) {
            const Part& part = f.mesh.parts[f.models[model].first_part + slot];
            data.materials.push_back(materials[part.material]);
            for (std::uint32_t i = 0; i < part.index_count; ++i) {
                const std::uint32_t source = f.mesh.indices[part.first_index + i];
                const auto [it, added] = remap.try_emplace(source, static_cast<std::uint32_t>(data.vertices.size()));
                if (added) {
                    const Vertex& v = f.mesh.vertices[source];
                    veach::MeshVertex out;
                    out.position = {v.position_u.x, v.position_u.y, v.position_u.z};
                    out.normal = {v.normal_v.x, v.normal_v.y, v.normal_v.z};
                    out.uv = {v.position_u.w, v.normal_v.w};
                    data.vertices.push_back(out);
                }
                data.indices.push_back(it->second);
            }
            data.triangle_slots.insert(data.triangle_slots.end(), part.index_count / 3, slot);
        }
        auto added = store.add(std::move(data));
        if (!added) {
            veach::log::error("sp16", added.error());
            return;
        }
        meshes[model] = *added;
    }
    for (const Instance& inst : f.instances) {
        const float c = inst.rows[0].x;
        const float s = inst.rows[0].z;
        world.entity()
            .set(veach::Transform{math::dvec3(inst.rows[0].w, inst.rows[1].w, inst.rows[2].w),
                                  math::angleAxis(std::atan2(s, c), math::vec3(0.0f, 1.0f, 0.0f)),
                                  math::vec3(std::sqrt(c * c + s * s))})
            .set(veach::RuntimeMeshInstance{meshes[inst.model]});
    }
    const View v = view_named(config.view);
    const auto pose = [v, pan = config.pan, strafe = config.strafe](std::uint64_t frame) {
        const float yaw = v.yaw + math::radians(pan) * static_cast<float>(frame);
        const math::quat rotation =
            math::angleAxis(yaw, math::vec3(0.0f, 1.0f, 0.0f)) * math::angleAxis(v.pitch, math::vec3(1.0f, 0.0f, 0.0f));
        const math::vec3 right = math::angleAxis(v.yaw, math::vec3(0.0f, 1.0f, 0.0f)) * math::vec3(1.0f, 0.0f, 0.0f);
        return veach::Transform{math::dvec3(v.eye + right * (strafe * static_cast<float>(frame))), rotation,
                                math::vec3(1.0f)};
    };
    const veach::Entity camera =
        world.entity("sp16 camera")
            .set(pose(0))
            .set(veach::Camera{.fov_y = math::radians(60.0f), .near_plane = 0.1f, .active = true, .ev100 = 14.0f});
    if (config.taa_off) {
        camera.set(veach::AntiAliasing{.temporal = false});
    }
    if (config.pan != 0.0f || config.strafe != 0.0f) {
        // VEACH_SP16_PAN degrees and VEACH_SP16_STRAFE meters (to the view's right) a frame: motion for TAA.
        world.system<const veach::Time>("Sp16Move")
            .kind<veach::phase::Simulation>()
            .each([camera, pose](const veach::Time& time) { camera.set(pose(time.frame)); });
    }
    world.entity("sp16 sky").set(veach::Sky{.time_of_day = 15.0f, .latitude = 45.0f});
    veach::log::info("sp16", "engine field: {} instances as entities, {} meshes", f.instances.size(), meshes.size());
}

void register_sp16_spike(veach::World& world) {
    const std::optional<std::string> mode = veach::env("VEACH_SP16");
    if (!mode) {
        return;
    }
    if (*mode != "vis" && *mode != "forward" && *mode != "prepass" && *mode != "engine") {
        veach::log::error("sp16", "VEACH_SP16 is '{}': use vis, forward, prepass or engine", *mode);
        return;
    }
    const Config config = config_from_env(*mode);
    world.system<veach::Window>("Sp16Size")
        .kind<veach::phase::Startup>()
        .each([w = config.width, h = config.height](veach::Window& window) {
            window.width = w;
            window.height = h;
        });
    if (*mode == "engine") {
        spawn_engine_field(world, config);
        return;
    }
    veach::services(world).get<render::Renderer>().add_feature(std::make_unique<Sp16Feature>(config));
}

}  // namespace lab
