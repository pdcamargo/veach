#include "atmosphere.hpp"

#include "backdrop.hpp"

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

/// Mirrors AerialVolumeParams in shaders/aerial_volume.slang.
struct alignas(16) AerialVolumeParams {
    rhi::DescriptorHandle sky;
    rhi::DescriptorHandle output;
    std::array<std::uint32_t, 4> size{};  ///< cells across, down, slices; w unused
};
static_assert(rhi::parameters_match<AerialVolumeParams>(veach_render_shaders::layout::aerial_volume,
                                                        {VEACH_PARAM(AerialVolumeParams, sky),
                                                         VEACH_PARAM(AerialVolumeParams, output),
                                                         VEACH_PARAM(AerialVolumeParams, size)}));

/// Mirrors SkyFrame in shaders/veach/atmosphere.slang (a structured-buffer element: offsets asserted by hand).
struct alignas(16) SkyFrame {
    AtmosphereParams atmosphere;
    math::vec4 sun{0.0f};       ///< xyz toward the sun, w = the brightest sun's illuminance at the top (lux)
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
    std::uint32_t aerial_slices = 0;
    std::uint32_t pad1 = 0;
    // The planet block: a PlanetSky's (planet = 1), or the Sky's one sun around a center below the camera.
    math::vec4 origin{0.0f};   ///< xyz = the camera relative to the planet's center (km), w = altitude (km)
    math::vec4 frame_x{0.0f};  ///< the sky-view LUT's azimuth origin, perpendicular to local up
    math::vec4 ground{0.0f};   ///< xyz = ground albedo
    std::array<math::vec4, PlanetSky::max_occluders> occluders{};  ///< xyz = center from the planet's (km), w = radius
    std::array<math::vec4, PlanetSky::max_occluders> occluder_albedo{};  ///< w = 1: the sky draws it
    std::array<math::vec4, PlanetSky::max_suns> sun_directions{};        ///< xyz toward the sun, w = angular radius
    std::array<math::vec4, PlanetSky::max_suns> sun_colors{};            ///< xyz = illuminance at the top / sun.w
    std::uint32_t planet = 0;
    std::uint32_t sun_count = 0;
    std::uint32_t march = 0;
    std::uint32_t march_steps = 0;
    /// The starfield (ResolvedSky::stars). x = brightness (0: off), y = a pixel's angle (rad), z = limiting
    /// magnitude, w = cells per cube-map face side.
    math::vec4 stars{0.0f};
    // The space backdrop (ADR 0091, veach/backdrop.slang). Unread unless backdrop_info[3] is 1.
    math::vec4 galactic_x{0.0f};  ///< row 0 of galactic from render; w = one pixel's angle
    math::vec4 galactic_y{0.0f};  ///< row 1; w = exposure compensation (stops)
    math::vec4 galactic_z{0.0f};  ///< row 2; w = exposure follow
    math::vec4 camera_right{0.0f};
    math::vec4 camera_up{0.0f};
    math::vec4 backdrop_stars{0.0f};               ///< density, magnitude scale, saturation, sky veil
    std::array<std::uint32_t, 4> backdrop_info{};  ///< seed, width, height, on
    rhi::DescriptorHandle backdrop;
    rhi::DescriptorHandle linear_repeat;
    math::vec4 occlusion{1.0f, 0.0f, 0.0f, 0.0f};  ///< x = sky visibility, y = its reach (km)
    std::uint32_t occluder_count = 0;
    std::array<std::uint32_t, 3> pad2{};
};
static_assert(offsetof(SkyFrame, inverse_view_projection) == 128);
static_assert(offsetof(SkyFrame, transmittance) == 192);
static_assert(offsetof(SkyFrame, mode) == 240);
static_assert(offsetof(SkyFrame, origin) == 256);
static_assert(offsetof(SkyFrame, occluders) == 304);
static_assert(offsetof(SkyFrame, occluder_albedo) == 368);
static_assert(offsetof(SkyFrame, sun_directions) == 432);
static_assert(offsetof(SkyFrame, sun_colors) == 496);
static_assert(offsetof(SkyFrame, planet) == 560);
static_assert(offsetof(SkyFrame, stars) == 576);
static_assert(offsetof(SkyFrame, galactic_x) == 592);
static_assert(offsetof(SkyFrame, backdrop_info) == 688);
static_assert(offsetof(SkyFrame, backdrop) == 704);
static_assert(offsetof(SkyFrame, linear_repeat) == 712);
static_assert(offsetof(SkyFrame, occlusion) == 720);
static_assert(offsetof(SkyFrame, occluder_count) == 736);
static_assert(sizeof(SkyFrame) == 752);

/// How fast a lit sky hides the backdrop's boost: its gain falls by e per this much exposed sky luminance
/// (veach/backdrop.slang's backdrop_gain). A day sky (about 0.2 exposed) hides it; a twilight sky (0.01) does not.
constexpr float backdrop_sky_veil = 30.0f;

struct Lut {
    const char* name;
    const char* entry;
    std::uint32_t width;
    std::uint32_t height;
};
/// The per-medium tables (one pair per cache slot) and the per-frame ones.
constexpr Lut transmittance_lut{"sky transmittance", "transmittance_lut", 256, 64};
constexpr Lut multiscattering_lut{"sky multiple scattering", "multiscattering_lut", 32, 32};
constexpr Lut sky_view_lut{"sky view", "sky_view_lut", 192, 108};  // sky_view_size in the shader
constexpr Lut irradiance_lut{"sky irradiance", "sky_irradiance", 1, 1};

/// The aerial-perspective volume: one cell per this many pixels across and down, and its depth slices.
constexpr std::uint32_t aerial_cell_pixels = 16;
constexpr std::uint32_t aerial_slices = 32;

math::vec3 extinction_at(const AtmosphereParams& a, float height) {
    const float rayleigh = std::exp(-height / a.rayleigh.w);
    const float mie = std::exp(-height / a.mie.w);
    const float ozone = std::max(0.0f, 1.0f - std::abs(height - a.ozone.w) / a.planet.w);
    return math::vec3(a.rayleigh) * rayleigh + (math::vec3(a.mie) + math::vec3(a.mie_absorption)) * mie +
           math::vec3(a.ozone) * ozone;
}

/// The nearest positive hit of a sphere of `radius` from a point at distance `r` from its center, or -1.
float ray_sphere_at(math::vec3 origin, math::vec3 direction, float r, float radius) {
    const auto span = sphere_span(origin, direction, (r - radius) * (r + radius));
    if (!span) {
        return -1.0f;
    }
    return span->t0 >= 0.0f ? span->t0 : (span->t1 >= 0.0f ? span->t1 : -1.0f);
}

}  // namespace

std::optional<SphereSpan> sphere_span(math::vec3 origin, math::vec3 direction, float c) {
    const float b = math::dot(origin, direction);
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }
    const float q = b >= 0.0f ? -b - std::sqrt(discriminant) : -b + std::sqrt(discriminant);
    const float other = q != 0.0f ? c / q : 0.0f;
    return SphereSpan{std::min(q, other), std::max(q, other)};
}

bool sphere_blocks(math::vec3 p, math::vec3 l, float radius) {
    const float b = math::dot(p, l);
    return b < 0.0f && math::dot(p, p) - b * b < radius * radius;
}

float aerial_slice_w(float x) {
    return std::acos(std::clamp(1.0f - 2.0f * std::sqrt(std::clamp(x, 0.0f, 1.0f)), -1.0f, 1.0f)) / pi;
}

float aerial_slice_x(float w) {
    const float f = 0.5f - 0.5f * std::cos(pi * w);
    return f * f;
}

AtmosphereLutCache::Slot AtmosphereLutCache::acquire(const AtmosphereParams& medium) {
    ++clock_;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].medium && *entries_[i].medium == medium) {
            entries_[i].used = clock_;
            return {i, false};
        }
    }
    std::size_t oldest = 0;
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (entries_[i].used < entries_[oldest].used) {
            oldest = i;
        }
    }
    entries_[oldest] = {medium, clock_};
    ++builds_;
    return {oldest, true};
}

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
    const float r = a.planet.x + std::max(altitude_km, 0.0f);
    const math::vec3 origin(0.0f, r, 0.0f);
    if (sphere_blocks(origin, direction, a.planet.x)) {
        return math::vec3(0.0f);  // through the planet
    }
    const float length_to_top = ray_sphere_at(origin, direction, r, a.planet.y);
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
    struct MediumTables {
        Target transmittance;
        Target multiscattering;
    };
    rhi::Device* device = nullptr;
    rhi::PipelineHandle transmittance_pipeline, multiscattering_pipeline, sky_view_pipeline, irradiance_pipeline,
        aerial_pipeline;
    std::array<MediumTables, AtmosphereLutCache::slots> media{};
    AtmosphereLutCache cache;
    Target sky_view, irradiance, aerial;
    std::array<std::uint32_t, 2> aerial_size{};
    rhi::SamplerHandle linear_clamp, linear_wrap;
    std::vector<rhi::BufferHandle> frame_buffers;
    BackdropRenderer backdrop;

    ~Impl() {
        if (device == nullptr) {
            return;
        }
        device->wait_idle();
        release_frame_buffers();
        for (rhi::PipelineHandle p : {transmittance_pipeline, multiscattering_pipeline, sky_view_pipeline,
                                      irradiance_pipeline, aerial_pipeline}) {
            if (p.is_valid()) {
                device->destroy(p);
            }
        }
        for (MediumTables& m : media) {
            destroy(m.transmittance);
            destroy(m.multiscattering);
        }
        destroy(sky_view);
        destroy(irradiance);
        destroy(aerial);
        for (rhi::SamplerHandle s : {linear_clamp, linear_wrap}) {
            if (s.is_valid()) {
                device->destroy(s);
            }
        }
    }
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void destroy(Target& t) {
        if (t.handle.is_valid()) {
            device->destroy(t.handle);
        }
        t = {};
    }

    void release_frame_buffers() {
        for (rhi::BufferHandle& b : frame_buffers) {
            device->destroy(b);
        }
        frame_buffers.clear();
    }

    Result<Target> make_2d(const Lut& lut) {
        auto texture = device->create_texture({lut.width, lut.height, rhi::Format::rgba16_float,
                                               rhi::TextureUsage::storage | rhi::TextureUsage::sampled, lut.name});
        if (!texture) {
            return std::unexpected(texture.error());
        }
        return Target{*texture, false};
    }

    Result<rhi::PipelineHandle> make_pipeline(const rhi::ShaderLibrary* library, const char* entry, std::uint32_t group,
                                              const char* name) {
        return device->create_compute_pipeline({library, entry, {group, group, 1}, name});
    }

    /// The volume's cells for a view of width x height pixels; recreated when they change.
    Result<> ensure_aerial(std::uint32_t width, std::uint32_t height) {
        const std::array<std::uint32_t, 2> size{
            std::clamp((width + aerial_cell_pixels - 1) / aerial_cell_pixels, 8u, 512u),
            std::clamp((height + aerial_cell_pixels - 1) / aerial_cell_pixels, 8u, 512u)};
        if (aerial.handle.is_valid() && size == aerial_size) {
            return {};
        }
        destroy(aerial);
        auto texture = device->create_texture({.width = size[0],
                                               .height = size[1],
                                               .format = rhi::Format::rgba16_float,
                                               .usage = rhi::TextureUsage::storage | rhi::TextureUsage::sampled,
                                               .debug_name = "aerial perspective",
                                               .type = rhi::TextureType::d3,
                                               .depth = aerial_slices});
        if (!texture) {
            return std::unexpected(texture.error());
        }
        aerial = {*texture, false};
        aerial_size = size;
        return {};
    }
};

AtmosphereRenderer::AtmosphereRenderer() : impl_(std::make_unique<Impl>()) {}

AtmosphereRenderer::~AtmosphereRenderer() = default;

std::uint64_t AtmosphereRenderer::medium_builds() const {
    return impl_->cache.builds();
}

Result<> AtmosphereRenderer::prepare(rhi::Device& device) {
    Impl& s = *impl_;
    s.device = &device;
    const rhi::ShaderLibrary* luts = &veach_render_shaders::atmosphere();
    const struct {
        rhi::PipelineHandle* out;
        const rhi::ShaderLibrary* library;
        const char* entry;
        std::uint32_t group;
        const char* name;
    } pipelines[] = {
        {&s.transmittance_pipeline, luts, transmittance_lut.entry, 8, transmittance_lut.name},
        {&s.multiscattering_pipeline, luts, multiscattering_lut.entry, 8, multiscattering_lut.name},
        {&s.sky_view_pipeline, luts, sky_view_lut.entry, 8, sky_view_lut.name},
        {&s.irradiance_pipeline, luts, irradiance_lut.entry, 1, irradiance_lut.name},
        {&s.aerial_pipeline, &veach_render_shaders::aerial_volume(), "aerial_volume", 8, "aerial perspective"},
    };
    for (const auto& p : pipelines) {
        auto pipeline = s.make_pipeline(p.library, p.entry, p.group, p.name);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *p.out = *pipeline;
    }
    for (Impl::MediumTables& m : s.media) {
        auto t = s.make_2d(transmittance_lut);
        if (!t) {
            return std::unexpected(t.error());
        }
        m.transmittance = *t;
        auto ms = s.make_2d(multiscattering_lut);
        if (!ms) {
            return std::unexpected(ms.error());
        }
        m.multiscattering = *ms;
    }
    for (auto [target, lut] : {std::pair{&s.sky_view, &sky_view_lut}, std::pair{&s.irradiance, &irradiance_lut}}) {
        auto t = s.make_2d(*lut);
        if (!t) {
            return std::unexpected(t.error());
        }
        *target = *t;
    }
    if (auto aerial = s.ensure_aerial(1280, 720); !aerial) {
        return aerial;
    }
    auto clamp = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::clamp});
    if (!clamp) {
        return std::unexpected(clamp.error());
    }
    s.linear_clamp = *clamp;
    auto wrap = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::repeat});
    if (!wrap) {
        return std::unexpected(wrap.error());
    }
    s.linear_wrap = *wrap;
    return s.backdrop.prepare(device);
}

namespace {

/// The planet block of a PlanetSky: its medium, its own up, every sun scaled to the brightest, the occluders.
void fill_planet(SkyFrame& f, const PlanetSky& p) {
    f.atmosphere = p.medium;
    f.planet = 1;
    f.origin = math::vec4(-p.center, p.altitude);
    f.camera = math::vec4(p.altitude, 0.0f, 0.0f, 0.0f);
    const math::vec3 up = math::normalize(-p.center);
    f.sun_count = std::min<std::uint32_t>(p.sun_count, static_cast<std::uint32_t>(p.suns.size()));
    float reference = 0.0f;
    for (std::uint32_t i = 0; i < f.sun_count; ++i) {
        const math::vec3 e = p.suns.at(i).illuminance;
        reference = std::max(reference, std::max(e.x, std::max(e.y, e.z)));
    }
    reference = std::max(reference, 1e-6f);
    for (std::uint32_t i = 0; i < f.sun_count; ++i) {
        f.sun_directions.at(i) = math::vec4(math::normalize(p.suns.at(i).direction), p.suns.at(i).angular_radius);
        f.sun_colors.at(i) = math::vec4(p.suns.at(i).illuminance / reference, 0.0f);
    }
    const math::vec3 primary = f.sun_count > 0 ? math::vec3(f.sun_directions[0]) : up;
    f.sun = math::vec4(primary, reference);
    // The LUT's azimuth origin: the first sun's horizontal direction (so its wrap faces away from it).
    math::vec3 x = primary - up * math::dot(primary, up);
    if (math::length(x) < 1e-3f) {
        x = std::abs(up.x) < 0.9f ? math::cross(up, math::vec3(1.0f, 0.0f, 0.0f))
                                  : math::cross(up, math::vec3(0.0f, 0.0f, 1.0f));
    }
    f.frame_x = math::vec4(math::normalize(x), 0.0f);
    f.ground = math::vec4(p.ground_albedo, 0.0f);
    f.occluder_count = std::min<std::uint32_t>(p.occluder_count, static_cast<std::uint32_t>(p.occluders.size()));
    for (std::uint32_t i = 0; i < f.occluder_count; ++i) {
        const PlanetOccluder& o = p.occluders.at(i);
        f.occluders.at(i) = math::vec4(o.center - p.center, o.radius);
        f.occluder_albedo.at(i) = math::vec4(o.albedo, o.draw ? 1.0f : 0.0f);
    }
    f.march = p.march;
    f.march_steps = std::max<std::uint32_t>(p.march_steps, 1);
}

}  // namespace

SkyResources AtmosphereRenderer::record(RenderGraph& graph, const ResolvedSky& sky, const RenderView& view) {
    VEACH_ZONE("AtmosphereRenderer::record");
    Impl& s = *impl_;
    s.release_frame_buffers();  // last frame's: their submission is made, so destroy() waits for the GPU
    rhi::Device& device = *s.device;
    if (auto aerial = s.ensure_aerial(view.width, view.height); !aerial) {
        log::error("render", aerial.error());
        return {};
    }
    SkyFrame f;
    f.atmosphere = atmosphere_params(sky.settings);
    f.sun = math::vec4(sky.sun_direction, sky.sun_illuminance_top);
    const float altitude = std::max(static_cast<float>(view.height_in_frame), 0.0f) * 0.001f;
    f.camera = math::vec4(altitude, 0.0f, 0.0f, 0.0f);
    f.constant = math::vec4(sky.settings.color, 0.0f);
    f.inverse_view_projection = view.inverse_view_projection;
    f.mode = sky.mode == SkyMode::physical ? 2u : sky.mode == SkyMode::constant ? 1u : 0u;
    f.aerial_on = sky.settings.aerial_perspective ? 1u : 0u;
    f.aerial_slices = aerial_slices;
    // The Sky's flat world as a planet block, for the aerial-perspective volume: the center straight below the
    // camera (world +Y is up), one sun of color 1.
    f.origin = math::vec4(0.0f, f.atmosphere.planet.x + std::max(altitude, 0.001f), 0.0f, std::max(altitude, 0.001f));
    f.frame_x = math::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    f.sun_count = 1;
    f.sun_directions[0] = math::vec4(sky.sun_direction, 0.004625f);
    f.sun_colors[0] = math::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    if (sky.planet && sky.mode == SkyMode::physical) {
        fill_planet(f, *sky.planet);
    }
    f.occlusion = math::vec4(std::clamp(view.sky_visibility.visibility, 0.0f, 1.0f),
                             std::max(view.sky_visibility.reach, 0.0f) * 0.001f, 0.0f, 0.0f);

    // Only a physical sky reads (and builds) the medium's tables.
    const AtmosphereLutCache::Slot slot =
        sky.mode == SkyMode::physical ? s.cache.acquire(f.atmosphere) : AtmosphereLutCache::Slot{};
    Impl::MediumTables& tables = s.media.at(slot.index);
    f.transmittance = device.descriptor(tables.transmittance.handle);
    f.multiscattering = device.descriptor(tables.multiscattering.handle);
    f.sky_view = device.descriptor(s.sky_view.handle);
    f.aerial = device.descriptor(s.aerial.handle);
    f.irradiance = device.descriptor(s.irradiance.handle);
    f.linear_clamp = device.descriptor(s.linear_clamp);
    f.linear_repeat = device.descriptor(s.linear_wrap);  // the backdrop's, when it draws, is the same

    if (sky.stars) {
        // One pixel's angle at the image's center: 2 tan(fov / 2) / height.
        const float pixel = view.height > 0 ? 2.0f / (view.projection[1][1] * static_cast<float>(view.height)) : 0.0f;
        f.stars = math::vec4(sky.stars->brightness, pixel, sky.stars->limiting_magnitude,
                             static_cast<float>(std::max<std::uint32_t>(sky.stars->cells, 1)));
    }

    // The space backdrop (ADR 0091): bake it if it changed, and put the galactic frame and the camera's axes into
    // the frame (render space turns with the camera's reference frame).
    std::optional<GraphResource> backdrop;
    if (sky.backdrop && sky.backdrop_settings.visible) {
        backdrop = s.backdrop.record(graph, *sky.backdrop);
    }
    if (backdrop) {
        const Backdrop& b = *sky.backdrop;
        const math::mat3 to_galactic = math::transpose(b.orientation);  // rows: the galactic axes in render space
        const float pixel = view.height > 0 ? 2.0f / (view.projection[1][1] * static_cast<float>(view.height)) : 1e-3f;
        const math::mat3 world_from_view = math::transpose(math::mat3(view.view));
        f.galactic_x = math::vec4(b.orientation[0], pixel);
        f.galactic_y = math::vec4(b.orientation[1], sky.backdrop_settings.exposure_compensation);
        f.galactic_z = math::vec4(b.orientation[2], std::clamp(sky.backdrop_settings.exposure_follow, 0.0f, 1.0f));
        f.camera_right = math::vec4(to_galactic * world_from_view[0], 0.0f);
        f.camera_up = math::vec4(to_galactic * world_from_view[1], 0.0f);
        f.backdrop_stars = math::vec4(b.star_density, b.star_magnitude_scale, b.star_saturation, backdrop_sky_veil);
        const std::uint32_t width = backdrop_width(b);
        f.backdrop_info = {b.seed, width, width / 2, 1u};
        f.backdrop = device.descriptor(s.backdrop.texture());
        f.linear_repeat = device.descriptor(s.backdrop.sampler());
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
    const auto import = [&](Impl::Target& t, const char* name) {
        return graph.import(t.handle, name, t.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
                            rhi::ResourceState::shader_read);
    };
    const GraphResource transmittance = import(tables.transmittance, transmittance_lut.name);
    const GraphResource multiscattering = import(tables.multiscattering, multiscattering_lut.name);
    const GraphResource sky_view = import(s.sky_view, sky_view_lut.name);
    const GraphResource irradiance = import(s.irradiance, irradiance_lut.name);
    const GraphResource aerial = import(s.aerial, "aerial perspective");

    if (sky.mode == SkyMode::physical) {
        const auto pass = [&](const Lut& lut, rhi::PipelineHandle pipeline, Impl::Target& target, GraphResource out,
                              std::initializer_list<GraphResource> inputs) {
            AtmosphereLutParams params{.sky = device.descriptor(*buffer),
                                       .output = device.descriptor(target.handle, rhi::View::storage),
                                       .size = {lut.width, lut.height}};
            const std::uint32_t group = &lut == &irradiance_lut ? 1u : 8u;
            const std::vector<GraphResource> reads(inputs);
            graph.add_compute_pass(
                lut.name,
                [&](PassBuilder& b) {
                    b.read(frame);
                    for (const GraphResource input : reads) {
                        b.read(input);
                    }
                    b.write(out, rhi::ResourceState::shader_write);
                },
                [params, pipeline, group](PassContext& ctx) {
                    ctx.cmd.set_pipeline(pipeline);
                    ctx.cmd.set_parameters(params);
                    ctx.cmd.dispatch((params.size[0] + group - 1) / group, (params.size[1] + group - 1) / group);
                });
            target.written = true;
        };
        // The transmittance and multiple-scattering tables depend only on the medium: built when the cache says.
        if (slot.build) {
            pass(transmittance_lut, s.transmittance_pipeline, tables.transmittance, transmittance, {});
            pass(multiscattering_lut, s.multiscattering_pipeline, tables.multiscattering, multiscattering,
                 {transmittance});
        }
        pass(sky_view_lut, s.sky_view_pipeline, s.sky_view, sky_view, {transmittance, multiscattering});
        pass(irradiance_lut, s.irradiance_pipeline, s.irradiance, irradiance, {sky_view});
        const AerialVolumeParams params{.sky = device.descriptor(*buffer),
                                        .output = device.descriptor(s.aerial.handle, rhi::View::storage),
                                        .size = {s.aerial_size[0], s.aerial_size[1], aerial_slices, 0}};
        graph.add_compute_pass(
            "aerial perspective",
            [&](PassBuilder& b) {
                b.read(frame);
                b.read(transmittance);
                b.read(multiscattering);
                b.write(aerial, rhi::ResourceState::shader_write);
            },
            [params, pipeline = s.aerial_pipeline](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(params);
                ctx.cmd.dispatch((params.size[0] + 7) / 8, (params.size[1] + 7) / 8);
            });
        s.aerial.written = true;
    }

    const std::array<GraphResource, 5> targets{transmittance, multiscattering, sky_view, irradiance, aerial};
    return {*buffer, [frame, targets, backdrop](PassBuilder& b) {
                b.read(frame);
                for (const GraphResource& t : targets) {
                    b.read(t);
                }
                if (backdrop) {
                    b.read(*backdrop);
                }
            }};
}

}  // namespace veach::render
