#include "backdrop.hpp"

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <vector>

namespace veach::render {

namespace {

constexpr double pi = 3.14159265358979323846;

/// Mirrors NebulaRecord in shaders/backdrop.slang.
struct BackdropNebulaRecord {
    math::vec4 direction{0.0f};  ///< xyz = center, w = angular radius
    math::vec4 color{0.0f};      ///< xyz = rim / reflection color, w = brightness
    math::vec4 core{0.0f};       ///< xyz = core color, w = core share
    math::vec4 shape{1.0f, 0.0f, 0.0f, 0.0f};
    std::array<std::uint32_t, 4> info{};  ///< kind, seed
};
static_assert(sizeof(BackdropNebulaRecord) == 80);

/// Mirrors BakeRecord in shaders/backdrop.slang (a structured-buffer element: offsets asserted by hand).
struct BackdropBakeRecord {
    math::vec4 band{0.0f};  ///< brightness, latitude, half-width, bulge
    math::vec4 more{0.0f};  ///< anticenter, dust
    std::array<std::uint32_t, 4> info{};
    std::array<BackdropNebulaRecord, max_backdrop_nebulae> nebulae{};
};
static_assert(offsetof(BackdropBakeRecord, info) == 32);
static_assert(offsetof(BackdropBakeRecord, nebulae) == 48);
static_assert(sizeof(BackdropBakeRecord) == 48 + 80 * max_backdrop_nebulae);

/// Mirrors BakeParams in shaders/backdrop.slang.
struct alignas(16) BackdropBakeParams {
    rhi::DescriptorHandle record;
    rhi::DescriptorHandle output;
    std::array<std::uint32_t, 2> size{};
};
static_assert(rhi::parameters_match<BackdropBakeParams>(veach_render_shaders::layout::backdrop,
                                                        {VEACH_PARAM(BackdropBakeParams, record),
                                                         VEACH_PARAM(BackdropBakeParams, output),
                                                         VEACH_PARAM(BackdropBakeParams, size)}));

BackdropBakeRecord bake_record(const Backdrop& b) {
    BackdropBakeRecord r;
    r.band = math::vec4(b.band_brightness, b.band_latitude, b.band_width, b.bulge);
    r.more = math::vec4(b.anticenter, b.dust, 0.0f, 0.0f);
    r.info = {b.seed, std::min<std::uint32_t>(b.nebula_count, max_backdrop_nebulae), 0, 0};
    for (std::size_t i = 0; i < max_backdrop_nebulae; ++i) {
        const BackdropNebula& n = b.nebulae.at(i);
        BackdropNebulaRecord& o = r.nebulae.at(i);
        o.direction = math::vec4(n.direction, std::max(n.radius, 1e-3f));
        o.color = math::vec4(n.color, n.brightness);
        o.core = math::vec4(n.core_color, n.core);
        o.shape = math::vec4(std::max(n.elongation, 0.1f), n.angle, 0.0f, 0.0f);
        o.info = {static_cast<std::uint32_t>(n.kind), n.seed, 0, 0};
    }
    return r;
}

// ---- the generator: canonical (veach::hash, dmath; + - * / sqrt only otherwise) ----------------------------------

math::dvec3 unit_direction(HashStream& h) {
    const double z = h.uniform(-1.0, 1.0);
    const double phi = h.uniform(0.0, 2.0 * pi);
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * dmath::cos(phi), z, r * dmath::sin(phi)};
}

double gaussian(HashStream& h) {
    const double u = std::max(h.next_double(), 1e-300);
    const double v = h.next_double();
    return std::sqrt(-2.0 * dmath::log(u)) * dmath::cos(2.0 * pi * v);
}

double log_uniform(HashStream& h, double lo, double hi) {
    return lo * dmath::exp(h.next_double() * dmath::log(hi / lo));
}

math::dvec3 lerp(const math::dvec3& a, const math::dvec3& b, double t) {
    return a + (b - a) * t;
}

math::vec3 to_float(const math::dvec3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

/// A direction at galactic latitude `b` and longitude `l` (radians): +X toward the center, +Y north (backdrop_uv).
math::dvec3 galactic_direction(double b, double l) {
    const double c = dmath::cos(b);
    return {c * dmath::cos(l), dmath::sin(b), c * dmath::sin(l)};
}

BackdropNebula make_nebula(HashStream& h, const GalaxyView& galaxy) {
    BackdropNebula n;
    const double pick = h.next_double();
    n.kind = pick < 0.5 ? NebulaKind::emission : (pick < 0.72 ? NebulaKind::reflection : NebulaKind::dark);
    // Star-forming regions and dust lie in the disk: emission and dark nebulae near the band's center line,
    // reflection nebulae (nearer, lit by single stars) anywhere but the poles.
    const double spread = n.kind == NebulaKind::reflection ? 0.35 : 1.3 * galaxy.band_width;
    const double latitude = std::clamp(galaxy.band_latitude + spread * gaussian(h), -1.3, 1.3);
    const double longitude = h.uniform(-pi, pi);
    n.direction = to_float(galactic_direction(latitude, longitude));
    switch (n.kind) {
        case NebulaKind::emission: {
            n.radius = static_cast<float>(log_uniform(h, 0.05, 0.28));
            // H-alpha (656 nm) with a little H-beta and [S II]: deep red to pink.
            n.color = to_float(lerp({1.0, 0.20, 0.28}, {1.0, 0.38, 0.50}, h.next_double()));
            // [O III] (501 nm): teal to blue-green.
            n.core_color = to_float(lerp({0.20, 0.75, 0.75}, {0.30, 0.62, 0.95}, h.next_double()));
            n.core = static_cast<float>(h.uniform(0.05, 0.6));
            n.brightness = static_cast<float>(h.uniform(1.5, 5.0));
            break;
        }
        case NebulaKind::reflection: {
            n.radius = static_cast<float>(log_uniform(h, 0.03, 0.14));
            n.color = to_float(lerp({0.30, 0.50, 1.0}, {0.55, 0.70, 1.0}, h.next_double()));
            n.brightness = static_cast<float>(h.uniform(1.0, 3.0));
            break;
        }
        case NebulaKind::dark: {
            n.radius = static_cast<float>(log_uniform(h, 0.06, 0.35));
            n.brightness = static_cast<float>(h.uniform(1.2, 3.5));
            break;
        }
    }
    n.elongation = static_cast<float>(h.uniform(1.0, 2.6));
    n.angle = static_cast<float>(h.uniform(0.0, pi));
    n.seed = static_cast<std::uint32_t>(h.next() >> 32);
    return n;
}

void absorb(std::uint64_t& h, float v) {
    h = hash(h, std::bit_cast<std::uint32_t>(v));
}

void absorb(std::uint64_t& h, const math::vec3& v) {
    absorb(h, v.x);
    absorb(h, v.y);
    absorb(h, v.z);
}

}  // namespace

bool Backdrop::bakes_like(const Backdrop& other) const {
    Backdrop a = *this;
    Backdrop b = other;
    for (Backdrop* x : {&a, &b}) {
        x->orientation = math::mat3(1.0f);
        x->star_density = x->star_magnitude_scale = x->star_saturation = 0.0f;
    }
    return a == b;
}

GalaxyView galaxy_view_from_seed(std::uint64_t seed) {
    HashStream h = HashStream::make(seed, "veach:backdrop.galaxy", 0);
    GalaxyView g;
    g.north = unit_direction(h);
    const math::dvec3 a = unit_direction(h);
    math::dvec3 c = a - g.north * math::dot(a, g.north);
    const double length = std::sqrt(math::dot(c, c));
    if (length < 1e-6) {
        c = std::abs(g.north.x) < 0.9 ? math::dvec3(1.0, 0.0, 0.0) : math::dvec3(0.0, 0.0, 1.0);
        c = c - g.north * math::dot(c, g.north);
        g.center = c / std::sqrt(math::dot(c, c));
    } else {
        g.center = c / length;
    }
    g.band_latitude = h.uniform(-0.04, 0.04);
    return g;
}

Backdrop generate_backdrop(std::uint64_t seed, const GalaxyView& galaxy) {
    HashStream h = HashStream::make(seed, "veach:backdrop", 0);
    Backdrop b;
    b.seed = static_cast<std::uint32_t>(h.next() >> 32);
    const math::dvec3 x = galaxy.center;
    const math::dvec3 y = galaxy.north;
    const math::dvec3 z = math::cross(x, y);
    b.orientation = math::mat3(to_float(x), to_float(y), to_float(z));
    b.band_brightness = static_cast<float>(galaxy.band_brightness);
    b.band_latitude = static_cast<float>(galaxy.band_latitude);
    b.band_width = static_cast<float>(galaxy.band_width);
    b.bulge = static_cast<float>(galaxy.bulge);
    b.anticenter = static_cast<float>(galaxy.anticenter);
    b.dust = static_cast<float>(galaxy.dust * h.uniform(0.8, 1.2));
    // Two to seven nebulae for an ordinary view, then scaled by how much star formation is nearby.
    HashStream nebulae = HashStream::make(seed, "veach:backdrop.nebulae", 0);
    const double expected = std::max(galaxy.nebulae, 0.0) * nebulae.uniform(2.0, 7.0);
    b.nebula_count = static_cast<std::uint32_t>(
        std::clamp(std::floor(expected + 0.5), 0.0, static_cast<double>(max_backdrop_nebulae)));
    for (std::uint32_t i = 0; i < b.nebula_count; ++i) {
        b.nebulae.at(i) = make_nebula(nebulae, galaxy);
    }
    // At least one emission nebula when there are any: the sky's color.
    if (b.nebula_count > 0 && std::ranges::none_of(std::span(b.nebulae).first(b.nebula_count),
                                                   [](const auto& n) { return n.kind == NebulaKind::emission; })) {
        HashStream again = HashStream::make(seed, "veach:backdrop.emission", 0);
        while (b.nebulae.front().kind != NebulaKind::emission) {
            b.nebulae.front() = make_nebula(again, galaxy);
        }
    }
    return b;
}

std::uint64_t backdrop_hash(const Backdrop& b) {
    std::uint64_t h = hash_id("veach:backdrop");
    h = hash(h, b.seed, b.nebula_count, b.resolution);
    for (int c = 0; c < 3; ++c) {
        absorb(h, b.orientation[c]);
    }
    for (const float v : {b.band_brightness, b.band_latitude, b.band_width, b.bulge, b.anticenter, b.dust,
                          b.star_density, b.star_magnitude_scale, b.star_saturation}) {
        absorb(h, v);
    }
    for (const BackdropNebula& n : b.nebulae) {
        absorb(h, n.direction);
        absorb(h, n.color);
        absorb(h, n.core_color);
        for (const float v : {n.radius, n.brightness, n.core, n.elongation, n.angle}) {
            absorb(h, v);
        }
        h = hash(h, static_cast<std::uint32_t>(n.kind), n.seed);
    }
    return h;
}

math::vec3 blackbody_color(float kelvin) {
    const float t = std::clamp(kelvin, 1000.0f, 15000.0f);
    const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t * t) /
                    (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t * t);
    const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t * t) /
                    (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t * t);
    const float d = 2.0f * u - 8.0f * v + 4.0f;
    const float x = 3.0f * u / d;
    const float y = 2.0f * v / d;
    const math::vec3 xyz(x / y, 1.0f, (1.0f - x - y) / y);
    const math::vec3 rgb = math::max(math::vec3(3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
                                                -0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
                                                0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z),
                                     math::vec3(0.0f));
    const float luminance = 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
    return rgb / std::max(luminance, 1e-6f);
}

// ---- the GPU side ----------------------------------------------------------------------------------------------

std::uint32_t backdrop_width(const Backdrop& backdrop) {
    return std::clamp<std::uint32_t>(backdrop.resolution, 256, 8192) & ~1u;
}

struct BackdropRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle pipeline;
    rhi::SamplerHandle linear_repeat;
    rhi::TextureHandle texture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool written = false;
    std::optional<Backdrop> baked;
    std::vector<rhi::BufferHandle> uploads;  ///< bake records of earlier frames
    std::uint32_t bakes = 0;

    Impl() = default;
    ~Impl() {
        if (device == nullptr) {
            return;
        }
        device->wait_idle();
        release_uploads();
        if (texture.is_valid()) {
            device->destroy(texture);
        }
        if (pipeline.is_valid()) {
            device->destroy(pipeline);
        }
        if (linear_repeat.is_valid()) {
            device->destroy(linear_repeat);
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void release_uploads() {
        for (rhi::BufferHandle& b : uploads) {
            device->destroy(b);
        }
        uploads.clear();
    }
};

BackdropRenderer::BackdropRenderer() : impl_(std::make_unique<Impl>()) {}

BackdropRenderer::~BackdropRenderer() = default;

Result<> BackdropRenderer::prepare(rhi::Device& device) {
    Impl& s = *impl_;
    s.device = &device;
    auto pipeline = device.create_compute_pipeline(
        {&veach_render_shaders::backdrop(), "backdrop_bake", {8, 8, 1}, "space backdrop bake"});
    if (!pipeline) {
        return std::unexpected(pipeline.error());
    }
    s.pipeline = *pipeline;
    auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::repeat});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.linear_repeat = *sampler;
    return {};
}

std::optional<GraphResource> BackdropRenderer::record(RenderGraph& graph, const Backdrop& backdrop) {
    VEACH_ZONE("BackdropRenderer::record");
    Impl& s = *impl_;
    s.release_uploads();  // last frame's: their submission is made, so destroy() waits for the GPU
    rhi::Device& device = *s.device;
    const std::uint32_t width = backdrop_width(backdrop);
    const std::uint32_t height = width / 2;
    if (!s.texture.is_valid() || s.width != width) {
        if (s.texture.is_valid()) {
            device.destroy(s.texture);
        }
        auto texture =
            device.create_texture({width, height, rhi::Format::rgba16_float,
                                   rhi::TextureUsage::storage | rhi::TextureUsage::sampled, "space backdrop"});
        if (!texture) {
            log::error("render", texture.error());
            s.texture = {};
            return std::nullopt;
        }
        s.texture = *texture;
        s.width = width;
        s.height = height;
        s.written = false;
        s.baked.reset();
    }
    const GraphResource target = graph.import(
        s.texture, "space backdrop", s.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
        rhi::ResourceState::shader_read);
    if (s.baked && s.baked->bakes_like(backdrop)) {
        return target;
    }
    const std::array<BackdropBakeRecord, 1> records{bake_record(backdrop)};
    auto buffer = device.create_buffer({.size = sizeof(BackdropBakeRecord),
                                        .memory = rhi::MemoryType::upload,
                                        .debug_name = "space backdrop bake",
                                        .stride = sizeof(BackdropBakeRecord)},
                                       std::as_bytes(std::span(records)));
    if (!buffer) {
        log::error("render", buffer.error());
        return std::nullopt;
    }
    s.uploads.push_back(*buffer);
    const GraphResource record = graph.import(*buffer, "space backdrop bake record", rhi::ResourceState::shader_read);
    BackdropBakeParams params{.size = {width, height}};
    graph.add_compute_pass(
        "space backdrop bake",
        [&](PassBuilder& b) {
            b.read(record);
            b.write(target, rhi::ResourceState::shader_write);
        },
        [params, pipeline = s.pipeline, record, target](PassContext& ctx) mutable {
            params.record = ctx.device.descriptor(ctx.buffer(record));
            params.output = ctx.device.descriptor(ctx.texture(target), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(params);
            ctx.cmd.dispatch((params.size[0] + 7) / 8, (params.size[1] + 7) / 8);
        });
    s.written = true;
    s.baked = backdrop;
    ++s.bakes;
    log::info("render", "space backdrop: baked {}x{} ({} nebulae)", width, height, backdrop.nebula_count);
    return target;
}

rhi::TextureHandle BackdropRenderer::texture() const {
    return impl_->texture;
}

rhi::SamplerHandle BackdropRenderer::sampler() const {
    return impl_->linear_repeat;
}

std::uint32_t BackdropRenderer::bakes() const {
    return impl_->bakes;
}

}  // namespace veach::render
