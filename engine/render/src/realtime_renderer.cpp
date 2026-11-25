#include "realtime_renderer.hpp"

#include <veach/asset/scene.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include "scene_records.hpp"
#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <unordered_map>

namespace veach::render {

namespace {

/// Mirrors VisibilityParams in shaders/realtime_visibility.slang.
struct alignas(16) VisibilityParams {
    math::mat4 view_projection{1.0f};
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    std::uint32_t instance = 0;
    std::uint32_t pad0 = 0;
    std::array<std::uint32_t, 2> pad1{};
};
static_assert(rhi::parameters_match<VisibilityParams>(
    veach_render_shaders::layout::realtime_visibility,
    {VEACH_PARAM(VisibilityParams, view_projection), VEACH_PARAM(VisibilityParams, instances),
     VEACH_PARAM(VisibilityParams, materials), VEACH_PARAM(VisibilityParams, instance),
     VEACH_PARAM(VisibilityParams, pad0), VEACH_PARAM(VisibilityParams, pad1)}));

/// Mirrors RealtimeParams in shaders/realtime_shade.slang.
struct alignas(16) RealtimeParams {
    rhi::DescriptorHandle frame;
    std::uint32_t argument = 0;
    std::uint32_t pad = 0;
};
static_assert(rhi::parameters_match<RealtimeParams>(
    veach_render_shaders::layout::realtime_shade,
    {VEACH_PARAM(RealtimeParams, frame), VEACH_PARAM(RealtimeParams, argument), VEACH_PARAM(RealtimeParams, pad)}));

/// Mirrors Params in shaders/taau.slang.
struct alignas(16) TaauParams {
    rhi::DescriptorHandle color;
    rhi::DescriptorHandle motion;
    rhi::DescriptorHandle position;
    rhi::DescriptorHandle history;
    rhi::DescriptorHandle output;
    rhi::DescriptorHandle linear_clamp;
    std::array<std::uint32_t, 2> render_size{};
    std::array<std::uint32_t, 2> output_size{};
    math::vec2 jitter{0.0f};
    std::uint32_t reset = 1;
    std::uint32_t pad = 0;
};
static_assert(rhi::parameters_match<TaauParams>(veach_render_shaders::layout::taau,
                                                {VEACH_PARAM(TaauParams, color), VEACH_PARAM(TaauParams, motion),
                                                 VEACH_PARAM(TaauParams, position), VEACH_PARAM(TaauParams, history),
                                                 VEACH_PARAM(TaauParams, output), VEACH_PARAM(TaauParams, linear_clamp),
                                                 VEACH_PARAM(TaauParams, render_size),
                                                 VEACH_PARAM(TaauParams, output_size), VEACH_PARAM(TaauParams, jitter),
                                                 VEACH_PARAM(TaauParams, reset), VEACH_PARAM(TaauParams, pad)}));

/// Mirrors RealtimeFrame in shaders/veach/realtime.slang (a structured buffer element: no reflection to check it
/// against, so its offsets are asserted by hand).
struct alignas(16) RealtimeFrame {
    math::mat4 view_projection{1.0f};
    math::mat4 inverse_view_projection{1.0f};
    math::mat4 unjittered_view_projection{1.0f};
    math::mat4 previous_view_projection{1.0f};
    math::vec4 camera_position{0.0f};
    math::vec4 previous_camera_position{0.0f};
    math::vec4 sky_ambient{0.0f};
    math::vec4 jitter{0.0f};
    rhi::DescriptorHandle tlas;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle lights;
    rhi::DescriptorHandle emissive;
    rhi::DescriptorHandle visibility;
    std::array<rhi::DescriptorHandle, 7>
        surfaces;  ///< position, normal, geometric, diffuse, specular, emission, motion
    std::array<rhi::DescriptorHandle, 7> surfaces_out;  ///< the same, writable
    std::array<rhi::DescriptorHandle, 5> previous;      ///< last frame's position .. specular
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle radiance_out;
    rhi::DescriptorHandle alias;
    rhi::DescriptorHandle temporal_out;
    rhi::DescriptorHandle temporal_reservoirs;
    rhi::DescriptorHandle final_out;
    rhi::DescriptorHandle final_reservoirs;
    rhi::DescriptorHandle previous_reservoirs;
    rhi::DescriptorHandle accumulation;
    rhi::DescriptorHandle accumulation_out;
    rhi::DescriptorHandle unused0;
    std::array<std::uint32_t, 2> render_size{};
    std::uint32_t frame = 0;
    std::uint32_t flags = 0;
    std::uint32_t light_count = 0;
    std::uint32_t emissive_count = 0;
    float emissive_total_weight = 0.0f;
    std::uint32_t scene_flags = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t ao_rays = 0;
    float ao_radius = 1.0f;
    std::uint32_t debug_skip = 0;
    std::uint32_t view = 0;
    std::uint32_t light_candidates = 0;
    std::uint32_t spatial_samples = 0;
    std::uint32_t temporal_reuse = 0;
    std::uint32_t accumulated = 0;
    std::uint32_t pad1 = 0;
    std::array<std::uint32_t, 2> pad2{};
};
static_assert(offsetof(RealtimeFrame, tlas) == 320);
static_assert(offsetof(RealtimeFrame, surfaces) == 368);
static_assert(offsetof(RealtimeFrame, previous) == 480);
static_assert(offsetof(RealtimeFrame, alias) == 536);
static_assert(offsetof(RealtimeFrame, render_size) == 608);
static_assert(sizeof(RealtimeFrame) == 688);

/// Mirrors PackedReservoir in shaders/veach/realtime.slang (ADR 0041).
struct PackedReservoir {
    math::vec4 position_light{0.0f};
    math::vec4 normal_w{0.0f};
    math::vec4 radiance_m{0.0f};
};
static_assert(sizeof(PackedReservoir) == 48);

constexpr std::uint32_t frame_history_valid = 1;  ///< RealtimeFrame::flags, as in veach/realtime.slang
constexpr std::uint32_t frame_reservoirs_valid = 2;
constexpr std::uint32_t jitter_phases = 16;  ///< Halton(2,3) phases (ADR 0040)

/// The surface targets (ADR 0040), in RealtimeFrame's order.
constexpr std::array<rhi::Format, 7> surface_formats{
    rhi::Format::rgba32_float, rhi::Format::rgba16_float, rhi::Format::rgba16_float, rhi::Format::rgba16_float,
    rhi::Format::rgba16_float, rhi::Format::rgba16_float, rhi::Format::rgba16_float};
constexpr std::array<const char*, 7> surface_names{
    "surface position", "surface normal", "surface geometric", "surface diffuse", "surface specular",
    "surface emission", "motion"};

float halton(std::uint32_t index, std::uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

/// A texture that outlives the frame: imported into each frame's graph in the state it was left in.
struct Persistent {
    rhi::TextureHandle handle;
    bool written = false;
};

/// The same for a buffer (ReSTIR's reservoirs).
struct PersistentBuffer {
    rhi::BufferHandle handle;
    bool written = false;
};

}  // namespace

struct RealtimeRenderer::Impl {
    /// One camera's targets and history (ADR 0040).
    struct ViewState {
        std::uint32_t render_width = 0;
        std::uint32_t render_height = 0;
        std::uint32_t output_width = 0;
        std::uint32_t output_height = 0;
        UpscalerMode mode = UpscalerMode::none;  ///< resolved: temporal or none
        Persistent visibility;
        std::array<std::array<Persistent, 7>, 2> surfaces;  ///< ping-pong: this frame's and last frame's
        Persistent radiance;
        std::array<Persistent, 2> upscaled;                ///< output resolution, ping-pong (TAAU history)
        Persistent accumulation;                           ///< RealtimeView::accumulated
        std::uint32_t accumulated = 0;                     ///< frames in it
        PersistentBuffer temporal_reservoirs;              ///< ReSTIR: initial + temporal, this frame
        std::array<PersistentBuffer, 2> final_reservoirs;  ///< ReSTIR: after spatial reuse, ping-pong
        std::uint64_t lights_hash = 0;                     ///< the lights last frame's reservoirs sampled
        bool history = false;                              ///< last frame's targets belong to this view
        std::uint32_t frame = 0;  ///< frames since the targets were made; its parity picks the set
        math::mat4 previous_view_projection{1.0f};
        math::mat4 previous_view{1.0f};
        math::vec3 previous_position{0.0f};
    };

    rhi::Device* device = nullptr;
    rhi::PipelineHandle visibility_pipeline;
    rhi::PipelineHandle surface_pipeline;
    rhi::PipelineHandle shade_pipeline;
    rhi::PipelineHandle restir_temporal_pipeline;
    rhi::PipelineHandle restir_spatial_pipeline;
    rhi::PipelineHandle accumulate_pipeline;
    rhi::PipelineHandle taau_pipeline;
    rhi::PipelineHandle display_pipeline;
    rhi::SamplerHandle linear_clamp;
    std::unordered_map<std::uint64_t, ViewState> views;
    std::uint32_t frame = 0;  ///< seeds the random numbers
    std::vector<rhi::BufferHandle> frame_buffers;

    ~Impl() {
        if (device == nullptr) {
            return;
        }
        device->wait_idle();
        release_frame_buffers();
        for (auto& [id, v] : views) {
            destroy(v);
        }
        for (rhi::PipelineHandle* p :
             {&visibility_pipeline, &surface_pipeline, &shade_pipeline, &restir_temporal_pipeline,
              &restir_spatial_pipeline, &accumulate_pipeline, &taau_pipeline, &display_pipeline}) {
            if (p->is_valid()) {
                device->destroy(*p);
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

    void destroy(Persistent& p) {
        if (p.handle.is_valid()) {
            device->destroy(p.handle);
        }
        p = {};
    }
    void destroy(ViewState& v) {
        destroy(v.visibility);
        for (auto& set : v.surfaces) {
            for (Persistent& p : set) {
                destroy(p);
            }
        }
        destroy(v.radiance);
        for (Persistent& p : v.upscaled) {
            destroy(p);
        }
        destroy(v.accumulation);
        destroy(v.temporal_reservoirs);
        for (PersistentBuffer& b : v.final_reservoirs) {
            destroy(b);
        }
    }
    void destroy(PersistentBuffer& b) {
        if (b.handle.is_valid()) {
            device->destroy(b.handle);
        }
        b = {};
    }
    bool create(PersistentBuffer& b, std::uint64_t size, const char* name) {
        auto buffer = device->create_buffer(
            {.size = size, .usage = rhi::BufferUsage::storage, .debug_name = name, .stride = sizeof(PackedReservoir)});
        if (!buffer) {
            log::error("render", buffer.error());
            return false;
        }
        b = {*buffer, false};
        return true;
    }
    GraphResource import(RenderGraph& graph, PersistentBuffer& b, const char* name) {
        return graph.import(b.handle, name, b.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
                            rhi::ResourceState::shader_read);
    }

    bool create(Persistent& p, const rhi::TextureDesc& desc) {
        auto texture = device->create_texture(desc);
        if (!texture) {
            log::error("render", texture.error());
            return false;
        }
        p = {*texture, false};
        return true;
    }

    /// (Re)creates the view's targets for these sizes and mode. False if the GPU refused one.
    bool allocate(ViewState& v, std::uint32_t rw, std::uint32_t rh, std::uint32_t ow, std::uint32_t oh,
                  UpscalerMode mode) {
        destroy(v);
        v = ViewState{.render_width = rw, .render_height = rh, .output_width = ow, .output_height = oh, .mode = mode};
        constexpr auto rw_usage = rhi::TextureUsage::storage | rhi::TextureUsage::sampled;
        bool ok = create(v.visibility, {rw, rh, rhi::Format::rg32_uint,
                                        rhi::TextureUsage::color_target | rhi::TextureUsage::sampled, "visibility"});
        for (auto& set : v.surfaces) {
            for (std::size_t i = 0; i < set.size(); ++i) {
                ok = ok && create(set[i], {rw, rh, surface_formats[i], rw_usage, surface_names[i]});
            }
        }
        ok = ok && create(v.radiance, {rw, rh, rhi::Format::rgba16_float, rw_usage, "radiance"});
        ok = ok && create(v.accumulation, {rw, rh, rhi::Format::rgba32_float, rw_usage, "accumulation"});
        const std::uint64_t reservoirs = std::uint64_t{rw} * rh * sizeof(PackedReservoir);
        ok = ok && create(v.temporal_reservoirs, reservoirs, "reservoirs (temporal)");
        for (PersistentBuffer& b : v.final_reservoirs) {
            ok = ok && create(b, reservoirs, "reservoirs");
        }
        if (mode != UpscalerMode::none) {
            for (Persistent& p : v.upscaled) {
                ok = ok && create(p, {ow, oh, rhi::Format::rgba16_float, rw_usage, "upscaled"});
            }
        }
        return ok;
    }

    GraphResource import(RenderGraph& graph, Persistent& p, const char* name) {
        return graph.import(p.handle, name, p.written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
                            rhi::ResourceState::shader_read);
    }
};

RealtimeRenderer::RealtimeRenderer() : impl_(std::make_unique<Impl>()) {}

RealtimeRenderer::~RealtimeRenderer() = default;

Result<> RealtimeRenderer::prepare(rhi::Device& device, const SceneShaders& shaders) {
    Impl& s = *impl_;
    s.device = &device;
    rhi::GraphicsPipelineDesc visibility;
    visibility.library = &shaders.visibility;
    visibility.vertex_entry = "vis_vs";
    visibility.fragment_entry = "vis_fs";
    visibility.color_formats = {rhi::Format::rg32_uint};
    visibility.depth_format = rhi::Format::depth32_float;
    visibility.depth_test = true;
    visibility.depth_write = true;
    visibility.depth_compare = rhi::CompareOp::greater;  // reverse-Z: nearer is greater
    visibility.cull = rhi::CullMode::none;               // every surface is two-sided, as in the path tracer
    visibility.debug_name = "visibility";
    auto visibility_pipeline = device.create_graphics_pipeline(visibility);
    if (!visibility_pipeline) {
        return std::unexpected(visibility_pipeline.error());
    }
    s.visibility_pipeline = *visibility_pipeline;
    const auto compute = [&](const rhi::ShaderLibrary& library, const char* entry,
                             rhi::PipelineHandle& out) -> Result<> {
        auto pipeline = device.create_compute_pipeline({&library, entry, {8, 8, 1}, entry});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        out = *pipeline;
        return {};
    };
    for (const auto& [library, entry, out] :
         {std::tuple{&shaders.shade, "surface", &s.surface_pipeline},
          std::tuple{&shaders.shade, "restir_temporal", &s.restir_temporal_pipeline},
          std::tuple{&shaders.shade, "restir_spatial", &s.restir_spatial_pipeline},
          std::tuple{&shaders.shade, "shade", &s.shade_pipeline},
          std::tuple{&shaders.shade, "accumulate", &s.accumulate_pipeline},
          std::tuple{&veach_render_shaders::taau(), "taau", &s.taau_pipeline},
          std::tuple{&veach_render_shaders::display(), "display", &s.display_pipeline}}) {
        if (auto made = compute(*library, entry, *out); !made) {
            return made;
        }
    }
    auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::clamp});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.linear_clamp = *sampler;
    return {};
}

void RealtimeRenderer::record(RenderGraph& graph, const FrameContext& frame, const RenderView& view,
                              const RealtimeScene& scene) {
    VEACH_ZONE("RealtimeRenderer::record");
    Impl& s = *impl_;
    s.release_frame_buffers();  // last frame's: their submission is made, so destroy() waits for the GPU
    const RealtimeLighting& lighting = view.realtime;

    // Resolution and upscaler (ADR 0040, ADR 0043). MetalFX comes later: every upscaler is TAAU for now.
    const UpscalerMode mode = view.upscaling.mode == UpscalerMode::none ? UpscalerMode::none : UpscalerMode::temporal;
    const float scale = mode == UpscalerMode::none ? 1.0f : std::clamp(view.upscaling.render_scale, 0.5f, 1.0f);
    const std::uint32_t ow = frame.width;
    const std::uint32_t oh = frame.height;
    const auto rw = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<float>(ow) * scale)));
    const auto rh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<float>(oh) * scale)));

    Impl::ViewState& v = s.views[view.camera];
    if (v.render_width != rw || v.render_height != rh || v.output_width != ow || v.output_height != oh ||
        v.mode != mode || !v.visibility.handle.is_valid()) {
        if (!s.allocate(v, rw, rh, ow, oh, mode)) {
            return;
        }
    }
    // A camera cut drops history: more than 2 m or 45 degrees in one frame (ADR 0040).
    if (v.history) {
        const math::vec3 forward = -math::vec3(math::transpose(view.view)[2]);
        const math::vec3 previous_forward = -math::vec3(math::transpose(v.previous_view)[2]);
        if (math::length(view.position - v.previous_position) > 2.0f ||
            math::dot(forward, previous_forward) < std::cos(math::radians(45.0f))) {
            v.history = false;
        }
    }

    // Jitter: Halton(2,3) in render pixels while an upscaler runs; none otherwise.
    math::vec2 jitter{0.0f};
    if (mode != UpscalerMode::none) {
        const std::uint32_t index = v.frame % jitter_phases + 1;
        jitter = {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
    }
    math::mat4 jitter_ndc(1.0f);
    jitter_ndc[3] =
        math::vec4(2.0f * jitter.x / static_cast<float>(rw), -2.0f * jitter.y / static_cast<float>(rh), 0.0f, 1.0f);
    const math::mat4 jittered = jitter_ndc * view.view_projection;

    const std::size_t current = v.frame % 2;
    const std::size_t previous = 1 - current;
    auto& surfaces = v.surfaces[current];
    auto& last = v.surfaces[previous];
    // ReSTIR's reservoirs carry over only while the lights stay the same (ADR 0041).
    const bool reservoirs_valid = v.history && v.lights_hash == scene.lights_hash;
    v.lights_hash = scene.lights_hash;
    // The accumulated view restarts whenever anything it shows could have changed.
    const bool accumulating = lighting.view == RealtimeView::accumulated;
    if (!accumulating || !reservoirs_valid || v.previous_view_projection != view.view_projection) {
        v.accumulated = 0;
    }

    RealtimeFrame f;
    f.view_projection = jittered;
    f.inverse_view_projection = math::inverse(jittered);
    f.unjittered_view_projection = view.view_projection;
    f.previous_view_projection = v.history ? v.previous_view_projection : view.view_projection;
    f.camera_position = math::vec4(view.position, view.exposure);
    f.previous_camera_position = math::vec4(v.history ? v.previous_position : view.position, 0.0f);
    f.sky_ambient = math::vec4(lighting.sky, lighting.ambient ? 1.0f : 0.0f);
    f.jitter = math::vec4(jitter, 0.0f, 0.0f);
    rhi::Device& device = *s.device;
    f.tlas = device.descriptor(scene.tlas_handle);
    f.instances = device.descriptor(scene.instances);
    f.materials = device.descriptor(scene.materials);
    f.lights = device.descriptor(scene.lights);
    f.emissive = device.descriptor(scene.emissive);
    f.visibility = device.descriptor(v.visibility.handle);
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        f.surfaces[i] = device.descriptor(surfaces[i].handle);
        f.surfaces_out[i] = device.descriptor(surfaces[i].handle, rhi::View::storage);
    }
    for (std::size_t i = 0; i < f.previous.size(); ++i) {
        f.previous[i] = device.descriptor(last[i].handle);
    }
    f.radiance = device.descriptor(v.radiance.handle);
    f.radiance_out = device.descriptor(v.radiance.handle, rhi::View::storage);
    f.alias = device.descriptor(scene.alias);
    f.temporal_out = device.descriptor(v.temporal_reservoirs.handle, rhi::View::storage);
    f.temporal_reservoirs = device.descriptor(v.temporal_reservoirs.handle);
    f.final_out = device.descriptor(v.final_reservoirs[current].handle, rhi::View::storage);
    f.final_reservoirs = device.descriptor(v.final_reservoirs[current].handle);
    f.previous_reservoirs = device.descriptor(v.final_reservoirs[previous].handle);
    f.accumulation = device.descriptor(v.accumulation.handle);
    f.accumulation_out = device.descriptor(v.accumulation.handle, rhi::View::storage);
    f.unused0 = device.null_descriptor(rhi::DescriptorKind::texture);
    f.render_size = {rw, rh};
    f.frame = s.frame++;
    f.flags = (v.history ? frame_history_valid : 0u) | (reservoirs_valid ? frame_reservoirs_valid : 0u);
    f.light_count = scene.light_count;
    f.emissive_count = scene.emissive_count;
    f.emissive_total_weight = scene.emissive_total_weight;
    f.scene_flags = scene.scene_flags;
    f.ao_rays = lighting.ao_rays;
    f.ao_radius = lighting.ao_radius;
    f.debug_skip = lighting.debug_skip;
    f.view = static_cast<std::uint32_t>(lighting.view);
    f.light_candidates = lighting.light_candidates;
    f.spatial_samples = lighting.spatial_samples;
    f.temporal_reuse = lighting.temporal_reuse ? 1u : 0u;
    f.accumulated = v.accumulated;

    const std::array<RealtimeFrame, 1> records{f};
    auto frame_buffer = device.create_buffer({.size = sizeof(RealtimeFrame),
                                              .memory = rhi::MemoryType::upload,
                                              .debug_name = "realtime frame",
                                              .stride = sizeof(RealtimeFrame)},
                                             std::as_bytes(std::span(records)));
    if (!frame_buffer) {
        log::error("render", frame_buffer.error());
        return;
    }
    s.frame_buffers.push_back(*frame_buffer);
    const GraphResource frame_data = graph.import(*frame_buffer, "realtime frame", rhi::ResourceState::shader_read);
    RealtimeParams params{.frame = device.descriptor(*frame_buffer)};

    // Remember this frame for the next one.
    v.previous_view_projection = view.view_projection;
    v.previous_view = view.view;
    v.previous_position = view.position;

    const GraphResource visibility = s.import(graph, v.visibility, "visibility");
    const GraphResource depth =
        graph.create_texture({rw, rh, rhi::Format::depth32_float, rhi::TextureUsage::depth_target, "visibility depth"});
    std::array<GraphResource, 7> current_targets{};
    std::array<GraphResource, 5> previous_targets{};
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        current_targets[i] = s.import(graph, surfaces[i], surface_names[i]);
        surfaces[i].written = true;
    }
    for (std::size_t i = 0; i < previous_targets.size(); ++i) {
        previous_targets[i] = s.import(graph, last[i], surface_names[i]);
    }
    const GraphResource radiance = s.import(graph, v.radiance, "radiance");
    const GraphResource alias = graph.import(scene.alias, "light alias table", rhi::ResourceState::shader_read);
    const GraphResource temporal_reservoirs = s.import(graph, v.temporal_reservoirs, "reservoirs (temporal)");
    const GraphResource final_reservoirs = s.import(graph, v.final_reservoirs[current], "reservoirs");
    const GraphResource previous_reservoirs = s.import(graph, v.final_reservoirs[previous], "reservoirs (last frame)");
    const GraphResource accumulation = s.import(graph, v.accumulation, "accumulation");
    v.visibility.written = true;
    v.radiance.written = true;
    v.temporal_reservoirs.written = true;
    v.final_reservoirs[current].written = true;

    // ---- visibility (ADR 0037), with this frame's jitter.
    VisibilityParams vis{.view_projection = jittered};
    graph.add_graphics_pass(
        "visibility",
        [&](PassBuilder& b) {
            b.color_target_uint(visibility, rhi::LoadOp::clear, {0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u});
            b.depth_target(depth, rhi::LoadOp::clear, 0.0f);  // reverse-Z: far is 0
            scene.read_geometry(b);
        },
        [vis, counts = std::vector<std::uint32_t>(scene.meshlet_counts.begin(), scene.meshlet_counts.end()),
         pipeline = s.visibility_pipeline, instances = scene.instances,
         materials = scene.materials](PassContext& ctx) mutable {
            vis.instances = ctx.device.descriptor(instances);
            vis.materials = ctx.device.descriptor(materials);
            ctx.cmd.set_pipeline(pipeline);
            for (std::size_t i = 0; i < counts.size(); ++i) {
                if (counts[i] == 0) {
                    continue;
                }
                vis.instance = static_cast<std::uint32_t>(i);
                ctx.cmd.set_parameters(vis);
                // One draw instance per meshlet, every triangle slot of it (vertex pulling, ADR 0037).
                ctx.cmd.draw(meshlet_max_triangles * 3, counts[i]);
            }
        });

    const auto dispatch = [&](const char* name, rhi::PipelineHandle pipeline, const auto& declare) {
        graph.add_compute_pass(
            name,
            [&](PassBuilder& b) {
                b.read(frame_data);
                declare(b);
            },
            [params, pipeline, rw, rh](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(params);
                ctx.cmd.dispatch((rw + 7) / 8, (rh + 7) / 8);
            });
    };

    // ---- surfaces, motion and disocclusion (ADR 0040).
    dispatch("surface", s.surface_pipeline, [&](PassBuilder& b) {
        scene.read_scene(b);
        b.read(visibility);
        for (const GraphResource& r : previous_targets) {
            b.read(r);
        }
        for (const GraphResource& r : current_targets) {
            b.write(r, rhi::ResourceState::shader_write);
        }
    });

    // ---- direct light: ReSTIR DI (ADR 0041), then one shadow ray per pixel.
    dispatch("restir: candidates and temporal reuse", s.restir_temporal_pipeline, [&](PassBuilder& b) {
        scene.read_scene(b);
        b.read(alias);
        for (const GraphResource& r : current_targets) {
            b.read(r);
        }
        for (const GraphResource& r : previous_targets) {
            b.read(r);
        }
        b.read(previous_reservoirs);
        b.write(temporal_reservoirs, rhi::ResourceState::shader_write);
    });
    dispatch("restir: spatial reuse", s.restir_spatial_pipeline, [&](PassBuilder& b) {
        scene.read_scene(b);
        for (const GraphResource& r : current_targets) {
            b.read(r);
        }
        b.read(temporal_reservoirs);
        b.write(final_reservoirs, rhi::ResourceState::shader_write);
    });
    dispatch("shade", s.shade_pipeline, [&](PassBuilder& b) {
        scene.read_scene(b);
        for (const GraphResource& r : current_targets) {
            b.read(r);
        }
        b.read(final_reservoirs);
        b.write(radiance, rhi::ResourceState::shader_write);
    });
    if (accumulating) {
        dispatch("accumulate", s.accumulate_pipeline, [&](PassBuilder& b) {
            b.read(radiance);
            b.write(accumulation, rhi::ResourceState::shader_write);
        });
        v.accumulation.written = true;
        ++v.accumulated;
    }

    // ---- upscaling (ADR 0043), unless a debug view shows render pixels as they are.
    const bool image = lighting.view == RealtimeView::final || lighting.view == RealtimeView::noisy;
    GraphResource shown = accumulating ? accumulation : radiance;
    std::array<std::uint32_t, 2> shown_size{rw, rh};
    if (mode != UpscalerMode::none && image) {
        Persistent& out = v.upscaled[current];
        Persistent& history = v.upscaled[previous];
        const GraphResource out_resource = s.import(graph, out, "upscaled");
        const GraphResource history_resource = s.import(graph, history, "upscaled history");
        out.written = true;
        TaauParams taau;
        taau.color = device.descriptor(v.radiance.handle);
        taau.motion = device.descriptor(surfaces[6].handle);
        taau.position = device.descriptor(surfaces[0].handle);
        taau.history = device.descriptor(history.handle);
        taau.output = device.descriptor(out.handle, rhi::View::storage);
        taau.linear_clamp = device.descriptor(s.linear_clamp);
        taau.render_size = {rw, rh};
        taau.output_size = {ow, oh};
        taau.jitter = jitter;
        taau.reset = v.history ? 0u : 1u;
        graph.add_compute_pass(
            "taau",
            [&](PassBuilder& b) {
                b.read(radiance);
                b.read(current_targets[6]);
                b.read(current_targets[0]);
                b.read(history_resource);
                b.write(out_resource, rhi::ResourceState::shader_write);
            },
            [taau, pipeline = s.taau_pipeline, ow, oh](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(taau);
                ctx.cmd.dispatch((ow + 7) / 8, (oh + 7) / 8);
            });
        shown = out_resource;
        shown_size = {ow, oh};
    }

    // ---- display: AgX (exposure was applied before upscaling, ADR 0043); debug views as they are.
    DisplayParams display;
    display.size = {ow, oh};
    display.input_size = shown_size;
    display.exposure = 1.0f;
    display.tonemap = image || accumulating ? 1u : 0u;
    graph.add_compute_pass(
        "display",
        [&](PassBuilder& b) {
            b.read(shown);
            b.write(frame.scene_color, rhi::ResourceState::shader_write);
        },
        [display, pipeline = s.display_pipeline, shown, color = frame.scene_color](PassContext& ctx) mutable {
            display.radiance = ctx.device.descriptor(ctx.texture(shown));
            display.output = ctx.device.descriptor(ctx.texture(color), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(display);
            ctx.cmd.dispatch((display.size[0] + 7) / 8, (display.size[1] + 7) / 8);
        });

    v.history = true;
    ++v.frame;
}

}  // namespace veach::render
