#include "shadows.hpp"

#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include "gpu_cull.hpp"
#include "veach_render_shaders.hpp"

#include <algorithm>
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
/// Rasterized bias on the casters (reverse-Z: negative pushes them away from the light), against the acne the
/// receiver's own offsets leave on steep slopes.
constexpr rhi::DepthBias caster_bias{.constant = 0, .slope = -1.5f, .clamp = 0.0f};

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
                       std::uint32_t resolution) {
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

    // Depth: 0 at radius behind the center (away from the light), 1 at twice the radius toward it; casters beyond
    // are flattened onto 1 by shadow_vs.
    const float range = 3.0f * radius;
    CascadeFit fit;
    fit.center = center;
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
    rhi::PipelineHandle cull_args;
    rhi::PipelineHandle casters_opaque;
    rhi::PipelineHandle casters_masked;
    rhi::SamplerHandle compare;

    struct Buffer {
        rhi::BufferHandle handle;
        std::uint64_t size = 0;
        bool written = false;
    };
    /// Per cascade (light slot x cascade): its culling's counters, records and draw lists.
    struct CascadeBuffers {
        Buffer counters;
        Buffer draw_args;
        Buffer dispatch_args;
        Buffer tasks;
        Buffer visible;
    };
    std::array<CascadeBuffers, max_shadowed_lights * max_shadow_cascades> cascades;
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
            for (Buffer* b : {&c.counters, &c.draw_args, &c.dispatch_args, &c.tasks, &c.visible}) {
                if (b->handle.is_valid()) {
                    device->destroy(b->handle);
                }
            }
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
             {&cull_reset, &cull_instances, &cull_meshlets, &cull_args, &casters_opaque, &casters_masked}) {
            if (p->is_valid()) {
                device->destroy(*p);
            }
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

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
    const rhi::ShaderLibrary& cull = veach_render_shaders::gpu_cull();
    for (const auto& [entry, group, out] :
         {std::tuple{"cull_reset", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_reset},
          std::tuple{"cull_instances", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_instances},
          std::tuple{"cull_meshlets", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_meshlets},
          std::tuple{"cull_args", std::array<std::uint32_t, 3>{1, 1, 1}, &s.cull_args}}) {
        auto pipeline = device.create_compute_pipeline({&cull, entry, group, std::string("shadow ") + entry});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    for (const auto& [fragment, out] :
         {std::pair{"shadow_fs", &s.casters_opaque}, std::pair{"shadow_fs_masked", &s.casters_masked}}) {
        rhi::GraphicsPipelineDesc desc;
        desc.library = &shaders.visibility;
        desc.vertex_entry = "shadow_vs";
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
                                              const RealtimeScene& scene, CullStats& stats) {
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
        write_frame();
        return out;
    }
    Impl::Map& maps = s.map;
    if (auto ready = s.ensure_map(resolution, layers); !ready) {
        log::error("render", ready.error());
        write_frame();
        return out;
    }

    // Draw lists: the camera's layout (the same capacities), phase 1 only (cull_reset clears both phases' records).
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
              s.ensure(c.tasks, std::uint64_t{scene.task_count} * u64, rw, u64, "shadow cull tasks"),
              s.ensure(c.visible, std::uint64_t{meshlets} * u64, rw, u64, "shadow draw lists")}) {
            if (!ready) {
                log::error("render", ready.error());
                write_frame();
                return out;
            }
        }
    }

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
    // Every cascade's culling runs in the same four compute passes (a dispatch per cascade, on buffers of its own):
    // twenty small dependent passes cost more in pipeline drains than their work.
    struct Cascade {
        std::uint32_t layer = 0;
        CascadeFit fit;
        GraphResource view;
        GraphResource counters;
        GraphResource draw_args;
        GraphResource dispatch_args;
        GraphResource tasks;
        GraphResource visible;
    };
    std::vector<Cascade> cascades;
    for (std::uint32_t light = 0; light < lights.size(); ++light) {
        shadow.lights.at(light) = lights[light];
        const math::vec3 toward = -frame.world.lights.at(lights[light]).direction;
        for (std::uint32_t c = 0; c < cascade_count; ++c) {
            const std::uint32_t layer = light * cascade_count + c;
            // Receivers: the slice of view depths this cascade serves, from where the previous one starts blending
            // into it.
            const float near = c == 0 ? view.near_plane : splits.at(c - 1) * (1.0f - cascade_blend);
            const CascadeFit fit = fit_cascade(view, frame.world.space.origin, toward, near, splits.at(c), resolution);
            shadow.cascades.at(layer) = {.clip_from_render = fit.clip_from_render,
                                         .scale = math::vec4(fit.texel, fit.depth_range, fit.radius, 0.0f)};
            CullView cull{
                .view_projection = fit.clip_from_render,
                .occlusion_view_projection = fit.clip_from_render,
                .planes = fit.planes,
                .camera = math::vec4(fit.center, 0.0f),
                .screen = {resolution, resolution},
                .flags = cull_shadow_view,
                .instance_count = scene.instance_count,
                .phase_stride = 0,
                .task_capacity = scene.task_count,
                .receiver_planes = receiver_planes(view, near, splits.at(c),
                                                   (max_penumbra_texels + normal_offset_texels + 2.0f) * fit.texel),
                .sweep = math::vec4(-toward, small_caster_texels * fit.texel)};
            std::copy(list_offsets.begin(), list_offsets.end(), cull.list_offsets.begin());
            const rhi::BufferHandle view_buffer = s.views.at(slot).at(layer);
            if (void* mapped = s.device->map(view_buffer); mapped != nullptr) {
                std::memcpy(mapped, &cull, sizeof(cull));
            }
            Impl::CascadeBuffers& b = s.cascades.at(layer);
            cascades.push_back({.layer = layer,
                                .fit = fit,
                                .view = graph.import(view_buffer, "shadow cull view", rhi::ResourceState::shader_read),
                                .counters = s.import(graph, b.counters, "shadow cull counters"),
                                .draw_args = s.import(graph, b.draw_args, "shadow draw args"),
                                .dispatch_args = s.import(graph, b.dispatch_args, "shadow dispatch args"),
                                .tasks = s.import(graph, b.tasks, "shadow cull tasks"),
                                .visible = s.import(graph, b.visible, "shadow draw lists")});
        }
    }

    const auto bind = [instances, materials, null_texture](PassContext& ctx, const Cascade& c) {
        const auto storage = [&](GraphResource r) {
            return ctx.device.descriptor(ctx.buffer(r), rhi::View::storage);
        };
        CullParams p{.phase = 0, .bin_count = 0};
        p.view = ctx.device.descriptor(ctx.buffer(c.view));
        p.instances = ctx.device.descriptor(ctx.buffer(instances));
        p.materials = ctx.device.descriptor(materials);
        p.hiz = null_texture;  // no occlusion: cull_previous_hiz is never set
        p.counters = storage(c.counters);
        p.draw_args = storage(c.draw_args);
        p.dispatch_args = storage(c.dispatch_args);
        p.tasks = storage(c.tasks);
        p.recheck_instances = storage(c.counters);  // phase 1 without a pyramid rechecks nothing
        p.recheck_meshlets = storage(c.tasks);
        p.visible = storage(c.visible);
        p.bin_tiles = storage(c.counters);  // bin_count 0: never written
        ctx.cmd.set_parameters(p);
    };
    enum class Step : std::uint8_t { reset, instances, args, meshlets };
    for (const auto& [step, name] :
         {std::pair{Step::reset, "shadow: cull reset"}, std::pair{Step::instances, "shadow: cull instances"},
          std::pair{Step::args, "shadow: cull args"}, std::pair{Step::meshlets, "shadow: cull meshlets"}}) {
        graph.add_compute_pass(
            name,
            [&](PassBuilder& pb) {
                if (step == Step::instances || step == Step::meshlets) {
                    scene.read_meshes(pb);
                }
                for (const Cascade& c : cascades) {
                    if (step == Step::instances || step == Step::meshlets) {
                        pb.read(c.view);
                        pb.write(c.tasks);
                        pb.write(c.visible);
                    }
                    if (step == Step::meshlets) {
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
             count = scene.instance_count](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                // The cascades' dispatches touch disjoint buffers: no barrier between them.
                for (const Cascade& c : cascades) {
                    bind(ctx, c);
                    if (step == Step::instances) {
                        const auto [x, y] = rows_of((count + 63) / 64);
                        ctx.cmd.dispatch(x, y);
                    } else if (step == Step::meshlets) {
                        ctx.cmd.dispatch_indirect(ctx.buffer(c.dispatch_args), dispatch_tasks_1);
                    } else {
                        ctx.cmd.dispatch(1, 1);
                    }
                }
            });
    }

    // Each cascade's casters: depth only, into its layer of the map.
    for (const Cascade& c : cascades) {
        const VisibilityParams params{
            .view_projection = c.fit.clip_from_render,
            .previous_view_projection = c.fit.clip_from_render,
            .inverse_size = {1.0f / static_cast<float>(resolution), 1.0f / static_cast<float>(resolution)}};
        graph.add_graphics_pass(
            std::format("shadow {}: casters", c.layer),
            [&](PassBuilder& pb) {
                pb.depth_target(map, rhi::LoadOp::clear, 0.0f, {.mip = 0, .layer = c.layer});  // reverse-Z
                scene.read_geometry(pb);
                pb.read(c.visible);
                pb.read(c.draw_args, rhi::ResourceState::indirect_argument);
            },
            [=, opaque = s.casters_opaque, masked = s.casters_masked, visible = c.visible,
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
                    const rhi::PipelineHandle pipeline = l < draw_bucket_count ? opaque : masked;
                    if (pipeline != bound) {
                        ctx.cmd.set_pipeline(pipeline);
                        bound = pipeline;
                    }
                    p.list_offset = list_offsets.at(l);
                    ctx.cmd.set_parameters(p);
                    ctx.cmd.draw_indirect(ctx.buffer(draw_args), l * sizeof(rhi::DrawIndirectArgs));
                }
            });
        // What it drew, for CullStats (MCP gpu_timings).
        if (frame.readbacks != nullptr) {
            frame.readbacks->read(
                graph, c.draw_args, draw_list_count * sizeof(rhi::DrawIndirectArgs),
                [&stats, layer = c.layer, layers](Result<ReadbackData> data) {
                    if (!data || data->bytes.size() < draw_list_count * sizeof(rhi::DrawIndirectArgs)) {
                        return;
                    }
                    std::array<rhi::DrawIndirectArgs, draw_list_count> records{};
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
