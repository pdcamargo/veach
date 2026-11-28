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
#include <optional>
#include <vector>

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

/// Mirrors ShadeParams in shaders/realtime_shade.slang.
struct alignas(16) ShadeParams {
    math::mat4 inverse_view_projection{1.0f};
    math::vec4 camera_position{0.0f};  ///< xyz; w = exposure
    rhi::DescriptorHandle visibility;
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle lights;
    rhi::DescriptorHandle sky;
    std::array<std::uint32_t, 2> size{};
    std::uint32_t light_count = 0;
    std::uint32_t flags = 0;               ///< shade_auto_exposure, shade_no_fp16_clamp
    rhi::DescriptorHandle exposure_state;  ///< spike SP-14 (VCH-090)
    std::array<std::uint32_t, 2> pad{};
};
static_assert(rhi::parameters_match<ShadeParams>(
    veach_render_shaders::layout::realtime_shade,
    {VEACH_PARAM(ShadeParams, inverse_view_projection), VEACH_PARAM(ShadeParams, camera_position),
     VEACH_PARAM(ShadeParams, visibility), VEACH_PARAM(ShadeParams, radiance), VEACH_PARAM(ShadeParams, instances),
     VEACH_PARAM(ShadeParams, materials), VEACH_PARAM(ShadeParams, lights), VEACH_PARAM(ShadeParams, sky),
     VEACH_PARAM(ShadeParams, size), VEACH_PARAM(ShadeParams, light_count), VEACH_PARAM(ShadeParams, flags),
     VEACH_PARAM(ShadeParams, exposure_state), VEACH_PARAM(ShadeParams, pad)}));
constexpr std::uint32_t shade_auto_exposure = 1u;
constexpr std::uint32_t shade_no_fp16_clamp = 2u;

/// Mirrors ExposureParams in shaders/exposure.slang (spike SP-14, VCH-090).
struct alignas(16) ExposureParams {
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle histogram;
    rhi::DescriptorHandle state;
    std::array<std::uint32_t, 2> size{};
    float histogram_min_ev = 0.0f;
    float histogram_max_ev = 0.0f;
    float min_ev100 = 0.0f;
    float max_ev100 = 0.0f;
    float low_percent = 0.0f;
    float high_percent = 0.0f;
    float speed_to_bright = 0.0f;
    float speed_to_dark = 0.0f;
    float max_rate_to_bright = 0.0f;
    float max_rate_to_dark = 0.0f;
    float compensation = 0.0f;
    float delta_time = 0.0f;
    float highlight_percent = 0.0f;
    float highlight_white = 0.0f;
    std::uint32_t reset = 0;
    float highlight_max_pull = 0.0f;
};
static_assert(rhi::parameters_match<ExposureParams>(veach_render_shaders::layout::exposure,
                                                    {VEACH_PARAM(ExposureParams, radiance),
                                                     VEACH_PARAM(ExposureParams, histogram),
                                                     VEACH_PARAM(ExposureParams, state),
                                                     VEACH_PARAM(ExposureParams, size),
                                                     VEACH_PARAM(ExposureParams, histogram_min_ev),
                                                     VEACH_PARAM(ExposureParams, histogram_max_ev),
                                                     VEACH_PARAM(ExposureParams, min_ev100),
                                                     VEACH_PARAM(ExposureParams, max_ev100),
                                                     VEACH_PARAM(ExposureParams, low_percent),
                                                     VEACH_PARAM(ExposureParams, high_percent),
                                                     VEACH_PARAM(ExposureParams, speed_to_bright),
                                                     VEACH_PARAM(ExposureParams, speed_to_dark),
                                                     VEACH_PARAM(ExposureParams, max_rate_to_bright),
                                                     VEACH_PARAM(ExposureParams, max_rate_to_dark),
                                                     VEACH_PARAM(ExposureParams, compensation),
                                                     VEACH_PARAM(ExposureParams, delta_time),
                                                     VEACH_PARAM(ExposureParams, highlight_percent),
                                                     VEACH_PARAM(ExposureParams, highlight_white),
                                                     VEACH_PARAM(ExposureParams, reset),
                                                     VEACH_PARAM(ExposureParams, highlight_max_pull)}));
/// Mirrors CompositeParams in shaders/backdrop_composite.slang (the space backdrop, ADR 0090).
struct alignas(16) BackdropCompositeParams {
    math::mat4 inverse_view_projection{1.0f};
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle sky;
    rhi::DescriptorHandle exposure_state;
    std::array<std::uint32_t, 2> size{};
    float exposure = 1.0f;
    std::uint32_t flags = 0;  ///< composite_auto_exposure
};
static_assert(rhi::parameters_match<BackdropCompositeParams>(
    veach_render_shaders::layout::backdrop_composite,
    {VEACH_PARAM(BackdropCompositeParams, inverse_view_projection), VEACH_PARAM(BackdropCompositeParams, radiance),
     VEACH_PARAM(BackdropCompositeParams, sky), VEACH_PARAM(BackdropCompositeParams, exposure_state),
     VEACH_PARAM(BackdropCompositeParams, size), VEACH_PARAM(BackdropCompositeParams, exposure),
     VEACH_PARAM(BackdropCompositeParams, flags)}));
constexpr std::uint32_t composite_auto_exposure = 1u;

constexpr std::uint32_t exposure_histogram_size = 132;  ///< 128 bins and the counters (exposure.slang)
constexpr std::uint32_t exposure_state_size = 4;        ///< float4s

constexpr std::uint32_t no_geometry = 0xFFFFFFFFu;  ///< the visibility buffer's clear value

}  // namespace

struct RealtimeRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle visibility_pipeline;
    rhi::PipelineHandle shade_pipeline;
    rhi::PipelineHandle display_pipeline;
    rhi::PipelineHandle backdrop_pipeline;  ///< the space backdrop's composite (ADR 0090)

    // Spike SP-14 (VCH-090): auto-exposure, created when a view first asks for it.
    struct ExposureReadback {
        rhi::BufferHandle buffer;
        std::uint64_t frame = 0;  ///< the frame whose state it holds
        bool pending = false;
        bool report = false;
    };
    rhi::PipelineHandle histogram_pipeline;
    rhi::PipelineHandle resolve_pipeline;
    rhi::BufferHandle histogram;
    rhi::BufferHandle exposure_state;
    bool exposure_written = false;
    std::vector<ExposureReadback> exposure_readbacks;

    Impl() = default;
    ~Impl() {
        if (device == nullptr) {
            return;
        }
        if (!exposure_readbacks.empty()) {
            device->wait_idle();
            std::vector<ExposureReadback*> pending;
            for (ExposureReadback& r : exposure_readbacks) {
                pending.push_back(&r);
            }
            std::ranges::sort(pending, {}, &ExposureReadback::frame);
            for (ExposureReadback* r : pending) {
                check(*r);
            }
            for (ExposureReadback& r : exposure_readbacks) {
                device->destroy(r.buffer);
            }
        }
        for (rhi::BufferHandle* b : {&histogram, &exposure_state}) {
            if (b->is_valid()) {
                device->destroy(*b);
            }
        }
        for (rhi::PipelineHandle* p : {&visibility_pipeline, &shade_pipeline, &display_pipeline, &backdrop_pipeline,
                                       &histogram_pipeline, &resolve_pipeline}) {
            if (p->is_valid()) {
                device->destroy(*p);
            }
        }
    }

    /// Creates the exposure pipelines and buffers; the state starts at `ev100`, uninitialized (the first resolve
    /// snaps to the metered value).
    Result<> ensure_exposure(float ev100) {
        if (exposure_state.is_valid()) {
            return {};
        }
        auto histogram_pass = device->create_compute_pipeline(
            {&veach_render_shaders::exposure(), "exposure_histogram", {16, 16, 1}, "exposure histogram"});
        if (!histogram_pass) {
            return std::unexpected(histogram_pass.error());
        }
        histogram_pipeline = *histogram_pass;
        auto resolve_pass = device->create_compute_pipeline(
            {&veach_render_shaders::exposure(), "exposure_resolve", {128, 1, 1}, "exposure resolve"});
        if (!resolve_pass) {
            return std::unexpected(resolve_pass.error());
        }
        resolve_pipeline = *resolve_pass;
        const std::array<std::uint32_t, exposure_histogram_size> zeros{};
        auto bins = device->create_buffer({.size = sizeof(zeros),
                                           .usage = rhi::BufferUsage::storage,
                                           .debug_name = "exposure histogram",
                                           .stride = sizeof(std::uint32_t)},
                                          std::as_bytes(std::span(zeros)));
        if (!bins) {
            return std::unexpected(bins.error());
        }
        histogram = *bins;
        const float exposure = 1.0f / (1.2f * std::exp2(ev100));
        const std::array<math::vec4, exposure_state_size> initial{math::vec4(ev100, exposure, exposure, 0.0f)};
        auto state = device->create_buffer({.size = sizeof(initial),
                                            .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src,
                                            .debug_name = "exposure state",
                                            .stride = sizeof(math::vec4)},
                                           std::as_bytes(std::span(initial)));
        if (!state) {
            return std::unexpected(state.error());
        }
        exposure_state = *state;
        for (std::uint32_t i = 0; i < device->frames_in_flight(); ++i) {
            auto readback = device->create_buffer({.size = sizeof(initial),
                                                   .usage = rhi::BufferUsage::copy_dst,
                                                   .memory = rhi::MemoryType::readback,
                                                   .debug_name = "exposure state readback"});
            if (!readback) {
                return std::unexpected(readback.error());
            }
            exposure_readbacks.push_back({*readback});
        }
        return {};
    }

    /// Reports a finished frame's exposure: an error for non-finite pixels, the state when the view asked.
    void check(ExposureReadback& r) const {
        if (!r.pending) {
            return;
        }
        r.pending = false;
        const auto* state = static_cast<const math::vec4*>(device->map(r.buffer));
        if (state == nullptr) {
            return;
        }
        const auto nonfinite = static_cast<std::uint64_t>(state[2].x);
        if (nonfinite != 0) {
            log::error(
                "render",
                Error{ErrorCode::render_nonfinite_pixels,
                      std::format("{} pixels of the HDR target were NaN or infinite (frame {})", nonfinite, r.frame),
                      "Infinity: a radiance above the fp16 maximum reached the target unclamped (the sun at a "
                      "dark exposure). NaN: a material, light or sky divided by zero."});
        }
        if (r.report) {
            log::info("exposure",
                      "frame {} ev100 {:.4f} target {:.4f} metered {:.4f} band {:.3f} {:.3f} pre_exposure {:.6g} "
                      "nonfinite {} saturated {} below {} metered_pixels {} white {:.6f} black {:.6f} highlight {:.4f}",
                      r.frame, state[0].x, state[1].x, state[1].y, state[1].z, state[1].w, state[0].z, nonfinite,
                      static_cast<std::uint64_t>(state[2].y), static_cast<std::uint64_t>(state[2].z),
                      static_cast<std::uint64_t>(state[2].w), state[3].x, state[3].y, state[3].w);
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
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
    visibility.cull = rhi::CullMode::none;               // every surface is two-sided, as in photo mode
    visibility.debug_name = "visibility";
    auto visibility_pipeline = device.create_graphics_pipeline(visibility);
    if (!visibility_pipeline) {
        return std::unexpected(visibility_pipeline.error());
    }
    s.visibility_pipeline = *visibility_pipeline;
    auto shade = device.create_compute_pipeline({&shaders.shade, "shade", {8, 8, 1}, "shade"});
    if (!shade) {
        return std::unexpected(shade.error());
    }
    s.shade_pipeline = *shade;
    auto display = device.create_compute_pipeline({&veach_render_shaders::display(), "display", {8, 8, 1}, "display"});
    if (!display) {
        return std::unexpected(display.error());
    }
    s.display_pipeline = *display;
    auto backdrop = device.create_compute_pipeline(
        {&veach_render_shaders::backdrop_composite(), "backdrop_composite", {8, 8, 1}, "space backdrop"});
    if (!backdrop) {
        return std::unexpected(backdrop.error());
    }
    s.backdrop_pipeline = *backdrop;
    return {};
}

void RealtimeRenderer::record(RenderGraph& graph, const FrameContext& frame, const RenderView& view,
                              const RealtimeScene& scene) {
    VEACH_ZONE("RealtimeRenderer::record");
    Impl& s = *impl_;
    const std::uint32_t width = frame.width;
    const std::uint32_t height = frame.height;
    const GraphResource visibility =
        graph.create_texture({width, height, rhi::Format::rg32_uint,
                              rhi::TextureUsage::color_target | rhi::TextureUsage::sampled, "visibility"});
    const GraphResource depth = graph.create_texture(
        {width, height, rhi::Format::depth32_float, rhi::TextureUsage::depth_target, "visibility depth"});
    const GraphResource radiance =
        graph.create_texture({width, height, rhi::Format::rgba16_float,
                              rhi::TextureUsage::storage | rhi::TextureUsage::sampled, "radiance"});

    // ---- visibility (ADR 0040): one draw per instance, one draw instance per meshlet, vertex pulling.
    VisibilityParams vis{.view_projection = view.view_projection};
    graph.add_graphics_pass(
        "visibility",
        [&](PassBuilder& b) {
            b.color_target_uint(visibility, rhi::LoadOp::clear, {no_geometry, no_geometry, 0u, 0u});
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
                ctx.cmd.draw(meshlet_max_triangles * 3, counts[i]);
            }
        });

    // ---- spike SP-14 (VCH-090): auto-exposure's state, from the previous frame's resolve (pre-exposure).
    const std::optional<AutoExposureSettings>& auto_exposure = view.auto_exposure;
    std::optional<GraphResource> exposure_state;
    std::optional<GraphResource> exposure_histogram;
    if (auto_exposure) {
        if (auto ready = s.ensure_exposure(std::log2(1.0f / (1.2f * view.exposure))); !ready) {
            log::error("render", ready.error());
        } else {
            exposure_state =
                graph.import(s.exposure_state, "exposure state",
                             s.exposure_written ? rhi::ResourceState::copy_src : rhi::ResourceState::undefined);
            exposure_histogram =
                graph.import(s.histogram, "exposure histogram",
                             s.exposure_written ? rhi::ResourceState::shader_write : rhi::ResourceState::undefined);
            s.exposure_written = true;
        }
    }

    // ---- shade: lights, the sky's ambient light, emission, the sky and aerial perspective, exposed.
    ShadeParams shade{.inverse_view_projection = view.inverse_view_projection,
                      .camera_position = math::vec4(view.position, view.exposure),
                      .size = {width, height},
                      .light_count = scene.light_count};
    if (exposure_state) {
        shade.flags = shade_auto_exposure | (auto_exposure->clamp_fp16 ? 0u : shade_no_fp16_clamp);
    }
    graph.add_compute_pass(
        "shade",
        [&](PassBuilder& b) {
            scene.read_geometry(b);
            scene.read_sky(b);
            b.read(visibility);
            if (exposure_state) {
                b.read(*exposure_state);
            }
            b.write(radiance, rhi::ResourceState::shader_write);
        },
        [shade, pipeline = s.shade_pipeline, visibility, radiance, instances = scene.instances,
         materials = scene.materials, lights = scene.lights, sky = scene.sky,
         exposure = exposure_state](PassContext& ctx) mutable {
            if (exposure) {
                shade.exposure_state = ctx.device.descriptor(ctx.buffer(*exposure));
            }
            shade.visibility = ctx.device.descriptor(ctx.texture(visibility));
            shade.radiance = ctx.device.descriptor(ctx.texture(radiance), rhi::View::storage);
            shade.instances = ctx.device.descriptor(instances);
            shade.materials = ctx.device.descriptor(materials);
            shade.lights = ctx.device.descriptor(lights);
            shade.sky = ctx.device.descriptor(sky);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(shade);
            ctx.cmd.dispatch((shade.size[0] + 7) / 8, (shade.size[1] + 7) / 8);
        });

    // ---- the space backdrop (ADR 0090): beyond the sky, on the pixels the shade pass marked, before metering.
    if (frame.world.sky.backdrop && frame.world.sky.backdrop_settings.visible) {
        BackdropCompositeParams composite{.inverse_view_projection = view.inverse_view_projection,
                                          .size = {width, height},
                                          .exposure = view.exposure,
                                          .flags = exposure_state ? composite_auto_exposure : 0u};
        graph.add_compute_pass(
            "space backdrop",
            [&](PassBuilder& b) {
                scene.read_sky(b);
                if (exposure_state) {
                    b.read(*exposure_state);
                }
                b.write(radiance, rhi::ResourceState::shader_write);
            },
            [composite, pipeline = s.backdrop_pipeline, radiance, sky = scene.sky,
             exposure = exposure_state](PassContext& ctx) mutable {
                if (exposure) {
                    composite.exposure_state = ctx.device.descriptor(ctx.buffer(*exposure));
                }
                composite.radiance = ctx.device.descriptor(ctx.texture(radiance), rhi::View::storage);
                composite.sky = ctx.device.descriptor(sky);
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(composite);
                ctx.cmd.dispatch((composite.size[0] + 7) / 8, (composite.size[1] + 7) / 8);
            });
    }

    // ---- spike SP-14 (VCH-090): meter the pre-exposed radiance, adapt, and keep the result on the GPU.
    if (exposure_state) {
        const AutoExposureSettings& a = *auto_exposure;
        ExposureParams exposure{.size = {width, height},
                                .histogram_min_ev = a.histogram_min_ev,
                                .histogram_max_ev = a.histogram_max_ev,
                                .min_ev100 = a.min_ev100,
                                .max_ev100 = a.max_ev100,
                                .low_percent = a.low_percent,
                                .high_percent = a.high_percent,
                                .speed_to_bright = a.speed_to_bright,
                                .speed_to_dark = a.speed_to_dark,
                                .max_rate_to_bright = a.max_rate_to_bright,
                                .max_rate_to_dark = a.max_rate_to_dark,
                                .compensation = a.compensation,
                                .delta_time = a.delta_time,
                                .highlight_percent = a.highlight_percent,
                                .highlight_white = a.highlight_white,
                                .reset = a.reset ? 1u : 0u,
                                .highlight_max_pull = a.highlight_max_pull};
        const GraphResource state = *exposure_state;
        const GraphResource bins = *exposure_histogram;
        graph.add_compute_pass(
            "exposure histogram",
            [&](PassBuilder& b) {
                b.read(radiance);
                b.write(state, rhi::ResourceState::shader_write);  // read only; declared storage for its view
                b.write(bins, rhi::ResourceState::shader_write);
            },
            [exposure, pipeline = s.histogram_pipeline, radiance, state, bins](PassContext& ctx) mutable {
                exposure.radiance = ctx.device.descriptor(ctx.texture(radiance));
                exposure.histogram = ctx.device.descriptor(ctx.buffer(bins), rhi::View::storage);
                exposure.state = ctx.device.descriptor(ctx.buffer(state), rhi::View::storage);
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(exposure);
                ctx.cmd.dispatch((exposure.size[0] + 15) / 16, (exposure.size[1] + 15) / 16);
            });
        graph.add_compute_pass(
            "exposure resolve",
            [&](PassBuilder& b) {
                b.write(bins, rhi::ResourceState::shader_write);
                b.write(state, rhi::ResourceState::shader_write);
            },
            [exposure, pipeline = s.resolve_pipeline, state, bins](PassContext& ctx) mutable {
                exposure.histogram = ctx.device.descriptor(ctx.buffer(bins), rhi::View::storage);
                exposure.state = ctx.device.descriptor(ctx.buffer(state), rhi::View::storage);
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(exposure);
                ctx.cmd.dispatch(1, 1);
            });
    }

    // ---- display: AgX on the already exposed radiance (ADR 0037).
    DisplayParams display;
    display.size = {width, height};
    display.input_size = {width, height};
    display.exposure = 1.0f;
    display.tonemap = exposure_state ? 3u : 1u;
    graph.add_compute_pass(
        "display",
        [&](PassBuilder& b) {
            b.read(radiance);
            if (exposure_state) {
                b.read(*exposure_state);
            }
            b.write(frame.scene_color, rhi::ResourceState::shader_write);
        },
        [display, pipeline = s.display_pipeline, radiance, color = frame.scene_color,
         exposure = exposure_state](PassContext& ctx) mutable {
            if (exposure) {
                display.exposure_state = ctx.device.descriptor(ctx.buffer(*exposure));
            }
            display.radiance = ctx.device.descriptor(ctx.texture(radiance));
            display.output = ctx.device.descriptor(ctx.texture(color), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(display);
            ctx.cmd.dispatch((display.size[0] + 7) / 8, (display.size[1] + 7) / 8);
        });

    // ---- spike SP-14: this slot's previous frame has completed (begin_frame waited for it): report it, then copy
    // this frame's state for the next time the slot comes round. Nothing waits for it.
    if (exposure_state) {
        Impl::ExposureReadback& slot =
            s.exposure_readbacks.at(s.device->current_frame().slot % s.exposure_readbacks.size());
        s.check(slot);
        const GraphResource readback = graph.import(slot.buffer, "exposure readback", rhi::ResourceState::undefined);
        const GraphResource state = *exposure_state;
        graph.add_transfer_pass(
            "exposure readback",
            [&](PassBuilder& b) {
                b.read(state, rhi::ResourceState::copy_src);
                b.write(readback, rhi::ResourceState::copy_dst);
            },
            [state, readback](PassContext& ctx) {
                ctx.cmd.copy_buffer(ctx.buffer(state), ctx.buffer(readback), exposure_state_size * sizeof(math::vec4));
            });
        slot.frame = frame.world.frame;
        slot.pending = true;
        slot.report = auto_exposure->report;
    }
}

}  // namespace veach::render
