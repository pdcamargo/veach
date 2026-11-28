#include "atmosphere.hpp"

#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace veach::render {

namespace {

constexpr float pi = 3.14159265358979f;

/// Mirrors AtmosphereLutParams in shaders/atmosphere.slang.
struct alignas(16) AtmosphereLutParams {
    rhi::DescriptorHandle sky;
    rhi::DescriptorHandle output;
    std::array<std::uint32_t, 2> size{};
};
static_assert(rhi::parameters_match<AtmosphereLutParams>(veach_render_shaders::layout::atmosphere,
                                                         {VEACH_PARAM(AtmosphereLutParams, sky),
                                                          VEACH_PARAM(AtmosphereLutParams, output),
                                                          VEACH_PARAM(AtmosphereLutParams, size)}));

/// Mirrors SkyFrame in shaders/veach/atmosphere.slang (a structured-buffer element: offsets asserted by hand).
struct alignas(16) SkyFrame {
    AtmosphereParams atmosphere;
    math::vec4 sun{0.0f};       ///< xyz toward the sun, w = illuminance at the top (lux)
    math::vec4 camera{0.0f};    ///< x = altitude (km)
    math::vec4 constant{0.0f};  ///< xyz = constant mode's radiance (nits)
    math::mat4 inverse_view_projection{1.0f};
    rhi::DescriptorHandle transmittance;
    rhi::DescriptorHandle multiscattering;
    rhi::DescriptorHandle sky_view;
    rhi::DescriptorHandle aerial;
    rhi::DescriptorHandle irradiance;
    rhi::DescriptorHandle linear_clamp;
    std::uint32_t mode = 0;  ///< 0 none, 1 constant, 2 physical
    std::uint32_t aerial_on = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
    // Spike SP-10 (VCH-086): a planet around any center (PlanetSky). Unread unless `planet` is 1.
    math::vec4 origin{0.0f};    ///< xyz = the camera relative to the planet's center (km), w = altitude (km)
    math::vec4 frame_x{0.0f};   ///< the sky-view LUT's azimuth origin, perpendicular to local up
    math::vec4 ground{0.0f};    ///< xyz = ground albedo
    math::vec4 occluder{0.0f};  ///< xyz = center relative to the planet's center (km), w = radius (km)
    math::vec4 occluder_albedo{0.0f};
    std::array<math::vec4, 4> sun_directions{};  ///< xyz toward the sun, w = angular radius
    std::array<math::vec4, 4> sun_colors{};      ///< xyz = illuminance at the top / sun.w
    std::uint32_t planet = 0;
    std::uint32_t sun_count = 0;
    std::uint32_t march = 0;
    std::uint32_t march_steps = 0;
};
static_assert(offsetof(SkyFrame, inverse_view_projection) == 128);
static_assert(offsetof(SkyFrame, transmittance) == 192);
static_assert(offsetof(SkyFrame, mode) == 240);
static_assert(offsetof(SkyFrame, origin) == 256);
static_assert(offsetof(SkyFrame, sun_directions) == 336);
static_assert(offsetof(SkyFrame, sun_colors) == 400);
static_assert(offsetof(SkyFrame, planet) == 464);
static_assert(sizeof(SkyFrame) == 480);

struct Lut {
    const char* name;
    const char* entry;
    std::uint32_t width;
    std::uint32_t height;
};
constexpr std::array<Lut, 5> luts{{{"sky transmittance", "transmittance_lut", 256, 64},
                                   {"sky multiple scattering", "multiscattering_lut", 32, 32},
                                   {"sky view", "sky_view_lut", 192, 108},
                                   {"aerial perspective", "aerial_lut", 1024, 32},
                                   {"sky irradiance", "sky_irradiance", 1, 1}}};
enum LutIndex : std::size_t { transmittance_lut, multiscattering_lut, sky_view_lut, aerial_lut, irradiance_lut };

float ray_sphere(math::vec3 origin, math::vec3 direction, float radius) {
    const float b = math::dot(origin, direction);
    const float c = math::dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return -1.0f;
    }
    const float s = std::sqrt(discriminant);
    const float t0 = -b - s;
    const float t1 = -b + s;
    return t0 >= 0.0f ? t0 : (t1 >= 0.0f ? t1 : -1.0f);
}

math::vec3 extinction_at(const AtmosphereParams& a, float height) {
    const float rayleigh = std::exp(-height / a.rayleigh.w);
    const float mie = std::exp(-height / a.mie.w);
    const float ozone = std::max(0.0f, 1.0f - std::abs(height - a.ozone.w) / a.planet.w);
    return math::vec3(a.rayleigh) * rayleigh + (math::vec3(a.mie) + math::vec3(a.mie_absorption)) * mie +
           math::vec3(a.ozone) * ozone;
}

}  // namespace

AtmosphereParams atmosphere_params(const Sky& sky) {
    AtmosphereParams a;
    a.rayleigh = math::vec4(math::vec3(5.802e-3f, 13.558e-3f, 33.1e-3f) * sky.rayleigh, 8.0f);
    a.mie = math::vec4(math::vec3(3.996e-3f * sky.mie), 1.2f);
    a.mie_absorption = math::vec4(math::vec3(4.40e-4f * sky.mie), sky.mie_g);
    a.ozone = math::vec4(math::vec3(0.650e-3f, 1.881e-3f, 0.085e-3f) * sky.ozone, 25.0f);
    a.planet = math::vec4(6360.0f, 6460.0f, sky.ground_albedo, 15.0f);
    return a;
}

math::vec3 sun_direction(const Sky& sky) {
    const float declination = -0.40910518f * std::cos(2.0f * pi * (sky.day_of_year + 10.0f) / 365.0f);  // 23.44 deg
    const float hour_angle = (sky.time_of_day - 12.0f) * (pi / 12.0f);
    const float latitude = math::radians(sky.latitude);
    // Local east, north, up.
    const float east = -std::cos(declination) * std::sin(hour_angle);
    const float north =
        std::sin(declination) * std::cos(latitude) - std::cos(declination) * std::cos(hour_angle) * std::sin(latitude);
    const float up =
        std::sin(declination) * std::sin(latitude) + std::cos(declination) * std::cos(hour_angle) * std::cos(latitude);
    // North is -Z turned by `north` about +Y; east is then its right (+X when north is -Z).
    const float c = std::cos(sky.north);
    const float s = std::sin(sky.north);
    const math::vec3 north_axis(-s, 0.0f, -c);
    const math::vec3 east_axis(c, 0.0f, -s);
    return math::normalize(east_axis * east + north_axis * north + math::vec3(0.0f, up, 0.0f));
}

math::vec3 atmosphere_transmittance(const AtmosphereParams& a, float altitude_km, math::vec3 direction) {
    const math::vec3 origin(0.0f, a.planet.x + std::max(altitude_km, 0.0f), 0.0f);
    if (ray_sphere(origin, direction, a.planet.x) > 0.0f) {
        return math::vec3(0.0f);  // through the planet
    }
    const float length_to_top = ray_sphere(origin, direction, a.planet.y);
    if (length_to_top <= 0.0f) {
        return math::vec3(1.0f);
    }
    constexpr int steps = 40;  // as integrate_transmittance in veach/atmosphere.slang
    const float dt = length_to_top / steps;
    math::vec3 depth(0.0f);
    for (int i = 0; i < steps; ++i) {
        const math::vec3 p = origin + direction * ((static_cast<float>(i) + 0.5f) * dt);
        depth += extinction_at(a, math::length(p) - a.planet.x) * dt;
    }
    return math::exp(-depth);
}

struct AtmosphereRenderer::Impl {
    struct Target {
        rhi::TextureHandle handle;
        bool written = false;
    };
    rhi::Device* device = nullptr;
    std::array<rhi::PipelineHandle, luts.size()> pipelines{};
    std::array<Target, luts.size()> targets{};
    rhi::SamplerHandle linear_clamp;
    std::optional<AtmosphereParams> built;  ///< what the transmittance and multiple-scattering LUTs hold
    std::vector<rhi::BufferHandle> frame_buffers;

    ~Impl() {
        if (device == nullptr) {
            return;
        }
        device->wait_idle();
        release_frame_buffers();
        for (rhi::PipelineHandle p : pipelines) {
            if (p.is_valid()) {
                device->destroy(p);
            }
        }
        for (Target& t : targets) {
            if (t.handle.is_valid()) {
                device->destroy(t.handle);
            }
        }
        if (linear_clamp.is_valid()) {
            device->destroy(linear_clamp);
        }
    }
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void release_frame_buffers() {
        for (rhi::BufferHandle& b : frame_buffers) {
            device->destroy(b);
        }
        frame_buffers.clear();
    }
};

AtmosphereRenderer::AtmosphereRenderer() : impl_(std::make_unique<Impl>()) {}

AtmosphereRenderer::~AtmosphereRenderer() = default;

Result<> AtmosphereRenderer::prepare(rhi::Device& device) {
    Impl& s = *impl_;
    s.device = &device;
    for (std::size_t i = 0; i < luts.size(); ++i) {
        auto pipeline =
            device.create_compute_pipeline({&veach_render_shaders::atmosphere(),
                                            luts[i].entry,
                                            {i == irradiance_lut ? 1u : 8u, i == irradiance_lut ? 1u : 8u, 1},
                                            luts[i].name});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        s.pipelines[i] = *pipeline;
        auto texture = device.create_texture({luts[i].width, luts[i].height, rhi::Format::rgba16_float,
                                              rhi::TextureUsage::storage | rhi::TextureUsage::sampled, luts[i].name});
        if (!texture) {
            return std::unexpected(texture.error());
        }
        s.targets[i] = {*texture, false};
    }
    auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::clamp});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.linear_clamp = *sampler;
    return {};
}

SkyResources AtmosphereRenderer::record(RenderGraph& graph, const ResolvedSky& sky, const RenderView& view) {
    VEACH_ZONE("AtmosphereRenderer::record");
    Impl& s = *impl_;
    s.release_frame_buffers();  // last frame's: their submission is made, so destroy() waits for the GPU
    rhi::Device& device = *s.device;
    SkyFrame f;
    f.atmosphere = atmosphere_params(sky.settings);
    f.sun = math::vec4(sky.sun_direction, sky.sun_illuminance_top);
    f.camera = math::vec4(std::max(view.position.y, 0.0f) * 0.001f, 0.0f, 0.0f, 0.0f);
    f.constant = math::vec4(sky.settings.color, 0.0f);
    f.inverse_view_projection = view.inverse_view_projection;
    f.transmittance = device.descriptor(s.targets[transmittance_lut].handle);
    f.multiscattering = device.descriptor(s.targets[multiscattering_lut].handle);
    f.sky_view = device.descriptor(s.targets[sky_view_lut].handle);
    f.aerial = device.descriptor(s.targets[aerial_lut].handle);
    f.irradiance = device.descriptor(s.targets[irradiance_lut].handle);
    f.linear_clamp = device.descriptor(s.linear_clamp);
    f.mode = sky.mode == SkyMode::physical ? 2u : sky.mode == SkyMode::constant ? 1u : 0u;
    f.aerial_on = sky.settings.aerial_perspective ? 1u : 0u;
    if (sky.planet && sky.mode == SkyMode::physical) {
        // Spike SP-10 (VCH-086): the planet's medium, its own up, every sun scaled to the brightest one.
        const PlanetSky& p = *sky.planet;
        f.atmosphere = p.medium;
        f.planet = 1;
        f.origin = math::vec4(-p.center, p.altitude);
        f.camera = math::vec4(p.altitude, 0.0f, 0.0f, 0.0f);
        const math::vec3 up = math::normalize(-p.center);
        f.sun_count = std::min<std::uint32_t>(p.sun_count, static_cast<std::uint32_t>(p.suns.size()));
        float reference = 0.0f;
        for (std::uint32_t i = 0; i < f.sun_count; ++i) {
            reference = std::max(reference, std::max(p.suns.at(i).illuminance.x,
                                                     std::max(p.suns.at(i).illuminance.y, p.suns.at(i).illuminance.z)));
        }
        reference = std::max(reference, 1e-6f);
        for (std::uint32_t i = 0; i < f.sun_count; ++i) {
            f.sun_directions.at(i) = math::vec4(math::normalize(p.suns.at(i).direction), p.suns.at(i).angular_radius);
            f.sun_colors.at(i) = math::vec4(p.suns.at(i).illuminance / reference, 0.0f);
        }
        const math::vec3 primary = f.sun_count > 0 ? math::vec3(f.sun_directions[0]) : up;
        f.sun = math::vec4(primary, reference);
        // The LUT's azimuth origin: the first sun's horizontal direction (so its back seam faces away from it).
        math::vec3 x = primary - up * math::dot(primary, up);
        if (math::length(x) < 1e-3f) {
            x = std::abs(up.x) < 0.9f ? math::cross(up, math::vec3(1.0f, 0.0f, 0.0f))
                                      : math::cross(up, math::vec3(0.0f, 0.0f, 1.0f));
        }
        f.frame_x = math::vec4(math::normalize(x), 0.0f);
        f.ground = math::vec4(p.ground_albedo, 1.0f);
        f.occluder =
            p.occluder.w > 0.0f ? math::vec4(math::vec3(p.occluder) - p.center, p.occluder.w) : math::vec4(0.0f);
        f.occluder_albedo = math::vec4(p.occluder_albedo, 0.0f);
        f.march = p.march;
        f.march_steps = std::max<std::uint32_t>(p.march_steps, 1);
    }

    const std::array<SkyFrame, 1> records{f};
    auto buffer = device.create_buffer({.size = sizeof(SkyFrame),
                                        .memory = rhi::MemoryType::upload,
                                        .debug_name = "sky frame",
                                        .stride = sizeof(SkyFrame)},
                                       std::as_bytes(std::span(records)));
    if (!buffer) {
        log::error("render", buffer.error());
        return {};
    }
    s.frame_buffers.push_back(*buffer);
    const GraphResource frame = graph.import(*buffer, "sky frame", rhi::ResourceState::shader_read);
    std::array<GraphResource, luts.size()> targets{};
    for (std::size_t i = 0; i < luts.size(); ++i) {
        Impl::Target& t = s.targets[i];
        targets[i] = graph.import(t.handle, luts[i].name,
                                  t.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
                                  rhi::ResourceState::shader_read);
    }

    if (sky.mode == SkyMode::physical) {
        const AtmosphereLutParams base{.sky = device.descriptor(*buffer)};
        const auto pass = [&](std::size_t lut, std::initializer_list<std::size_t> inputs) {
            AtmosphereLutParams params = base;
            params.output = device.descriptor(s.targets[lut].handle, rhi::View::storage);
            params.size = {luts[lut].width, luts[lut].height};
            const std::uint32_t group = lut == irradiance_lut ? 1u : 8u;
            graph.add_compute_pass(
                luts[lut].name,
                [&](PassBuilder& b) {
                    b.read(frame);
                    for (const std::size_t input : inputs) {
                        b.read(targets[input]);
                    }
                    b.write(targets[lut], rhi::ResourceState::shader_write);
                },
                [params, pipeline = s.pipelines[lut], group](PassContext& ctx) {
                    ctx.cmd.set_pipeline(pipeline);
                    ctx.cmd.set_parameters(params);
                    ctx.cmd.dispatch((params.size[0] + group - 1) / group, (params.size[1] + group - 1) / group);
                });
            s.targets[lut].written = true;
        };
        // The transmittance and multiple-scattering LUTs depend only on the medium: rebuilt when it changes.
        if (!s.built || *s.built != f.atmosphere) {
            pass(transmittance_lut, {});
            pass(multiscattering_lut, {transmittance_lut});
            s.built = f.atmosphere;
        }
        pass(sky_view_lut, {transmittance_lut, multiscattering_lut});
        pass(aerial_lut, {transmittance_lut, multiscattering_lut});
        pass(irradiance_lut, {sky_view_lut});
    }

    return {*buffer, [frame, targets](PassBuilder& b) {
                b.read(frame);
                for (const GraphResource& t : targets) {
                    b.read(t);
                }
            }};
}

}  // namespace veach::render
