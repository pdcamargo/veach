#include "sky_occlusion.hpp"

#include <veach/core/env.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/render/shadow_cascades.hpp>

#include "gpu_cull.hpp"
#include "veach_render_shaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace veach::render {

namespace {

/// Mirrors SkyParams in shaders/sky_occlusion.slang.
struct alignas(16) SkyParams {
    math::mat4 inverse_view_projection{1.0f};
    math::vec4 camera{0.0f};
    rhi::DescriptorHandle frame;
    rhi::DescriptorHandle shadows;
    rhi::DescriptorHandle sky;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle visibility;
    rhi::DescriptorHandle top;
    rhi::DescriptorHandle lowest;
    rhi::DescriptorHandle downward;
    rhi::DescriptorHandle heights;
    rhi::DescriptorHandle heights_out;
    rhi::DescriptorHandle ratios;
    rhi::DescriptorHandle distances;
    rhi::DescriptorHandle raw;
    rhi::DescriptorHandle near;
    std::array<std::uint32_t, 2> size{};
    std::array<std::uint32_t, 2> extent{};
};
static_assert(rhi::parameters_match<SkyParams>(
    veach_render_shaders::layout::sky_occlusion,
    {VEACH_PARAM(SkyParams, inverse_view_projection), VEACH_PARAM(SkyParams, camera), VEACH_PARAM(SkyParams, frame),
     VEACH_PARAM(SkyParams, shadows), VEACH_PARAM(SkyParams, sky), VEACH_PARAM(SkyParams, instances),
     VEACH_PARAM(SkyParams, visibility), VEACH_PARAM(SkyParams, top), VEACH_PARAM(SkyParams, lowest),
     VEACH_PARAM(SkyParams, downward), VEACH_PARAM(SkyParams, heights), VEACH_PARAM(SkyParams, heights_out),
     VEACH_PARAM(SkyParams, ratios), VEACH_PARAM(SkyParams, distances), VEACH_PARAM(SkyParams, raw),
     VEACH_PARAM(SkyParams, near), VEACH_PARAM(SkyParams, size), VEACH_PARAM(SkyParams, extent)}));

/// Full-resolution pixels per side of the cells the occlusion is traced in (veach/sky_occlusion.slang's sky_cell).
constexpr std::uint32_t sky_cell = 8;
/// Texels per side of the height map. At the default reach (200 m) a texel is half a meter.
constexpr std::uint32_t map_resolution = 1024;
/// Mips of the height pyramid: the trace's last steps (tens of meters apart) read mip 6 or 7.
constexpr std::uint32_t map_mips = 8;
/// The map reaches this far beyond where the occlusion has faded out, as a share of that distance.
constexpr float map_margin = 0.25f;
/// The occlusion fades out over the last quarter of its distance.
constexpr float fade_share = 0.25f;
/// Casters smaller than this many texels are left out of the map (grass blades, small props), as in the shadow
/// cascades.
constexpr float small_caster_texels = 0.5f;
/// The map is drawn again every this many frames (for what moves in it), or when its center moves: the center snaps
/// to the texels of the pyramid's coarsest mip (62.5 m at the default reach), so every mip covers the same ground
/// whichever frame drew it, and a still camera's image never depends on where the camera came from.
constexpr std::uint64_t refresh_frames = 60;
/// The map's layers, each a depth map from above.
constexpr std::size_t map_highest = 0;
constexpr std::size_t map_lowest = 1;
constexpr std::size_t map_downward = 2;  ///< the highest face turned down (a ceiling, a closed object's bottom)
constexpr std::array<const char*, 3> map_layer_names{"highest", "lowest", "downward"};
/// Downward faces are the back faces seen from above (glTF's counterclockwise front faces, axis_x x axis_y = up).
constexpr rhi::CullMode downward_faces = rhi::CullMode::front;

}  // namespace

SkyMapFit fit_sky_map(const math::vec3& center, const math::vec3& up, float half_width, std::uint32_t resolution) {
    SkyMapFit fit;
    fit.up = math::normalize(up);
    // The axes depend only on up: turning the camera never rotates the map.
    const math::vec3 reference =
        std::abs(fit.up.y) < 0.99f ? math::vec3(0.0f, 1.0f, 0.0f) : math::vec3(1.0f, 0.0f, 0.0f);
    fit.axis_x = math::normalize(math::cross(reference, fit.up));
    fit.axis_y = math::cross(fit.up, fit.axis_x);
    fit.center = center;
    fit.half_width = half_width;
    fit.texel = 2.0f * half_width / static_cast<float>(resolution);
    fit.below = half_width;
    fit.depth_range = 2.0f * half_width;
    // Depth 0 at the bottom (half_width below the center), 1 at the top; casters above are flattened onto it.
    math::mat4 m(0.0f);
    const math::vec3 rows[3] = {fit.axis_x / half_width, fit.axis_y / half_width, fit.up / fit.depth_range};
    const float offsets[3] = {-math::dot(center, fit.axis_x) / half_width, -math::dot(center, fit.axis_y) / half_width,
                              (fit.below - math::dot(center, fit.up)) / fit.depth_range};
    for (int r = 0; r < 3; ++r) {
        m[0][r] = rows[r].x;
        m[1][r] = rows[r].y;
        m[2][r] = rows[r].z;
        m[3][r] = offsets[r];
    }
    m[3][3] = 1.0f;
    fit.clip_from_render = m;
    const float cx = math::dot(center, fit.axis_x);
    const float cy = math::dot(center, fit.axis_y);
    fit.planes = {math::vec4(fit.axis_x, half_width - cx), math::vec4(-fit.axis_x, half_width + cx),
                  math::vec4(fit.axis_y, half_width - cy), math::vec4(-fit.axis_y, half_width + cy),
                  math::vec4(fit.up, fit.below - math::dot(center, fit.up))};
    return fit;
}

math::dvec3 snap_sky_map(const math::vec3& camera, const math::vec3& up, const math::dvec3& origin, float texel) {
    const SkyMapFit axes = fit_sky_map(camera, up, 1.0f, 1);
    const math::dvec3 absolute = origin + math::dvec3(camera);
    const auto snap = [&](math::vec3 axis) {
        const double u = math::dot(absolute, math::dvec3(axis));
        const auto t = static_cast<double>(texel);
        return std::floor(u / t + 0.5) * t - u;
    };
    return absolute + math::dvec3(axes.axis_x) * snap(axes.axis_x) + math::dvec3(axes.axis_y) * snap(axes.axis_y);
}

struct SkyOcclusionRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle cull_reset;
    rhi::PipelineHandle cull_instances;
    rhi::PipelineHandle cull_meshlets;
    rhi::PipelineHandle cull_args;
    /// Per layer of the map: opaque and alpha-tested casters.
    std::array<std::array<rhi::PipelineHandle, 2>, 3> casters;
    rhi::PipelineHandle heights;
    rhi::PipelineHandle heights_mip;
    rhi::PipelineHandle trace;
    rhi::PipelineHandle blur;

    struct Buffer {
        rhi::BufferHandle handle;
        std::uint64_t size = 0;
        bool written = false;
    };
    Buffer counters;
    Buffer draw_args;
    Buffer dispatch_args;
    Buffer tasks;
    Buffer visible;
    std::vector<rhi::BufferHandle> views;   ///< a CullView per frame slot
    std::vector<rhi::BufferHandle> frames;  ///< a SkyOcclusionFrame per frame slot

    struct Texture {
        rhi::TextureHandle handle;
        bool written = false;
    };
    /// The heights and their pyramid, kept between frames: drawn again only every refresh_frames frames or when the
    /// camera moves away from where they were drawn.
    Texture heights_map;
    struct Drawn {
        math::dvec3 center{0.0};  ///< absolute, snapped
        math::vec3 up{0.0f};
        float half_width = 0.0f;
        std::uint64_t frame = 0;
        bool valid = false;
    };
    Drawn drawn;
    /// The trace's results, kept so the frame's descriptors are known when it is written.
    Texture ratios;
    Texture distances;
    std::array<std::uint32_t, 2> extent{};

    Impl() = default;
    ~Impl() {
        if (device == nullptr) {
            return;
        }
        for (Buffer* b : {&counters, &draw_args, &dispatch_args, &tasks, &visible}) {
            if (b->handle.is_valid()) {
                device->destroy(b->handle);
            }
        }
        for (const std::vector<rhi::BufferHandle>* list : {&views, &frames}) {
            for (const rhi::BufferHandle b : *list) {
                device->destroy(b);
            }
        }
        for (Texture* t : {&heights_map, &ratios, &distances}) {
            if (t->handle.is_valid()) {
                device->destroy(t->handle);
            }
        }
        for (rhi::PipelineHandle* p :
             {&cull_reset, &cull_instances, &cull_meshlets, &cull_args, &heights, &heights_mip, &trace, &blur}) {
            if (p->is_valid()) {
                device->destroy(*p);
            }
        }
        for (auto& layer : casters) {
            for (rhi::PipelineHandle& p : layer) {
                if (p.is_valid()) {
                    device->destroy(p);
                }
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

    GraphResource import(RenderGraph& graph, Texture& t, const char* name) {
        const GraphResource r =
            graph.import(t.handle, name, t.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
                         rhi::ResourceState::shader_read);
        t.written = true;
        return r;
    }

    Result<> make(Texture& t, const rhi::TextureDesc& desc) {
        if (t.handle.is_valid()) {
            device->destroy(t.handle);
            t.handle = {};
        }
        auto texture = device->create_texture(desc);
        if (!texture) {
            return std::unexpected(texture.error());
        }
        t.handle = *texture;
        t.written = false;
        return {};
    }

    Result<> ensure_textures(std::uint32_t width, std::uint32_t height) {
        constexpr rhi::TextureUsage rw = rhi::TextureUsage::storage | rhi::TextureUsage::sampled;
        if (!heights_map.handle.is_valid()) {
            if (auto made = make(heights_map, {.width = map_resolution,
                                               .height = map_resolution,
                                               .format = rhi::Format::rgba16_float,
                                               .usage = rw,
                                               .debug_name = "sky map heights",
                                               .mip_levels = map_mips});
                !made) {
                return made;
            }
            drawn.valid = false;
        }
        if (extent[0] == width && extent[1] == height && ratios.handle.is_valid()) {
            return {};
        }
        extent = {};
        if (auto made = make(ratios, {.width = width,
                                      .height = height,
                                      .format = rhi::Format::rgba16_float,
                                      .usage = rw,
                                      .debug_name = "sky occlusion"});
            !made) {
            return made;
        }
        if (auto made = make(distances, {.width = width,
                                         .height = height,
                                         .format = rhi::Format::r16_float,
                                         .usage = rw,
                                         .debug_name = "sky occlusion distances"});
            !made) {
            return made;
        }
        extent = {width, height};
        return {};
    }
};

SkyOcclusionRenderer::SkyOcclusionRenderer() : impl_(std::make_unique<Impl>()) {}

SkyOcclusionRenderer::~SkyOcclusionRenderer() = default;

Result<> SkyOcclusionRenderer::prepare(rhi::Device& device, const SceneShaders& shaders) {
    Impl& s = *impl_;
    s.device = &device;
    const rhi::ShaderLibrary& cull = veach_render_shaders::gpu_cull();
    for (const auto& [entry, group, out] :
         {std::tuple{"cull_reset", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_reset},
          std::tuple{"cull_instances", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_instances},
          std::tuple{"cull_meshlets", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_meshlets},
          std::tuple{"cull_args", std::array<std::uint32_t, 3>{1, 1, 1}, &s.cull_args}}) {
        auto pipeline = device.create_compute_pipeline({&cull, entry, group, std::string("sky map ") + entry});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    for (std::size_t layer = 0; layer < s.casters.size(); ++layer) {
        for (std::size_t i = 0; i < 2; ++i) {
            rhi::GraphicsPipelineDesc desc;
            desc.library = &shaders.visibility;
            desc.vertex_entry = "shadow_vs";
            desc.fragment_entry = i == 0 ? "shadow_fs" : "shadow_fs_masked";
            desc.depth_format = rhi::Format::depth32_float;
            desc.depth_test = true;
            desc.depth_write = true;
            // Reverse-Z, 1 at the top: the highest surface wins with greater, the lowest with less.
            desc.depth_compare = layer == map_lowest ? rhi::CompareOp::less : rhi::CompareOp::greater;
            // Every surface counts from both sides, as in the shadow maps; the downward layer keeps the faces turned
            // down.
            desc.cull = layer == map_downward ? downward_faces : rhi::CullMode::none;
            desc.debug_name = std::format("sky map {} {}", map_layer_names.at(layer), desc.fragment_entry);
            auto pipeline = device.create_graphics_pipeline(desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            s.casters.at(layer).at(i) = *pipeline;
        }
    }
    for (const auto& [entry, out] : {std::pair{"sky_heights", &s.heights}, std::pair{"sky_heights_mip", &s.heights_mip},
                                     std::pair{"sky_trace", &s.trace}, std::pair{"sky_blur", &s.blur}}) {
        auto pipeline = device.create_compute_pipeline(
            {&veach_render_shaders::sky_occlusion(), entry, {8, 8, 1}, std::string(entry)});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    for (std::uint32_t i = 0; i < device.frames_in_flight(); ++i) {
        auto frame = device.create_buffer({.size = sizeof(SkyOcclusionFrame),
                                           .memory = rhi::MemoryType::upload,
                                           .debug_name = "sky occlusion frame",
                                           .stride = sizeof(SkyOcclusionFrame)});
        if (!frame) {
            return std::unexpected(frame.error());
        }
        s.frames.push_back(*frame);
        auto view = device.create_buffer({.size = sizeof(CullView),
                                          .memory = rhi::MemoryType::upload,
                                          .debug_name = "sky map cull view",
                                          .stride = sizeof(CullView)});
        if (!view) {
            return std::unexpected(view.error());
        }
        s.views.push_back(*view);
    }
    return {};
}

SkyOcclusionRenderer::Output SkyOcclusionRenderer::record(RenderGraph& graph, const FrameContext& frame,
                                                          const RenderView& view, const RealtimeScene& scene,
                                                          GraphResource depth, GraphResource visibility,
                                                          const math::mat4& inverse_view_projection,
                                                          const ShadowRenderer::Output& shadows, std::uint32_t width,
                                                          std::uint32_t height) {
    VEACH_ZONE("SkyOcclusionRenderer::record");
    Impl& s = *impl_;
    const std::uint32_t slot = s.device->current_frame().slot % static_cast<std::uint32_t>(s.frames.size());
    Output out{.frame = s.frames.at(slot)};
    SkyOcclusionFrame f;
    const auto write_frame = [&] {
        if (void* mapped = s.device->map(out.frame); mapped != nullptr) {
            std::memcpy(mapped, &f, sizeof(f));
        }
    };
    const AmbientOcclusion& settings = view.ambient_occlusion;
    if (!settings.enabled || !(settings.sky_distance > 0.0f) || scene.instance_count == 0 ||
        env_is_one("VEACH_DISABLE_SKY_OCCLUSION")) {
        write_frame();
        return out;
    }

    // ---- persistent resources.
    std::uint32_t meshlets = 0;
    std::array<std::uint32_t, draw_list_count> list_offsets{};
    for (std::uint32_t l = 0; l < draw_list_count; ++l) {
        list_offsets.at(l) = meshlets;
        meshlets += scene.list_capacity.at(l);
    }
    constexpr rhi::BufferUsage rw = rhi::BufferUsage::storage;
    constexpr auto u32 = static_cast<std::uint32_t>(sizeof(std::uint32_t));
    constexpr auto u64 = static_cast<std::uint32_t>(2 * sizeof(std::uint32_t));
    const std::array<std::uint32_t, 2> cells{(width + sky_cell - 1) / sky_cell, (height + sky_cell - 1) / sky_cell};
    for (const Result<>& ready :
         {s.ensure(s.counters, counter_count * u32, rw, u32, "sky map cull counters"),
          s.ensure(s.draw_args, 2 * draw_list_count * sizeof(rhi::DrawIndirectArgs), rw | rhi::BufferUsage::indirect,
                   u32, "sky map draw args"),
          s.ensure(s.dispatch_args, 4 * dispatch_record, rw | rhi::BufferUsage::indirect, u32, "sky map dispatch args"),
          s.ensure(s.tasks, std::uint64_t{scene.task_count} * u64, rw, u64, "sky map cull tasks"),
          s.ensure(s.visible, std::uint64_t{meshlets} * u64, rw, u64, "sky map draw lists"),
          s.ensure_textures(cells[0], cells[1])}) {
        if (!ready) {
            log::error("render", ready.error());
            write_frame();
            return out;
        }
    }

    // ---- where the map is: drawn again around the camera when it is stale or the camera has left its middle.
    const float reach = settings.sky_distance;
    const float half_width = reach * (1.0f + map_margin);
    const math::vec3 up = math::normalize(view.up);
    const math::dvec3& origin = frame.world.space.origin;
    const std::uint64_t device_frame = s.device->current_frame().index;
    const math::dvec3 wanted = snap_sky_map(view.position, up, origin,
                                            2.0f * half_width / static_cast<float>(map_resolution >> (map_mips - 1)));
    Impl::Drawn& drawn = s.drawn;
    const bool redraw = !drawn.valid || drawn.half_width != half_width || math::dot(drawn.up, up) < 0.99999f ||
                        device_frame >= drawn.frame + refresh_frames || math::length(wanted - drawn.center) > 1e-3;
    if (redraw) {
        drawn = {.center = wanted, .up = up, .half_width = half_width, .frame = device_frame, .valid = true};
    }
    const SkyMapFit fit = fit_sky_map(math::vec3(drawn.center - origin), drawn.up, half_width, map_resolution);
    f.center = math::vec4(fit.center, fit.half_width);
    f.up = math::vec4(fit.up, fit.texel);
    f.axis_x = math::vec4(fit.axis_x, fit.depth_range);
    f.axis_y = math::vec4(fit.axis_y, fit.below);
    f.reach = math::vec4(reach * (1.0f - fade_share), reach, static_cast<float>(map_mips), 0.0f);
    if (shadows.map) {
        const std::vector<std::uint32_t> lights = shadowed_lights(frame.world.lights, view.shadows.suns);
        for (std::size_t i = 0; i < lights.size() && i < f.lights.size(); ++i) {
            f.lights.at(i) = math::vec4(-math::normalize(frame.world.lights.at(lights[i]).direction), 1.0f);
        }
    }
    f.ratios = s.device->descriptor(s.ratios.handle);
    f.distances = s.device->descriptor(s.distances.handle);
    f.extent = cells;
    write_frame();

    const GraphResource heights = s.import(graph, s.heights_map, "sky map heights");
    const rhi::BufferHandle frame_buffer = out.frame;
    const SkyParams base{.inverse_view_projection = inverse_view_projection,
                         .camera = math::vec4(view.position, 0.0f),
                         .size = {width, height}};
    if (redraw) {
        // ---- culling: the map's box, the shadow cascades' rules (no normal cones, no casters under half a texel).
        CullView cull{.view_projection = fit.clip_from_render,
                      .occlusion_view_projection = fit.clip_from_render,
                      .planes = fit.planes,
                      .camera = math::vec4(fit.center, 0.0f),
                      .screen = {map_resolution, map_resolution},
                      .flags = cull_shadow_view,
                      .instance_count = scene.instance_count,
                      .phase_stride = 0,
                      .task_capacity = scene.task_count,
                      .sweep = math::vec4(-fit.up, small_caster_texels * fit.texel)};
        for (math::vec4& p : cull.receiver_planes) {
            p = math::vec4(0.0f, 0.0f, 0.0f, 1.0f);  // every caster is wanted wherever it is in the box
        }
        std::copy(list_offsets.begin(), list_offsets.end(), cull.list_offsets.begin());
        const rhi::BufferHandle view_buffer = s.views.at(slot);
        if (void* mapped = s.device->map(view_buffer); mapped != nullptr) {
            std::memcpy(mapped, &cull, sizeof(cull));
        }
        const GraphResource cull_view = graph.import(view_buffer, "sky map cull view", rhi::ResourceState::shader_read);
        const GraphResource counters = s.import(graph, s.counters, "sky map cull counters");
        const GraphResource draw_args = s.import(graph, s.draw_args, "sky map draw args");
        const GraphResource dispatch_args = s.import(graph, s.dispatch_args, "sky map dispatch args");
        const GraphResource tasks = s.import(graph, s.tasks, "sky map cull tasks");
        const GraphResource visible = s.import(graph, s.visible, "sky map draw lists");
        const GraphResource instances = scene.instances;
        const rhi::BufferHandle materials = scene.materials;
        const rhi::DescriptorHandle null_texture = s.device->null_descriptor(rhi::DescriptorKind::texture);
        const auto bind_cull = [=](PassContext& ctx) {
            const auto storage = [&](GraphResource r) {
                return ctx.device.descriptor(ctx.buffer(r), rhi::View::storage);
            };
            CullParams p{.phase = 0, .bin_count = 0};
            p.view = ctx.device.descriptor(ctx.buffer(cull_view));
            p.instances = ctx.device.descriptor(ctx.buffer(instances));
            p.materials = ctx.device.descriptor(materials);
            p.hiz = null_texture;
            p.counters = storage(counters);
            p.draw_args = storage(draw_args);
            p.dispatch_args = storage(dispatch_args);
            p.tasks = storage(tasks);
            p.recheck_instances = storage(counters);  // phase 1 without a pyramid rechecks nothing
            p.recheck_meshlets = storage(tasks);
            p.visible = storage(visible);
            p.bin_tiles = storage(counters);  // bin_count 0: never written
            ctx.cmd.set_parameters(p);
        };
        enum class Step : std::uint8_t { reset, instances, args, meshlets };
        for (const auto& [step, name] :
             {std::pair{Step::reset, "sky map: cull reset"}, std::pair{Step::instances, "sky map: cull instances"},
              std::pair{Step::args, "sky map: cull args"}, std::pair{Step::meshlets, "sky map: cull meshlets"}}) {
            graph.add_compute_pass(
                name,
                [&](PassBuilder& pb) {
                    if (step == Step::instances || step == Step::meshlets) {
                        scene.read_meshes(pb);
                        pb.read(cull_view);
                        pb.write(tasks);
                        pb.write(visible);
                    }
                    if (step == Step::meshlets) {
                        pb.read(dispatch_args, rhi::ResourceState::indirect_argument);
                    } else if (step == Step::args) {
                        pb.write(dispatch_args);
                    }
                    pb.write(counters);
                    if (step != Step::args) {
                        pb.write(draw_args);
                    }
                },
                [=,
                 pipeline = step == Step::reset       ? s.cull_reset
                            : step == Step::instances ? s.cull_instances
                            : step == Step::args      ? s.cull_args
                                                      : s.cull_meshlets,
                 count = scene.instance_count](PassContext& ctx) {
                    ctx.cmd.set_pipeline(pipeline);
                    bind_cull(ctx);
                    if (step == Step::instances) {
                        const auto [x, y] = rows_of((count + 63) / 64);
                        ctx.cmd.dispatch(x, y);
                    } else if (step == Step::meshlets) {
                        ctx.cmd.dispatch_indirect(ctx.buffer(dispatch_args), dispatch_tasks_1);
                    } else {
                        ctx.cmd.dispatch(1, 1);
                    }
                });
        }

        // ---- the two depth maps from above.
        constexpr rhi::TextureUsage depth_usage = rhi::TextureUsage::depth_target | rhi::TextureUsage::sampled;
        std::array<GraphResource, 3> maps{};
        for (std::size_t m = 0; m < maps.size(); ++m) {
            maps.at(m) = graph.create_texture({map_resolution, map_resolution, rhi::Format::depth32_float, depth_usage,
                                               std::format("sky map {}", map_layer_names.at(m))});
        }
        const std::array<std::uint32_t, draw_list_count> list_capacity = scene.list_capacity;
        const VisibilityParams draw_params{
            .view_projection = fit.clip_from_render,
            .previous_view_projection = fit.clip_from_render,
            .inverse_size = {1.0f / static_cast<float>(map_resolution), 1.0f / static_cast<float>(map_resolution)}};
        for (std::size_t m = 0; m < maps.size(); ++m) {
            graph.add_graphics_pass(
                std::format("sky map: {}", map_layer_names.at(m)),
                [&](PassBuilder& pb) {
                    // Reverse-Z: 0 is the map's bottom, 1 its top; each layer starts where nothing is.
                    pb.depth_target(maps.at(m), rhi::LoadOp::clear, m == map_lowest ? 1.0f : 0.0f);
                    scene.read_geometry(pb);
                    pb.read(visible);
                    pb.read(draw_args, rhi::ResourceState::indirect_argument);
                },
                [=, pipelines = s.casters.at(m)](PassContext& ctx) mutable {
                    VisibilityParams p = draw_params;
                    p.instances = ctx.device.descriptor(ctx.buffer(instances));
                    p.materials = ctx.device.descriptor(materials);
                    p.visible = ctx.device.descriptor(ctx.buffer(visible));
                    rhi::PipelineHandle bound;
                    for (std::uint32_t l = 0; l < draw_list_count; ++l) {
                        if (list_capacity.at(l) == 0) {
                            continue;
                        }
                        const rhi::PipelineHandle pipeline = l < draw_bucket_count ? pipelines[0] : pipelines[1];
                        if (pipeline != bound) {
                            ctx.cmd.set_pipeline(pipeline);
                            bound = pipeline;
                        }
                        p.list_offset = list_offsets.at(l);
                        ctx.cmd.set_parameters(p);
                        ctx.cmd.draw_indirect(ctx.buffer(draw_args), l * sizeof(rhi::DrawIndirectArgs));
                    }
                });
        }

        // ---- the heights and their pyramid.
        for (std::uint32_t level = 0; level < map_mips; ++level) {
            const GraphResource target = graph.mips(heights, level);
            const GraphResource source = level == 0 ? heights : graph.mips(heights, level - 1);
            const std::uint32_t size = rhi::mip_size(map_resolution, level);
            graph.add_compute_pass(
                level == 0 ? std::string("sky map: heights") : std::format("sky map: heights mip {}", level),
                [&](PassBuilder& pb) {
                    if (level == 0) {
                        for (const GraphResource map : maps) {
                            pb.read(map);
                        }
                    } else {
                        pb.read(source);
                    }
                    pb.write(target);
                },
                [=, pipeline = level == 0 ? s.heights : s.heights_mip](PassContext& ctx) {
                    SkyParams p = base;
                    p.extent = {size, size};
                    p.frame = ctx.device.descriptor(frame_buffer);
                    if (level == 0) {
                        p.top = ctx.device.descriptor(ctx.texture(maps[map_highest]));
                        p.lowest = ctx.device.descriptor(ctx.texture(maps[map_lowest]));
                        p.downward = ctx.device.descriptor(ctx.texture(maps[map_downward]));
                    } else {
                        p.heights = ctx.device.descriptor(ctx.texture(source), rhi::View::read, ctx.range(source));
                    }
                    p.heights_out = ctx.device.descriptor(ctx.texture(target), rhi::View::storage, ctx.range(target));
                    ctx.cmd.set_pipeline(pipeline);
                    ctx.cmd.set_parameters(p);
                    ctx.cmd.dispatch((size + 7) / 8, (size + 7) / 8);
                });
        }
    }

    // ---- the trace, a thread per cell of 8 x 8 pixels, then its blur into the ratios the shade pass reads.
    const GraphResource raw = graph.create_texture({cells[0], cells[1], rhi::Format::rgba16_float,
                                                    rhi::TextureUsage::storage | rhi::TextureUsage::sampled,
                                                    "sky occlusion before the blur"});
    const GraphResource ratios = s.import(graph, s.ratios, "sky occlusion");
    const GraphResource distances = s.import(graph, s.distances, "sky occlusion distances");
    const std::optional<GraphResource> shadow_map = shadows.map;
    const rhi::BufferHandle shadow_frame = shadows.frame;
    const rhi::BufferHandle sky = scene.sky;
    const GraphResource scene_instances = scene.instances;
    graph.add_compute_pass(
        "sky occlusion: trace",
        [&](PassBuilder& pb) {
            scene.read_sky(pb);
            scene.read_meshes(pb);
            pb.read(depth);
            pb.read(visibility);
            pb.read(heights);
            if (shadow_map) {
                pb.read(*shadow_map);
            }
            pb.write(raw);
            pb.write(distances);
        },
        [=, pipeline = s.trace](PassContext& ctx) {
            SkyParams p = base;
            p.extent = cells;
            p.frame = ctx.device.descriptor(frame_buffer);
            p.shadows = ctx.device.descriptor(shadow_frame);
            p.sky = ctx.device.descriptor(sky);
            p.instances = ctx.device.descriptor(ctx.buffer(scene_instances));
            p.visibility = ctx.device.descriptor(ctx.texture(visibility));
            p.top = ctx.device.descriptor(ctx.texture(depth));
            p.heights = ctx.device.descriptor(ctx.texture(heights));
            p.ratios = ctx.device.descriptor(ctx.texture(raw), rhi::View::storage);
            p.distances = ctx.device.descriptor(ctx.texture(distances), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(p);
            ctx.cmd.dispatch((cells[0] + 7) / 8, (cells[1] + 7) / 8);
        });
    graph.add_compute_pass(
        "sky occlusion: blur",
        [&](PassBuilder& pb) {
            pb.read(raw);
            pb.read(distances);
            pb.write(ratios);
        },
        [=, pipeline = s.blur](PassContext& ctx) {
            SkyParams p = base;
            p.extent = cells;
            p.raw = ctx.device.descriptor(ctx.texture(raw));
            p.near = ctx.device.descriptor(ctx.texture(distances));
            p.ratios = ctx.device.descriptor(ctx.texture(ratios), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(p);
            ctx.cmd.dispatch((cells[0] + 7) / 8, (cells[1] + 7) / 8);
        });
    out.active = true;
    out.textures = {ratios, distances};
    return out;
}

}  // namespace veach::render
