#include "shadows.hpp"

#include <veach/core/env.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include "gpu_cull.hpp"
#include "veach_render_shaders.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <numeric>
#include <string>

namespace veach::render {

namespace {

// Filtering and bias, in texels of each cascade (ADR 0095): measured on Bistro, the slice and the field.
constexpr float normal_offset_texels = 1.5f;
constexpr float depth_bias_texels = 0.5f;
constexpr float max_penumbra_texels = 6.0f;
constexpr float cascade_blend = 0.1f;  ///< the share of a cascade's far distance over which it blends into the next
/// Casters whose bounding sphere is smaller than this many texels of a cascade are left out of it: their shadow
/// would be under a texel (the far cascades' small details).
constexpr float small_caster_texels = 0.35f;
constexpr float fade_share = 0.2f;  ///< the last cascade fades out over this share of the distance
/// A cached cascade's sphere is this many times its slice's: the camera can move by the difference before the cached
/// map stops covering the slice (12.5% wider texels in the far cascades, which are 35 cm and more at 2048^2).
constexpr float cache_radius_scale = 1.125f;
/// Rasterized bias on the casters (reverse-Z: negative pushes them away from the light), against the acne the
/// receiver's own offsets leave on steep slopes.
constexpr rhi::DepthBias caster_bias{.constant = 0, .slope = -1.5f, .clamp = 0.0f};

/// Mirrors PyramidParams in shaders/shadow_pyramids.slang.
struct alignas(16) PyramidParams {
    math::mat4 inverse_view_projection{1.0f};
    rhi::DescriptorHandle depth;
    rhi::DescriptorHandle shadows;
    rhi::DescriptorHandle pyramids;
    rhi::DescriptorHandle map;
    std::array<std::uint32_t, 2> size{};
    std::uint32_t mask_size = 0;
    std::uint32_t levels = 0;
    std::uint32_t layer_stride = 0;
    std::uint32_t count = 0;
    std::uint32_t level = 0;
    float margin_texels = 0.0f;
    float depth_margin = 0.0f;
    std::uint32_t first_layer = 0;
    std::uint32_t map_size = 0;
    std::uint32_t receivers = 0;
    std::uint32_t map_layer = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
    std::uint32_t pad2 = 0;
};
static_assert(rhi::parameters_match<PyramidParams>(veach_render_shaders::layout::shadow_pyramids,
                                                   {VEACH_PARAM(PyramidParams, inverse_view_projection),
                                                    VEACH_PARAM(PyramidParams, depth),
                                                    VEACH_PARAM(PyramidParams, shadows),
                                                    VEACH_PARAM(PyramidParams, pyramids),
                                                    VEACH_PARAM(PyramidParams, map),
                                                    VEACH_PARAM(PyramidParams, size),
                                                    VEACH_PARAM(PyramidParams, mask_size),
                                                    VEACH_PARAM(PyramidParams, levels),
                                                    VEACH_PARAM(PyramidParams, layer_stride),
                                                    VEACH_PARAM(PyramidParams, count),
                                                    VEACH_PARAM(PyramidParams, level),
                                                    VEACH_PARAM(PyramidParams, margin_texels),
                                                    VEACH_PARAM(PyramidParams, depth_margin),
                                                    VEACH_PARAM(PyramidParams, first_layer),
                                                    VEACH_PARAM(PyramidParams, map_size),
                                                    VEACH_PARAM(PyramidParams, receivers),
                                                    VEACH_PARAM(PyramidParams, map_layer),
                                                    VEACH_PARAM(PyramidParams, pad0),
                                                    VEACH_PARAM(PyramidParams, pad1),
                                                    VEACH_PARAM(PyramidParams, pad2)}));

/// The culling pyramids' level 0 has a texel per 16 x 16 texels of the map (16 to 256 per side), so a lookup's reach
/// (receiver_margin_texels) touches at most 2 x 2 of them.
constexpr std::uint32_t pyramid_texels = 16;
/// How far from its receiver a lookup reads the map, in texels: the blocker search and the widest PCF disk
/// (max_penumbra_texels), bilinear taps, the normal offset, and one texel to spare.
constexpr float receiver_margin_texels = max_penumbra_texels + 1.0f + normal_offset_texels + 1.5f;
/// How much farther from the light than its receiver a lookup's point may be: the normal offset, and one to spare.
constexpr float receiver_depth_margin_texels = normal_offset_texels + 1.0f;

float luminance(math::vec3 c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

}  // namespace

std::array<float, max_shadow_cascades> cascade_splits(float distance, std::uint32_t count) {
    count = std::clamp(count, 1u, max_shadow_cascades);
    std::array<float, max_shadow_cascades> splits{};
    float far = distance;
    for (std::uint32_t c = count; c-- > 0;) {
        splits.at(c) = far;
        far *= 0.25f;
    }
    return splits;
}

CascadeFit fit_cascade(const RenderView& view, const math::dvec3& origin, math::vec3 l, float near, float far,
                       std::uint32_t resolution, float toward_light, float radius_scale) {
    // The slice's bounding sphere: on the view axis, through its far corners (and its near ones when it fits both).
    const float tan_x = 1.0f / view.projection[0][0];
    const float tan_y = 1.0f / view.projection[1][1];
    const float k2 = tan_x * tan_x + tan_y * tan_y;
    float along = 0.5f * (near + far) * (1.0f + k2);
    float radius = 0.0f;
    if (along >= far) {
        along = far;
        radius = far * std::sqrt(k2);
    } else {
        radius = std::sqrt((far - along) * (far - along) + far * far * k2);
    }
    radius *= radius_scale;
    const math::mat4 world_from_view = math::inverse(view.view);
    const math::vec3 forward = -math::normalize(math::vec3(world_from_view[2]));
    const math::vec3 eye(world_from_view[3]);
    math::vec3 center = eye + forward * along;

    // The light's axes depend only on its direction: turning the camera never rotates the map.
    l = math::normalize(l);
    const math::vec3 reference = std::abs(l.y) < 0.99f ? math::vec3(0.0f, 1.0f, 0.0f) : math::vec3(1.0f, 0.0f, 0.0f);
    const math::vec3 x = math::normalize(math::cross(reference, l));
    const math::vec3 y = math::cross(l, x);

    // Snap the center to a whole texel of a grid fixed in the render frame (in double: render space moves by whole
    // 64 m cells, which are not whole texels).
    const float texel = 2.0f * radius / static_cast<float>(resolution);
    const math::dvec3 absolute = origin + math::dvec3(center);
    const auto snap = [&](math::vec3 axis) {
        const double u = math::dot(absolute, math::dvec3(axis));
        const double t = static_cast<double>(texel);
        return static_cast<float>(std::floor(u / t + 0.5) * t - u);
    };
    center += x * snap(x) + y * snap(y);

    // Depth: 0 at radius behind the center (away from the light), 1 at twice the radius toward it or `toward_light`
    // if farther; casters beyond are flattened onto 1 by shadow_vs.
    const float range = radius + std::max(2.0f * radius, toward_light);
    CascadeFit fit;
    fit.center = center;
    fit.x = x;
    fit.y = y;
    fit.radius = radius;
    fit.texel = texel;
    fit.depth_range = range;
    math::mat4 m(0.0f);
    const math::vec3 rows[3] = {x / radius, y / radius, l / range};
    const float offsets[3] = {-math::dot(center, x) / radius, -math::dot(center, y) / radius,
                              (radius - math::dot(center, l)) / range};
    for (int r = 0; r < 3; ++r) {
        m[0][r] = rows[r].x;
        m[1][r] = rows[r].y;
        m[2][r] = rows[r].z;
        m[3][r] = offsets[r];
    }
    m[3][3] = 1.0f;
    fit.clip_from_render = m;
    fit.planes = {math::vec4(x, radius - math::dot(center, x)), math::vec4(-x, radius + math::dot(center, x)),
                  math::vec4(y, radius - math::dot(center, y)), math::vec4(-y, radius + math::dot(center, y)),
                  math::vec4(l, radius - math::dot(center, l))};
    return fit;
}

std::array<math::vec4, 6> receiver_planes(const RenderView& view, float near, float far, float margin) {
    const math::mat4& m = view.view_projection;
    const auto row = [&](int i) {
        return math::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
    };
    std::array<math::vec4, 6> planes{row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1), {}, {}};
    for (std::size_t i = 0; i < 4; ++i) {
        planes.at(i) /= math::length(math::vec3(planes.at(i)));
    }
    const math::mat4 world_from_view = math::inverse(view.view);
    const math::vec3 forward = -math::normalize(math::vec3(world_from_view[2]));
    const float eye = math::dot(forward, math::vec3(world_from_view[3]));
    planes[4] = math::vec4(forward, -(eye + near));
    planes[5] = math::vec4(-forward, eye + far);
    for (math::vec4& p : planes) {
        p.w += margin;
    }
    return planes;
}

std::vector<std::uint32_t> shadowed_lights(std::span<const RenderLight> lights, std::uint32_t max) {
    std::vector<std::uint32_t> found;
    for (std::uint32_t i = 0; i < lights.size(); ++i) {
        if (lights[i].kind == LightKind::directional && luminance(lights[i].radiance) > 0.0f &&
            std::isfinite(luminance(lights[i].radiance))) {
            found.push_back(i);
        }
    }
    std::ranges::stable_sort(found, [&](std::uint32_t a, std::uint32_t b) {
        return luminance(lights[a].radiance) > luminance(lights[b].radiance);
    });
    found.resize(std::min<std::size_t>(found.size(), std::min(max, max_shadowed_lights)));
    return found;
}

struct ShadowRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle cull_reset;
    rhi::PipelineHandle cull_instances;
    rhi::PipelineHandle cull_meshlets;
    rhi::PipelineHandle cull_recheck;
    rhi::PipelineHandle cull_args;
    rhi::PipelineHandle casters_opaque;
    rhi::PipelineHandle casters_masked;
    rhi::PipelineHandle receivers_clear;
    rhi::PipelineHandle receivers_mark;
    rhi::PipelineHandle pyramid_reduce;
    rhi::PipelineHandle occluders_build;
    rhi::SamplerHandle compare;

    struct Buffer {
        rhi::BufferHandle handle;
        std::uint64_t size = 0;
        bool written = false;
    };
    /// Per cascade (light slot x cascade): its culling's counters, records and draw lists (both phases').
    struct CascadeBuffers {
        Buffer counters;
        Buffer draw_args;
        Buffer dispatch_args;
        Buffer tasks;
        Buffer recheck_instances;
        Buffer recheck_meshlets;
        Buffer visible;
    };
    std::array<CascadeBuffers, max_shadowed_lights * max_shadow_cascades> cascades;
    /// Every cascade's receiver pyramid, then every cascade's occluder pyramid (shadow_pyramids.slang).
    Buffer pyramids;
    /// What each layer's occluder pyramid was built from: last frame's cascade, for phase 1's occlusion test.
    struct Previous {
        math::mat4 clip_from_render{1.0f};
        math::dvec3 origin{0.0};
        bool valid = false;
    };
    std::array<Previous, max_shadowed_lights * max_shadow_cascades> previous{};
    /// A cached cascade (the far ones): the fit its map and occluder pyramid were last drawn whole with.
    struct Cached {
        CascadeFit fit;
        bool valid = false;
    };
    std::array<Cached, max_shadowed_lights * max_shadow_cascades> cached{};
    /// What the frame before rendered with: a cached cascade holds only while none of it changed.
    struct Last {
        std::uint64_t space_frame = 0;
        math::dvec3 origin{0.0};
        std::vector<std::uint32_t> lights;
        std::array<math::vec3, max_shadowed_lights> towards{};
        std::uint32_t resolution = 0;
        std::uint32_t cascade_count = 0;
        float distance = 0.0f;
        bool valid = false;
    };
    Last last;
    std::uint32_t pyramid_layers = 0;  ///< the layer count and size the occluder pyramids were built for
    std::uint32_t pyramid_size = 0;
    /// Per frame slot: a CullView per cascade, and the ShadowFrame the shading reads.
    std::vector<std::array<rhi::BufferHandle, max_shadowed_lights * max_shadow_cascades>> views;
    std::vector<rhi::BufferHandle> frames;

    /// The shadow maps: one depth32 2D array, a layer per cascade. One set serves every frame in flight (a set per
    /// frame slot measured no faster in Bistro and costs 64 MB more at 4 x 2048^2).
    struct Map {
        rhi::TextureHandle texture;
        std::uint32_t resolution = 0;
        std::uint32_t layers = 0;
        bool written = false;
    };
    Map map;

    Impl() = default;
    ~Impl() {
        if (device == nullptr) {
            return;
        }
        for (CascadeBuffers& c : cascades) {
            for (Buffer* b : {&c.counters, &c.draw_args, &c.dispatch_args, &c.tasks, &c.recheck_instances,
                              &c.recheck_meshlets, &c.visible}) {
                if (b->handle.is_valid()) {
                    device->destroy(b->handle);
                }
            }
        }
        if (pyramids.handle.is_valid()) {
            device->destroy(pyramids.handle);
        }
        for (const auto& slot : views) {
            for (const rhi::BufferHandle b : slot) {
                if (b.is_valid()) {
                    device->destroy(b);
                }
            }
        }
        for (const rhi::BufferHandle b : frames) {
            device->destroy(b);
        }
        if (map.texture.is_valid()) {
            device->destroy(map.texture);
        }
        if (compare.is_valid()) {
            device->destroy(compare);
        }
        for (rhi::PipelineHandle* p :
             {&cull_reset, &cull_instances, &cull_meshlets, &cull_recheck, &cull_args, &casters_opaque, &casters_masked,
              &receivers_clear, &receivers_mark, &pyramid_reduce, &occluders_build}) {
            if (p->is_valid()) {
                device->destroy(*p);
            }
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    /// `b` holds at least `size` bytes. `keep`: a grown buffer is a new one, its contents undefined (`written` false).
    Result<> ensure(Buffer& b, std::uint64_t size, rhi::BufferUsage usage, std::uint32_t stride, const char* name) {
        size = std::max<std::uint64_t>(size, 16);
        if (b.handle.is_valid() && b.size >= size) {
            return {};
        }
        if (b.handle.is_valid()) {
            device->destroy(b.handle);
            b.handle = {};
        }
        const std::uint64_t grown = b.size == 0 ? size : std::max(size, b.size + b.size / 2);
        auto buffer = device->create_buffer({.size = grown, .usage = usage, .debug_name = name, .stride = stride});
        if (!buffer) {
            b.size = 0;
            return std::unexpected(buffer.error());
        }
        b.handle = *buffer;
        b.size = grown;
        b.written = false;
        return {};
    }

    GraphResource import(RenderGraph& graph, Buffer& b, const char* name) {
        const GraphResource r =
            graph.import(b.handle, name, b.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
                         rhi::ResourceState::shader_read);
        b.written = true;
        return r;
    }

    Result<> ensure_map(std::uint32_t resolution, std::uint32_t layers) {
        if (map.texture.is_valid() && map.resolution == resolution && map.layers == layers) {
            return {};
        }
        if (map.texture.is_valid()) {
            device->destroy(map.texture);
            map.texture = {};
        }
        auto texture = device->create_texture({.width = resolution,
                                               .height = resolution,
                                               .format = rhi::Format::depth32_float,
                                               .usage = rhi::TextureUsage::depth_target | rhi::TextureUsage::sampled,
                                               .debug_name = "sun shadow maps",
                                               .type = rhi::TextureType::d2_array,
                                               .layers = layers});
        if (!texture) {
            map.resolution = map.layers = 0;
            return std::unexpected(texture.error());
        }
        map.texture = *texture;
        map.resolution = resolution;
        map.layers = layers;
        map.written = false;
        return {};
    }
};

ShadowRenderer::ShadowRenderer() : impl_(std::make_unique<Impl>()) {}

ShadowRenderer::~ShadowRenderer() = default;

Result<> ShadowRenderer::prepare(rhi::Device& device, const SceneShaders& shaders) {
    Impl& s = *impl_;
    s.device = &device;
    for (const auto& [library, entry, group, out] :
         {std::tuple{&veach_render_shaders::gpu_cull(), "cull_reset", std::array<std::uint32_t, 3>{64, 1, 1},
                     &s.cull_reset},
          std::tuple{&veach_render_shaders::gpu_cull(), "cull_instances", std::array<std::uint32_t, 3>{64, 1, 1},
                     &s.cull_instances},
          std::tuple{&veach_render_shaders::gpu_cull(), "cull_meshlets", std::array<std::uint32_t, 3>{64, 1, 1},
                     &s.cull_meshlets},
          std::tuple{&veach_render_shaders::gpu_cull(), "cull_recheck_meshlets", std::array<std::uint32_t, 3>{64, 1, 1},
                     &s.cull_recheck},
          std::tuple{&veach_render_shaders::gpu_cull(), "cull_args", std::array<std::uint32_t, 3>{1, 1, 1},
                     &s.cull_args},
          std::tuple{&veach_render_shaders::shadow_pyramids(), "receivers_clear",
                     std::array<std::uint32_t, 3>{64, 1, 1}, &s.receivers_clear},
          std::tuple{&veach_render_shaders::shadow_pyramids(), "receivers_mark",
                     std::array<std::uint32_t, 3>{16, 16, 1}, &s.receivers_mark},
          std::tuple{&veach_render_shaders::shadow_pyramids(), "pyramid_reduce", std::array<std::uint32_t, 3>{8, 8, 1},
                     &s.pyramid_reduce},
          std::tuple{&veach_render_shaders::shadow_pyramids(), "occluders_build", std::array<std::uint32_t, 3>{8, 8, 1},
                     &s.occluders_build}}) {
        auto pipeline = device.create_compute_pipeline({library, entry, group, std::string("shadow ") + entry});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    for (const auto& [vertex, fragment, out] : {std::tuple{"shadow_vs_opaque", "shadow_fs", &s.casters_opaque},
                                                std::tuple{"shadow_vs", "shadow_fs_masked", &s.casters_masked}}) {
        rhi::GraphicsPipelineDesc desc;
        desc.library = &shaders.visibility;
        desc.vertex_entry = vertex;
        desc.fragment_entry = fragment;
        desc.depth_format = rhi::Format::depth32_float;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.depth_compare = rhi::CompareOp::greater;  // reverse-Z: nearer the light is greater
        desc.cull = rhi::CullMode::none;               // every surface casts from both sides, as in photo mode
        desc.depth_bias = caster_bias;
        desc.debug_name = std::string("shadow casters ") + fragment;
        auto pipeline = device.create_graphics_pipeline(desc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    auto sampler = device.create_sampler(
        {.filter = rhi::Filter::linear, .address = rhi::AddressMode::clamp, .compare = rhi::CompareOp::greater_equal});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.compare = *sampler;
    for (std::uint32_t i = 0; i < device.frames_in_flight(); ++i) {
        auto frame = device.create_buffer({.size = sizeof(ShadowFrame),
                                           .memory = rhi::MemoryType::upload,
                                           .debug_name = "shadow frame",
                                           .stride = sizeof(ShadowFrame)});
        if (!frame) {
            return std::unexpected(frame.error());
        }
        s.frames.push_back(*frame);
        s.views.emplace_back();
        for (rhi::BufferHandle& view : s.views.back()) {
            auto buffer = device.create_buffer({.size = sizeof(CullView),
                                                .memory = rhi::MemoryType::upload,
                                                .debug_name = "shadow cull view",
                                                .stride = sizeof(CullView)});
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            view = *buffer;
        }
    }
    return {};
}

ShadowRenderer::Output ShadowRenderer::record(RenderGraph& graph, const FrameContext& frame, const RenderView& view,
                                              const RealtimeScene& scene, GraphResource depth,
                                              const math::mat4& inverse_depth_projection, CullStats& stats) {
    VEACH_ZONE("ShadowRenderer::record");
    Impl& s = *impl_;
    const std::uint32_t slot = s.device->current_frame().slot % static_cast<std::uint32_t>(s.frames.size());
    Output out{.frame = s.frames.at(slot)};

    ShadowFrame shadow;
    shadow.map = s.device->null_descriptor(rhi::DescriptorKind::texture_2d_array);
    shadow.compare = s.device->descriptor(s.compare);
    const SunShadows& settings = view.shadows;
    const std::vector<std::uint32_t> lights = scene.instance_count == 0 || !(settings.distance > 0.0f)
                                                  ? std::vector<std::uint32_t>{}
                                                  : shadowed_lights(frame.world.lights, settings.suns);
    const std::uint32_t cascade_count = std::clamp(settings.cascades, 1u, max_shadow_cascades);
    const std::uint32_t resolution = std::clamp(settings.resolution, 256u, 4096u);
    const auto layers = static_cast<std::uint32_t>(lights.size()) * cascade_count;
    const auto write_frame = [&] {
        if (void* mapped = s.device->map(out.frame); mapped != nullptr) {
            std::memcpy(mapped, &shadow, sizeof(shadow));
        }
    };
    if (layers == 0) {
        stats.shadow_meshlets_drawn.clear();
        for (Impl::Previous& p : s.previous) {
            p.valid = false;
        }
        for (Impl::Cached& c : s.cached) {
            c.valid = false;
        }
        s.last.valid = false;
        write_frame();
        return out;
    }
    Impl::Map& maps = s.map;
    if (auto ready = s.ensure_map(resolution, layers); !ready) {
        log::error("render", ready.error());
        write_frame();
        return out;
    }

    // The culling (ADR 0096): the receivers this frame's pixels look up in each cascade (from the camera's depth),
    // and occlusion from the light in two phases, as the camera's (ADR 0093): phase 1 tests last frame's map of the
    // cascade, phase 2 what phase 1 rejected against this frame's. VEACH_SHADOW_NO_RECEIVERS=1 and
    // VEACH_SHADOW_NO_OCCLUSION=1 turn either off, to compare.
    const rhi::TextureDesc* depth_desc = graph.texture_desc(depth);
    const bool masked = depth_desc != nullptr && !env_is_one("VEACH_SHADOW_NO_RECEIVERS");
    const bool occlusion = !env_is_one("VEACH_SHADOW_NO_OCCLUSION");
    const std::uint32_t pyramid_size = std::clamp(std::bit_floor(resolution / pyramid_texels), 16u, 256u);
    const auto pyramid_levels = static_cast<std::uint32_t>(std::bit_width(pyramid_size));
    const std::uint32_t layer_stride = (4 * pyramid_size * pyramid_size - 1) / 3;  // every level, 1 x 1 included
    const std::uint32_t phases = occlusion ? 2 : 1;

    // Draw lists: the camera's layout (the same capacities), for each phase (cull_reset clears both phases' records).
    std::uint32_t meshlets = 0;
    std::array<std::uint32_t, draw_list_count> list_offsets{};
    for (std::uint32_t l = 0; l < draw_list_count; ++l) {
        list_offsets.at(l) = meshlets;
        meshlets += scene.list_capacity.at(l);
    }
    constexpr rhi::BufferUsage rw = rhi::BufferUsage::storage;
    constexpr auto u32 = static_cast<std::uint32_t>(sizeof(std::uint32_t));
    constexpr auto u64 = static_cast<std::uint32_t>(2 * sizeof(std::uint32_t));
    for (std::uint32_t k = 0; k < layers; ++k) {
        Impl::CascadeBuffers& c = s.cascades.at(k);
        for (const Result<>& ready :
             {s.ensure(c.counters, counter_count * u32, rw, u32, "shadow cull counters"),
              s.ensure(c.draw_args, 2 * draw_list_count * sizeof(rhi::DrawIndirectArgs),
                       rw | rhi::BufferUsage::indirect | rhi::BufferUsage::copy_src, u32, "shadow draw args"),
              s.ensure(c.dispatch_args, 4 * dispatch_record, rw | rhi::BufferUsage::indirect, u32,
                       "shadow dispatch args"),
              s.ensure(c.tasks, std::uint64_t{phases} * scene.task_count * u64, rw, u64, "shadow cull tasks"),
              s.ensure(c.recheck_instances, occlusion ? std::uint64_t{scene.instance_count} * u32 : 0, rw, u32,
                       "shadow recheck instances"),
              s.ensure(c.recheck_meshlets, occlusion ? std::uint64_t{meshlets} * u64 : 0, rw, u64,
                       "shadow recheck meshlets"),
              s.ensure(c.visible, std::uint64_t{phases} * meshlets * u64, rw, u64, "shadow draw lists")}) {
            if (!ready) {
                log::error("render", ready.error());
                write_frame();
                return out;
            }
        }
    }
    if (masked || occlusion) {
        if (auto ready = s.ensure(s.pyramids, 2ull * layers * layer_stride * u32, rw, u32, "shadow culling pyramids");
            !ready) {
            log::error("render", ready.error());
            write_frame();
            return out;
        }
    }
    // Last frame's occluder pyramids serve phase 1 only if they were built for these layers in this buffer.
    const bool pyramids_kept = s.pyramids.written && s.pyramid_layers == layers && s.pyramid_size == pyramid_size;
    if (!pyramids_kept || !maps.written) {
        for (Impl::Cached& c : s.cached) {
            c.valid = false;  // a cached map or pyramid is gone
        }
    }
    s.pyramid_layers = layers;
    s.pyramid_size = pyramid_size;

    const GraphResource map = graph.import(
        maps.texture, "sun shadow maps", maps.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
        rhi::ResourceState::shader_read);
    maps.written = true;
    out.map = map;
    shadow.map = s.device->descriptor(maps.texture);
    shadow.light_count = static_cast<std::uint32_t>(lights.size());
    shadow.cascade_count = cascade_count;
    shadow.fade_end = settings.distance;
    shadow.fade_start = settings.distance * (1.0f - fade_share);
    shadow.resolution = static_cast<float>(resolution);
    shadow.normal_offset = normal_offset_texels;
    shadow.depth_bias = depth_bias_texels;
    shadow.max_penumbra = max_penumbra_texels;
    shadow.blend = cascade_blend;
    {
        const math::mat4 world_from_view = math::inverse(view.view);
        const math::vec3 forward = -math::normalize(math::vec3(world_from_view[2]));
        shadow.view_axis = math::vec4(forward, -math::dot(forward, math::vec3(world_from_view[3])));
    }

    const std::array<float, max_shadow_cascades> splits = cascade_splits(settings.distance, cascade_count);
    shadow.splits = math::vec4(splits[0], splits[1], splits[2], splits[3]);
    const GraphResource instances = scene.instances;
    const rhi::BufferHandle materials = scene.materials;
    const std::array<std::uint32_t, draw_list_count> list_capacity = scene.list_capacity;
    const rhi::DescriptorHandle null_texture = s.device->null_descriptor(rhi::DescriptorKind::texture);
    // Every cascade's culling runs in the same compute passes (a dispatch per cascade, on buffers of its own): twenty
    // small dependent passes cost more in pipeline drains than their work.
    struct Cascade {
        std::uint32_t layer = 0;
        CascadeFit fit;
        bool masked = false;  ///< culled by this frame's receivers (not drawn whole for the cache)
        GraphResource view;
        GraphResource counters;
        GraphResource draw_args;
        GraphResource dispatch_args;
        GraphResource tasks;
        GraphResource recheck_instances;
        GraphResource recheck_meshlets;
        GraphResource visible;
    };
    std::vector<Cascade> cascades;
    const math::dvec3 origin = frame.world.space.origin;
    // Cached cascades (the last two of three or four): while nothing moved since the frame before, in the scene, the
    // render frame or the lights, a far cascade is drawn whole once, then kept while its map still covers the slice
    // it serves. Their spheres are cache_radius_scale times the slice's, always, so their texels never change size.
    std::array<math::vec3, max_shadowed_lights> towards{};
    for (std::uint32_t light = 0; light < lights.size(); ++light) {
        towards.at(light) = -frame.world.lights.at(lights[light]).direction;
    }
    const bool stable = !env_is_one("VEACH_SHADOW_NO_CACHE") && scene.still && s.last.valid &&
                        s.last.space_frame == frame.world.space.frame && s.last.origin == origin &&
                        s.last.lights == lights && s.last.towards == towards && s.last.resolution == resolution &&
                        s.last.cascade_count == cascade_count && s.last.distance == settings.distance;
    s.last = {.space_frame = frame.world.space.frame,
              .origin = origin,
              .lights = lights,
              .towards = towards,
              .resolution = resolution,
              .cascade_count = cascade_count,
              .distance = settings.distance,
              .valid = true};
    const std::uint32_t first_cached = cascade_count >= 3 ? cascade_count - 2 : cascade_count;
    stats.shadow_meshlets_drawn.resize(layers);
    for (std::uint32_t light = 0; light < lights.size(); ++light) {
        shadow.lights.at(light) = lights[light];
        const math::vec3 toward = towards.at(light);
        for (std::uint32_t c = 0; c < cascade_count; ++c) {
            const std::uint32_t layer = light * cascade_count + c;
            // Receivers: the slice of view depths this cascade serves, from where the previous one starts blending
            // into it. Its depth reaches the shadow distance toward the light, so casters between the cascade and the
            // light keep their depth order (occlusion) rather than all flattening onto its near plane.
            const float near = c == 0 ? view.near_plane : splits.at(c - 1) * (1.0f - cascade_blend);
            const bool cacheable = c >= first_cached;
            const CascadeFit fit = fit_cascade(view, origin, toward, near, splits.at(c), resolution, settings.distance,
                                               cacheable ? cache_radius_scale : 1.0f);
            Impl::Cached& cache = s.cached.at(layer);
            bool reuse = false;
            if (cacheable && stable && cache.valid) {
                // The slice's own sphere, and as far as its lookups reach, inside the cached map.
                const CascadeFit need =
                    fit_cascade(view, origin, toward, near, splits.at(c), resolution, settings.distance);
                const math::vec3 d = need.center - cache.fit.center;
                const float reach = need.radius + (receiver_margin_texels + 1.0f) * cache.fit.texel;
                reuse = std::max(std::abs(math::dot(d, cache.fit.x)), std::abs(math::dot(d, cache.fit.y))) + reach <=
                            cache.fit.radius &&
                        need.radius - math::dot(d, toward) <= cache.fit.radius;
            }
            const CascadeFit& used = reuse ? cache.fit : fit;
            shadow.cascades.at(layer) = {.clip_from_render = used.clip_from_render,
                                         .scale = math::vec4(used.texel, used.depth_range, used.radius, 0.0f)};
            if (reuse) {
                stats.shadow_meshlets_drawn.at(layer) = 0;  // its map and occluder pyramid stay as they are
                continue;
            }
            // Drawn whole (without the receivers) when it will be kept.
            const bool whole = cacheable && stable;
            cache = {.fit = fit, .valid = whole};
            // Phase 1's occluders: last frame's map of this layer, its view moved into this frame's render space.
            Impl::Previous& previous = s.previous.at(layer);
            const bool occlude = occlusion && pyramids_kept && previous.valid;
            const math::mat4 occlusion_view_projection =
                occlude ? previous.clip_from_render *
                              math::translate(math::mat4(1.0f), math::vec3(origin - previous.origin))
                        : fit.clip_from_render;
            previous = {.clip_from_render = fit.clip_from_render, .origin = origin, .valid = occlusion};
            const bool receivers = masked && !whole;
            CullView cull{
                .view_projection = fit.clip_from_render,
                .occlusion_view_projection = occlusion_view_projection,
                .planes = fit.planes,
                .camera = math::vec4(fit.center, 0.0f),
                .screen = {resolution, resolution},
                .flags = cull_shadow_view | (receivers ? cull_receivers : 0u) | (occlude ? cull_previous_hiz : 0u),
                .instance_count = scene.instance_count,
                .phase_stride = meshlets,
                .task_capacity = scene.task_count,
                .receiver_base = layer * layer_stride,
                .pyramid_size = pyramid_size,
                .pyramid_levels = pyramid_levels,
                .receiver_planes = receiver_planes(view, near, splits.at(c),
                                                   (max_penumbra_texels + normal_offset_texels + 2.0f) * fit.texel),
                .sweep = math::vec4(-toward, small_caster_texels * fit.texel),
                .occluder_base = (layers + layer) * layer_stride};
            if (whole) {
                // Every receiver the slice may hold later: the slice planes widened to the whole cached sphere.
                cull.receiver_planes = receiver_planes(view, near, splits.at(c), 2.0f * fit.radius);
            }
            std::copy(list_offsets.begin(), list_offsets.end(), cull.list_offsets.begin());
            const rhi::BufferHandle view_buffer = s.views.at(slot).at(layer);
            if (void* mapped = s.device->map(view_buffer); mapped != nullptr) {
                std::memcpy(mapped, &cull, sizeof(cull));
            }
            Impl::CascadeBuffers& b = s.cascades.at(layer);
            cascades.push_back({.layer = layer,
                                .fit = fit,
                                .masked = receivers,
                                .view = graph.import(view_buffer, "shadow cull view", rhi::ResourceState::shader_read),
                                .counters = s.import(graph, b.counters, "shadow cull counters"),
                                .draw_args = s.import(graph, b.draw_args, "shadow draw args"),
                                .dispatch_args = s.import(graph, b.dispatch_args, "shadow dispatch args"),
                                .tasks = s.import(graph, b.tasks, "shadow cull tasks"),
                                .recheck_instances = s.import(graph, b.recheck_instances, "shadow recheck instances"),
                                .recheck_meshlets = s.import(graph, b.recheck_meshlets, "shadow recheck meshlets"),
                                .visible = s.import(graph, b.visible, "shadow draw lists")});
        }
    }
    for (std::uint32_t k = layers; k < s.previous.size(); ++k) {
        s.previous.at(k).valid = false;
        s.cached.at(k).valid = false;
    }

    std::optional<GraphResource> pyramids;
    if (masked || occlusion) {
        pyramids = s.import(graph, s.pyramids, "shadow culling pyramids");
    }
    if (occlusion) {
        // The shading skips the filter where the occluders say a lookup is wholly in shadow.
        out.occluders = pyramids;
        shadow.occluders = s.device->descriptor(s.pyramids.handle);
        shadow.occluder_size = pyramid_size;
        shadow.occluder_stride = layer_stride;
        shadow.occluder_base = layers * layer_stride;
    }
    const GraphResource frame_data = graph.import(out.frame, "shadow frame", rhi::ResourceState::shader_read);
    const auto bind = [instances, materials, null_texture, pyramids](PassContext& ctx, const Cascade& c,
                                                                     std::uint32_t phase) {
        const auto storage = [&](GraphResource r) {
            return ctx.device.descriptor(ctx.buffer(r), rhi::View::storage);
        };
        CullParams p{.phase = phase, .bin_count = 0};
        p.view = ctx.device.descriptor(ctx.buffer(c.view));
        p.instances = ctx.device.descriptor(ctx.buffer(instances));
        p.materials = ctx.device.descriptor(materials);
        p.hiz = null_texture;  // a shadow view's occluders are in `pyramids`
        p.counters = storage(c.counters);
        p.draw_args = storage(c.draw_args);
        p.dispatch_args = storage(c.dispatch_args);
        p.tasks = storage(c.tasks);
        p.recheck_instances = storage(c.recheck_instances);
        p.recheck_meshlets = storage(c.recheck_meshlets);
        p.visible = storage(c.visible);
        p.bin_tiles = storage(c.counters);  // bin_count 0: never written
        p.pyramids = ctx.device.descriptor(ctx.buffer(pyramids ? *pyramids : c.view));
        ctx.cmd.set_parameters(p);
    };
    const PyramidParams pyramid_params{
        .inverse_view_projection = inverse_depth_projection,
        .size = {depth_desc != nullptr ? depth_desc->width : 0u, depth_desc != nullptr ? depth_desc->height : 0u},
        .mask_size = pyramid_size,
        .levels = pyramid_levels,
        .layer_stride = layer_stride,
        .count = layers * layer_stride,  // the receivers' half: the occluders stay for the next frame's phase 1
        .margin_texels = receiver_margin_texels,
        .depth_margin = receiver_depth_margin_texels,
        .map_size = resolution};
    const auto bind_pyramids = [pyramid_params, pyramids, depth, frame_data, masked, map](
                                   PassContext& ctx, std::uint32_t level, std::uint32_t first_layer,
                                   std::uint32_t map_layer = 0, bool receivers = false) {
        PyramidParams p = pyramid_params;
        p.level = level;
        p.first_layer = first_layer;
        p.map_layer = map_layer;
        p.receivers = receivers ? 1u : 0u;
        p.depth = masked ? ctx.device.descriptor(ctx.texture(depth))
                         : ctx.device.null_descriptor(rhi::DescriptorKind::texture);
        p.shadows = ctx.device.descriptor(ctx.buffer(frame_data));
        p.pyramids = ctx.device.descriptor(ctx.buffer(*pyramids), rhi::View::storage);
        p.map = ctx.device.descriptor(ctx.texture(map));
        ctx.cmd.set_parameters(p);
    };
    // A pyramid's levels above 0, four per pass (pyramid_reduce), for layers [first, first + layers).
    const auto reduce = [&](const char* name, std::uint32_t first_layer) {
        for (std::uint32_t level = 0; level + 1 < pyramid_levels; level += 4) {
            graph.add_compute_pass(
                std::format("{} {}", name, level + 1), [&](PassBuilder& pb) { pb.write(*pyramids); },
                [=, pipeline = s.pyramid_reduce](PassContext& ctx) {
                    ctx.cmd.set_pipeline(pipeline);
                    bind_pyramids(ctx, level, first_layer);
                    const std::uint32_t groups = std::max(1u, ((pyramid_size >> (level + 1)) + 7) / 8);
                    ctx.cmd.dispatch(groups, groups, layers);
                });
        }
    };

    enum class Step : std::uint8_t { reset, instances, args, meshlets };
    const auto cull_pass = [&](Step step, std::uint32_t phase, const std::string& name) {
        graph.add_compute_pass(
            name,
            [&](PassBuilder& pb) {
                const bool culls = step == Step::instances || step == Step::meshlets;
                if (culls) {
                    scene.read_meshes(pb);
                }
                if (pyramids && (culls || step == Step::reset)) {
                    if (step == Step::reset) {
                        pb.write(*pyramids);
                    } else {
                        pb.read(*pyramids);
                    }
                }
                for (const Cascade& c : cascades) {
                    if (culls) {
                        pb.read(c.view);
                        pb.write(c.tasks);
                        pb.write(c.recheck_instances);
                        pb.write(c.recheck_meshlets);
                        pb.write(c.visible);
                    }
                    if (step == Step::meshlets || (step == Step::instances && phase == 1)) {
                        pb.read(c.dispatch_args, rhi::ResourceState::indirect_argument);
                    } else if (step == Step::args) {
                        pb.write(c.dispatch_args);
                    }
                    pb.write(c.counters);
                    if (step != Step::args) {
                        pb.write(c.draw_args);
                    }
                }
            },
            [=,
             pipeline = step == Step::reset       ? s.cull_reset
                        : step == Step::instances ? s.cull_instances
                        : step == Step::args      ? s.cull_args
                                                  : s.cull_meshlets,
             recheck = s.cull_recheck, clear = s.receivers_clear, count = scene.instance_count](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                // The cascades' dispatches touch disjoint buffers: no barrier between them.
                for (const Cascade& c : cascades) {
                    bind(ctx, c, phase);
                    if (step == Step::instances) {
                        if (phase == 1) {
                            ctx.cmd.dispatch_indirect(ctx.buffer(c.dispatch_args), dispatch_recheck_instances);
                        } else {
                            const auto [x, y] = rows_of((count + 63) / 64);
                            ctx.cmd.dispatch(x, y);
                        }
                    } else if (step == Step::meshlets) {
                        ctx.cmd.dispatch_indirect(ctx.buffer(c.dispatch_args),
                                                  phase == 0 ? dispatch_tasks_1 : dispatch_tasks_2);
                    } else {
                        ctx.cmd.dispatch(1, 1);
                    }
                }
                if (step == Step::meshlets && phase == 1) {
                    // Appends to the same lists through atomics: no ordering needed between the two.
                    ctx.cmd.set_pipeline(recheck);
                    for (const Cascade& c : cascades) {
                        bind(ctx, c, 1);
                        ctx.cmd.dispatch_indirect(ctx.buffer(c.dispatch_args), dispatch_recheck_meshlets);
                    }
                }
                if (step == Step::reset && masked) {
                    ctx.cmd.set_pipeline(clear);
                    bind_pyramids(ctx, 0, 0);
                    const auto [x, y] = rows_of((pyramid_params.count + 63) / 64);
                    ctx.cmd.dispatch(x, y);
                }
            });
    };
    // Each cascade's casters of a phase: depth only, into its layer of the map.
    const auto draw = [&](std::uint32_t phase) {
        for (const Cascade& c : cascades) {
            const VisibilityParams params{
                .view_projection = c.fit.clip_from_render,
                .previous_view_projection = c.fit.clip_from_render,
                .inverse_size = {1.0f / static_cast<float>(resolution), 1.0f / static_cast<float>(resolution)}};
            graph.add_graphics_pass(
                std::format("shadow {}: casters{}", c.layer, phase == 0 ? "" : " (late)"),
                [&](PassBuilder& pb) {
                    pb.depth_target(map, phase == 0 ? rhi::LoadOp::clear : rhi::LoadOp::load, 0.0f,
                                    {.mip = 0, .layer = c.layer});  // reverse-Z
                    scene.read_geometry(pb);
                    pb.read(c.visible);
                    pb.read(c.draw_args, rhi::ResourceState::indirect_argument);
                },
                [=, opaque = s.casters_opaque, masked_casters = s.casters_masked, visible = c.visible,
                 draw_args = c.draw_args](PassContext& ctx) mutable {
                    VisibilityParams p = params;
                    p.instances = ctx.device.descriptor(ctx.buffer(instances));
                    p.materials = ctx.device.descriptor(materials);
                    p.visible = ctx.device.descriptor(ctx.buffer(visible));
                    rhi::PipelineHandle bound;
                    for (std::uint32_t l = 0; l < draw_list_count; ++l) {
                        if (list_capacity.at(l) == 0) {
                            continue;
                        }
                        const rhi::PipelineHandle pipeline = l < draw_bucket_count ? opaque : masked_casters;
                        if (pipeline != bound) {
                            ctx.cmd.set_pipeline(pipeline);
                            bound = pipeline;
                        }
                        p.list_offset = list_offsets.at(l) + phase * meshlets;
                        ctx.cmd.set_parameters(p);
                        ctx.cmd.draw_indirect(ctx.buffer(draw_args),
                                              (phase * draw_list_count + l) * sizeof(rhi::DrawIndirectArgs));
                    }
                });
        }
    };

    cull_pass(Step::reset, 0, "shadow: cull reset");
    if (masked) {
        // What this frame's pixels look up in each cascade, then the pyramid over it.
        graph.add_compute_pass(
            "shadow: receivers",
            [&](PassBuilder& pb) {
                pb.read(depth);
                pb.read(frame_data);
                pb.write(*pyramids);
            },
            [=, pipeline = s.receivers_mark](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                bind_pyramids(ctx, 0, 0);
                ctx.cmd.dispatch((pyramid_params.size[0] + 15) / 16, (pyramid_params.size[1] + 15) / 16);
            });
        reduce("shadow: receivers", 0);
    }
    cull_pass(Step::instances, 0, "shadow: cull instances");
    cull_pass(Step::args, 0, "shadow: cull args");
    cull_pass(Step::meshlets, 0, "shadow: cull meshlets");
    draw(0);
    if (occlusion) {
        // The occluders: phase 1's maps, phase 2's test now and next frame's phase 1.
        graph.add_compute_pass(
            "shadow: occluders",
            [&](PassBuilder& pb) {
                pb.read(map);
                pb.write(*pyramids);
            },
            [=, pipeline = s.occluders_build](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                // A cascade at a time: a cached one keeps its pyramid, one drawn whole for the cache ignores the
                // receivers (later frames have others). Disjoint pyramids: no barrier between the dispatches.
                for (const Cascade& c : cascades) {
                    bind_pyramids(ctx, 0, layers + c.layer, c.layer, c.masked);
                    ctx.cmd.dispatch(pyramid_size, pyramid_size, 1);
                }
            });
        reduce("shadow: occluders", layers);
        cull_pass(Step::args, 1, "shadow: cull args (late)");
        cull_pass(Step::instances, 1, "shadow: cull instances (late)");
        cull_pass(Step::args, 1, "shadow: cull args (late meshlets)");
        cull_pass(Step::meshlets, 1, "shadow: cull meshlets (late)");
        draw(1);
    }

    // What each cascade drew, both phases, for CullStats (MCP gpu_timings).
    if (frame.readbacks != nullptr) {
        for (const Cascade& c : cascades) {
            frame.readbacks->read(
                graph, c.draw_args, 2 * draw_list_count * sizeof(rhi::DrawIndirectArgs),
                [&stats, layer = c.layer, layers](Result<ReadbackData> data) {
                    if (!data || data->bytes.size() < 2 * draw_list_count * sizeof(rhi::DrawIndirectArgs)) {
                        return;
                    }
                    std::array<rhi::DrawIndirectArgs, 2 * draw_list_count> records{};
                    std::memcpy(records.data(), data->bytes.data(), sizeof(records));
                    stats.shadow_meshlets_drawn.resize(layers);
                    stats.shadow_meshlets_drawn.at(layer) = 0;
                    for (const rhi::DrawIndirectArgs& r : records) {
                        stats.shadow_meshlets_drawn.at(layer) += r.instance_count;
                    }
                },
                "shadow draw args readback");
        }
    }
    write_frame();
    return out;
}

}  // namespace veach::render
