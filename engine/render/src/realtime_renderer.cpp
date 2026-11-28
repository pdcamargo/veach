#include "realtime_renderer.hpp"

#include "ambient_occlusion.hpp"
#include "bloom_renderer.hpp"

#include <veach/asset/scene.hpp>
#include <veach/core/env.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include "gpu_cull.hpp"
#include "scene_records.hpp"
#include "shadows.hpp"
#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace veach::render {

namespace {

constexpr std::uint32_t max_bins = 32;  ///< classify.slang's header: the sky and up to 31 material kinds

/// Mirrors HizParams in shaders/hiz.slang.
struct HizParams {
    rhi::DescriptorHandle source;
    rhi::DescriptorHandle target;
    std::array<std::uint32_t, 2> source_size{};
    std::array<std::uint32_t, 2> target_size{};
};
static_assert(rhi::parameters_match<HizParams>(veach_render_shaders::layout::hiz,
                                               {VEACH_PARAM(HizParams, source), VEACH_PARAM(HizParams, target),
                                                VEACH_PARAM(HizParams, source_size),
                                                VEACH_PARAM(HizParams, target_size)}));

/// Mirrors ClassifyParams in shaders/classify.slang.
struct alignas(16) ClassifyParams {
    math::mat4 inverse_view_projection{1.0f};
    math::mat4 previous_view_projection{1.0f};
    rhi::DescriptorHandle visibility;
    rhi::DescriptorHandle motion;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle bin_args;
    rhi::DescriptorHandle bin_tiles;
    std::array<std::uint32_t, 2> size{};
    std::uint32_t bin_count = 0;
    std::uint32_t tile_capacity = 0;
};
static_assert(rhi::parameters_match<ClassifyParams>(
    veach_render_shaders::layout::classify,
    {VEACH_PARAM(ClassifyParams, inverse_view_projection), VEACH_PARAM(ClassifyParams, previous_view_projection),
     VEACH_PARAM(ClassifyParams, visibility), VEACH_PARAM(ClassifyParams, motion),
     VEACH_PARAM(ClassifyParams, instances), VEACH_PARAM(ClassifyParams, materials),
     VEACH_PARAM(ClassifyParams, bin_args), VEACH_PARAM(ClassifyParams, bin_tiles), VEACH_PARAM(ClassifyParams, size),
     VEACH_PARAM(ClassifyParams, bin_count), VEACH_PARAM(ClassifyParams, tile_capacity)}));
constexpr std::uint32_t bin_header = 32;  ///< classify.slang: bin_tiles starts with a count per bin
constexpr std::uint32_t tile_size = 8;

/// Mirrors ShadeParams in shaders/realtime_shade.slang (compiled per bin: realtime_shade_bins.slang).
struct alignas(16) ShadeParams {
    math::mat4 inverse_view_projection{1.0f};
    math::vec4 camera_position{0.0f};  ///< xyz; w = exposure
    rhi::DescriptorHandle visibility;
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle lights;
    rhi::DescriptorHandle sky;
    rhi::DescriptorHandle exposure_state;  ///< spike SP-14 (VCH-090)
    rhi::DescriptorHandle bin_tiles;
    std::array<std::uint32_t, 2> size{};
    std::uint32_t light_count = 0;
    std::uint32_t flags = 0;  ///< shade_auto_exposure, shade_no_fp16_clamp
    std::uint32_t tile_capacity = 0;
    std::uint32_t pad0 = 0;
    rhi::DescriptorHandle shadows;            ///< ShadowFrame (shadows.hpp)
    rhi::DescriptorHandle ambient_occlusion;  ///< with shade_ambient_occlusion (ambient_occlusion.hpp)
};
static_assert(rhi::parameters_match<ShadeParams>(
    veach_render_shaders::layout::realtime_shade_bins,
    {VEACH_PARAM(ShadeParams, inverse_view_projection), VEACH_PARAM(ShadeParams, camera_position),
     VEACH_PARAM(ShadeParams, visibility), VEACH_PARAM(ShadeParams, radiance), VEACH_PARAM(ShadeParams, instances),
     VEACH_PARAM(ShadeParams, materials), VEACH_PARAM(ShadeParams, lights), VEACH_PARAM(ShadeParams, sky),
     VEACH_PARAM(ShadeParams, exposure_state), VEACH_PARAM(ShadeParams, bin_tiles), VEACH_PARAM(ShadeParams, size),
     VEACH_PARAM(ShadeParams, light_count), VEACH_PARAM(ShadeParams, flags), VEACH_PARAM(ShadeParams, tile_capacity),
     VEACH_PARAM(ShadeParams, pad0), VEACH_PARAM(ShadeParams, shadows), VEACH_PARAM(ShadeParams, ambient_occlusion)}));
constexpr std::uint32_t shade_auto_exposure = 1u;
constexpr std::uint32_t shade_no_fp16_clamp = 2u;
constexpr std::uint32_t shade_ambient_occlusion = 4u;
constexpr std::uint32_t shade_show_occlusion = 8u;

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
/// Mirrors CompositeParams in shaders/backdrop_composite.slang (the space backdrop, ADR 0091).
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
constexpr std::uint32_t display_bloom = 4u;  ///< DisplayParams::tonemap: blend the bloom chain in (display.slang)

/// Mirrors TaaParams in shaders/taa.slang (ADR 0061).
struct alignas(16) TaaParams {
    math::mat4 reprojection{1.0f};
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle motion;
    rhi::DescriptorHandle depth;
    rhi::DescriptorHandle visibility;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle history;
    rhi::DescriptorHandle history_meta;
    rhi::DescriptorHandle linear_clamp;
    rhi::DescriptorHandle history_out;
    rhi::DescriptorHandle meta_out;
    rhi::DescriptorHandle resolved;
    rhi::DescriptorHandle exposure_state;
    std::array<std::uint32_t, 2> size{};
    float near_plane = 0.1f;
    float exposure_ratio = 1.0f;
    std::uint32_t flags = 0;  ///< taa_history_valid, taa_auto_exposure
    float min_weight = 0.0f;
    float moving_weight = 0.0f;
    float moving_pixels = 1.0f;
    float clip_gamma = 1.0f;
    float edge_weight = 0.5f;
    float sharpness = 0.0f;
    float pad0 = 0.0f;
};
static_assert(rhi::parameters_match<TaaParams>(
    veach_render_shaders::layout::taa, {VEACH_PARAM(TaaParams, reprojection),   VEACH_PARAM(TaaParams, radiance),
                                        VEACH_PARAM(TaaParams, motion),         VEACH_PARAM(TaaParams, depth),
                                        VEACH_PARAM(TaaParams, visibility),     VEACH_PARAM(TaaParams, instances),
                                        VEACH_PARAM(TaaParams, history),        VEACH_PARAM(TaaParams, history_meta),
                                        VEACH_PARAM(TaaParams, linear_clamp),   VEACH_PARAM(TaaParams, history_out),
                                        VEACH_PARAM(TaaParams, meta_out),       VEACH_PARAM(TaaParams, resolved),
                                        VEACH_PARAM(TaaParams, exposure_state), VEACH_PARAM(TaaParams, size),
                                        VEACH_PARAM(TaaParams, near_plane),     VEACH_PARAM(TaaParams, exposure_ratio),
                                        VEACH_PARAM(TaaParams, flags),          VEACH_PARAM(TaaParams, min_weight),
                                        VEACH_PARAM(TaaParams, moving_weight),  VEACH_PARAM(TaaParams, moving_pixels),
                                        VEACH_PARAM(TaaParams, clip_gamma),     VEACH_PARAM(TaaParams, edge_weight),
                                        VEACH_PARAM(TaaParams, sharpness),      VEACH_PARAM(TaaParams, pad0)}));
constexpr std::uint32_t taa_history_valid = 1u;
constexpr std::uint32_t taa_auto_exposure = 2u;
// The blend (taa.slang), tuned on SP-16's field by eye and by FLIP against 16-sample references, still and moving
// (VCH-144, docs/research/spikes/vch-144-taa.md): this frame weighs at least taa_min_weight in a still pixel, rising
// to taa_moving_weight for one that moves taa_moving_pixels a frame or more, and taa_edge_weight where its surface is
// found only beside where it was; the variance clip keeps taa_clip_gamma standard deviations; the displayed image is
// sharpened by up to taa_sharpness where it moves.
constexpr float taa_min_weight = 1.0f / 12.0f;
constexpr float taa_moving_weight = 0.5f;
constexpr float taa_moving_pixels = 1.0f;
constexpr float taa_edge_weight = 1.0f;
constexpr float taa_clip_gamma = 1.0f;
constexpr float taa_sharpness = 1.0f;

constexpr std::uint32_t exposure_histogram_size = 132;  ///< 128 bins and the counters (exposure.slang)
constexpr std::uint32_t exposure_state_size = 4;        ///< float4s

constexpr std::uint32_t no_geometry = 0xFFFFFFFFu;  ///< the visibility buffer's clear value

/// The frustum's side and near planes of a reverse-Z, infinite-far view_projection (column vectors): inside where
/// dot(xyz, p) + w >= 0, normalized so that is a distance.
std::array<math::vec4, 5> frustum_planes(const math::mat4& m) {
    const auto row = [&](int i) {
        return math::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
    };
    std::array<math::vec4, 5> planes{row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1),
                                     row(3) - row(2)};
    for (math::vec4& p : planes) {
        p /= math::length(math::vec3(p));
    }
    return planes;
}

}  // namespace

struct RealtimeRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle cull_reset;
    rhi::PipelineHandle cull_instances;
    rhi::PipelineHandle cull_meshlets;
    rhi::PipelineHandle cull_recheck;
    rhi::PipelineHandle cull_args;
    rhi::PipelineHandle hiz_reduce;
    rhi::PipelineHandle visibility_opaque;
    rhi::PipelineHandle visibility_masked;
    rhi::PipelineHandle classify;
    rhi::PipelineHandle classify_args;
    std::vector<rhi::PipelineHandle> shade;  ///< per bin: the sky, then each material kind
    rhi::PipelineHandle display_pipeline;
    rhi::PipelineHandle backdrop_pipeline;       ///< the space backdrop's composite (ADR 0091)
    AmbientOcclusionRenderer ambient_occlusion;  ///< ADR 0096
    BloomRenderer bloom;                         ///< docs/specs/bloom/spec.md

    /// A persistent buffer of culling's, grown on demand. Every frame leaves it in shader_read.
    struct Buffer {
        rhi::BufferHandle handle;
        std::uint64_t size = 0;
        bool written = false;
    };
    Buffer counters;
    Buffer draw_args;
    Buffer dispatch_args;
    Buffer bin_args;
    Buffer tasks;
    Buffer recheck_instances;
    Buffer recheck_meshlets;
    Buffer visible;
    Buffer bin_tiles;
    std::vector<rhi::BufferHandle> views;  ///< a CullView per frame slot, in upload memory
    ShadowRenderer shadows;                ///< the sun shadow maps (shadows.hpp)

    /// Last frame's HiZ pyramid: phase 1's occluders. Rebuilt every frame from phase 1's depth.
    rhi::TextureHandle hiz;
    std::array<std::uint32_t, 2> hiz_size{};
    std::uint32_t hiz_levels = 0;
    bool hiz_written = false;

    /// What the frame before saw, for phase 1's occlusion test and motion vectors.
    struct Previous {
        math::mat4 view_projection{1.0f};           ///< unjittered: motion vectors and the history
        math::mat4 jittered_view_projection{1.0f};  ///< what it rasterized: its HiZ pyramid's
        math::dvec3 origin{0.0};
        std::uint64_t space_frame = 0;
        std::uint64_t camera = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool valid = false;
    };
    Previous previous;
    CullStats stats;

    /// Temporal AA's history (ADR 0061): two of each, written in turn, renderer data outside the ECS (ADR 0020).
    struct History {
        std::array<rhi::TextureHandle, 2> color;  ///< rgba16_float: the resolved radiance, alpha 1 + backdrop share
        std::array<rhi::TextureHandle, 2> meta;   ///< rg32_uint: surface id | samples << 24, linear depth
        std::array<std::uint32_t, 2> size{};
        std::uint32_t last = 0;      ///< the pair last frame wrote
        bool valid = false;          ///< last frame wrote it, for this camera, render frame and size
        std::uint64_t frame = 0;     ///< the device frame that wrote it
        float exposure = 0.0f;       ///< the pre-exposure it was written with (without auto-exposure)
        bool auto_exposure = false;  ///< whether it was written under auto-exposure
    };
    History history;
    rhi::PipelineHandle taa_pipeline;
    rhi::PipelineHandle taa_sharpen_pipeline;
    rhi::SamplerHandle linear_clamp;

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
        for (Buffer* b : {&counters, &draw_args, &dispatch_args, &bin_args, &tasks, &recheck_instances,
                          &recheck_meshlets, &visible, &bin_tiles}) {
            if (b->handle.is_valid()) {
                device->destroy(b->handle);
            }
        }
        for (const rhi::BufferHandle b : views) {
            device->destroy(b);
        }
        if (hiz.is_valid()) {
            device->destroy(hiz);
        }
        destroy_history();
        if (linear_clamp.is_valid()) {
            device->destroy(linear_clamp);
        }
        std::vector<rhi::PipelineHandle*> pipelines{
            &cull_reset,         &cull_instances,    &cull_meshlets,    &cull_recheck,
            &cull_args,          &hiz_reduce,        &classify,         &classify_args,
            &visibility_opaque,  &visibility_masked, &display_pipeline, &backdrop_pipeline,
            &histogram_pipeline, &resolve_pipeline,  &taa_pipeline,     &taa_sharpen_pipeline};
        for (rhi::PipelineHandle& p : shade) {
            pipelines.push_back(&p);
        }
        for (rhi::PipelineHandle* p : pipelines) {
            if (p->is_valid()) {
                device->destroy(*p);
            }
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    /// `b` holds at least `size` bytes (grown by half again when it must grow; its contents are not kept).
    Result<> ensure(Buffer& b, std::uint64_t size, rhi::BufferUsage usage, std::uint32_t stride, const char* name) {
        size = std::max<std::uint64_t>(size, 16);
        if (b.handle.is_valid() && b.size >= size) {
            return {};
        }
        if (b.handle.is_valid()) {
            device->destroy(b.handle);  // deferred until the frames in flight are done with it
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

    /// The pyramid for a width x height depth buffer: mip 0 is half of it, rounded down, down to 1 x 1.
    Result<> ensure_hiz(std::uint32_t width, std::uint32_t height) {
        const std::array<std::uint32_t, 2> size{std::max(1u, width / 2), std::max(1u, height / 2)};
        if (hiz.is_valid() && size == hiz_size) {
            return {};
        }
        if (hiz.is_valid()) {
            device->destroy(hiz);
            hiz = {};
        }
        const auto levels = static_cast<std::uint32_t>(std::bit_width(std::max(size[0], size[1])));
        auto texture =
            device->create_texture({size[0], size[1], rhi::Format::r32_float,
                                    rhi::TextureUsage::storage | rhi::TextureUsage::sampled, "HiZ pyramid", levels});
        if (!texture) {
            hiz_size = {};
            return std::unexpected(texture.error());
        }
        hiz = *texture;
        hiz_size = size;
        hiz_levels = levels;
        hiz_written = false;
        return {};
    }

    void destroy_history() {
        for (auto* set : {&history.color, &history.meta}) {
            for (rhi::TextureHandle& t : *set) {
                if (t.is_valid()) {
                    device->destroy(t);  // deferred until the frames in flight are done with it
                    t = {};
                }
            }
        }
        history.size = {};
        history.valid = false;
    }

    /// Temporal AA's history textures at width x height; new ones (after a resize) start invalid.
    Result<> ensure_history(std::uint32_t width, std::uint32_t height) {
        if (history.color[0].is_valid() && history.size == std::array<std::uint32_t, 2>{width, height}) {
            return {};
        }
        destroy_history();
        constexpr rhi::TextureUsage usage = rhi::TextureUsage::storage | rhi::TextureUsage::sampled;
        for (std::size_t i = 0; i < 2; ++i) {
            auto color = device->create_texture({width, height, rhi::Format::rgba16_float, usage, "TAA history"});
            auto meta = device->create_texture({width, height, rhi::Format::rg32_uint, usage, "TAA history ids"});
            if (!color || !meta) {
                if (color) {
                    device->destroy(*color);
                }
                if (meta) {
                    device->destroy(*meta);
                }
                destroy_history();
                return std::unexpected(color ? meta.error() : color.error());
            }
            history.color.at(i) = *color;
            history.meta.at(i) = *meta;
        }
        history.size = {width, height};
        return {};
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
};

RealtimeRenderer::RealtimeRenderer() : impl_(std::make_unique<Impl>()) {}

RealtimeRenderer::~RealtimeRenderer() = default;

const CullStats& RealtimeRenderer::stats() const {
    return impl_->stats;
}

Result<> RealtimeRenderer::prepare(rhi::Device& device, const SceneShaders& shaders, std::uint32_t kind_count) {
    Impl& s = *impl_;
    s.device = &device;
    if (kind_count + 1 > max_bins) {
        return make_error(ErrorCode::render_graph_invalid,
                          std::format("the real-time renderer bins at most {} material kinds; this app has {}",
                                      max_bins - 1, kind_count),
                          "Merge material kinds (ADR 0035), or raise classify.slang's bin header.");
    }
    const auto compute = [&](const rhi::ShaderLibrary& library, std::string_view entry,
                             std::array<std::uint32_t, 3> group, rhi::PipelineHandle& out) -> Result<> {
        auto pipeline = device.create_compute_pipeline({&library, entry, group, std::string(entry)});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        out = *pipeline;
        return {};
    };
    const rhi::ShaderLibrary& cull = veach_render_shaders::gpu_cull();
    for (const auto& [entry, group, out] :
         {std::tuple{"cull_reset", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_reset},
          std::tuple{"cull_instances", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_instances},
          std::tuple{"cull_meshlets", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_meshlets},
          std::tuple{"cull_recheck_meshlets", std::array<std::uint32_t, 3>{64, 1, 1}, &s.cull_recheck},
          std::tuple{"cull_args", std::array<std::uint32_t, 3>{1, 1, 1}, &s.cull_args}}) {
        if (auto made = compute(cull, entry, group, *out); !made) {
            return made;
        }
    }
    if (auto made = compute(veach_render_shaders::hiz(), "hiz_reduce", {8, 8, 1}, s.hiz_reduce); !made) {
        return made;
    }
    if (auto made = compute(veach_render_shaders::classify(), "classify", {8, 8, 1}, s.classify); !made) {
        return made;
    }
    if (auto made = compute(veach_render_shaders::classify(), "classify_args", {32, 1, 1}, s.classify_args); !made) {
        return made;
    }
    for (const auto& [fragment, out] :
         {std::pair{"vis_fs", &s.visibility_opaque}, std::pair{"vis_fs_masked", &s.visibility_masked}}) {
        rhi::GraphicsPipelineDesc visibility;
        visibility.library = &shaders.visibility;
        visibility.vertex_entry = "vis_vs";
        visibility.fragment_entry = fragment;
        visibility.color_formats = {rhi::Format::rg32_uint, rhi::Format::rg16_float};
        visibility.depth_format = rhi::Format::depth32_float;
        visibility.depth_test = true;
        visibility.depth_write = true;
        visibility.depth_compare = rhi::CompareOp::greater;  // reverse-Z: nearer is greater
        visibility.cull = rhi::CullMode::none;               // every surface is two-sided, as in photo mode
        visibility.debug_name = std::string("visibility ") + fragment;
        auto pipeline = device.create_graphics_pipeline(visibility);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    s.shade.assign(kind_count + 1, {});
    for (std::uint32_t bin = 0; bin <= kind_count; ++bin) {
        const std::string entry = bin == 0 ? "shade_sky" : std::format("shade_kind_{}", bin - 1);
        if (auto made = compute(shaders.shade, entry, {8, 8, 1}, s.shade[bin]); !made) {
            return made;
        }
    }
    if (auto made = compute(veach_render_shaders::display(), "display", {8, 8, 1}, s.display_pipeline); !made) {
        return made;
    }
    if (auto made =
            compute(veach_render_shaders::backdrop_composite(), "backdrop_composite", {8, 8, 1}, s.backdrop_pipeline);
        !made) {
        return made;
    }
    if (auto made = s.ambient_occlusion.prepare(device); !made) {
        return made;
    }
    if (auto made = s.bloom.prepare(device); !made) {
        return made;
    }
    if (auto made = s.shadows.prepare(device, shaders); !made) {
        return made;
    }
    if (auto made = compute(veach_render_shaders::taa(), "taa_resolve", {8, 8, 1}, s.taa_pipeline); !made) {
        return made;
    }
    if (auto made = compute(veach_render_shaders::taa(), "taa_sharpen", {8, 8, 1}, s.taa_sharpen_pipeline); !made) {
        return made;
    }
    auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::clamp});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.linear_clamp = *sampler;
    for (std::uint32_t i = 0; i < device.frames_in_flight(); ++i) {
        auto view = device.create_buffer({.size = sizeof(CullView),
                                          .memory = rhi::MemoryType::upload,
                                          .debug_name = "cull view",
                                          .stride = sizeof(CullView)});
        if (!view) {
            return std::unexpected(view.error());
        }
        s.views.push_back(*view);
    }
    return {};
}

void RealtimeRenderer::record(RenderGraph& graph, const FrameContext& frame, const RenderView& view,
                              const RealtimeScene& scene) {
    VEACH_ZONE("RealtimeRenderer::record");
    Impl& s = *impl_;
    const std::uint32_t width = frame.width;
    const std::uint32_t height = frame.height;
    const auto bins = static_cast<std::uint32_t>(s.shade.size());

    // ---- continuity with the frame before: the same camera in the same render frame and target size.
    const RenderSpace& space = frame.world.space;
    const bool continuous = s.previous.valid && s.previous.camera == view.camera &&
                            s.previous.space_frame == space.frame && s.previous.width == width &&
                            s.previous.height == height;
    // This frame's jitter (ADR 0061): culling, the visibility pass and shading see the jittered projection; motion
    // vectors, the history and the backdrop the unjittered one.
    const math::mat4 jittered = jittered_view_projection(view.view_projection, view.jitter, width, height);
    const math::mat4 inverse_jittered =
        view.jitter == math::vec2(0.0f) ? view.inverse_view_projection : math::inverse(jittered);
    // Last frame's view in this frame's render space: render space moved by whole 64 m cells (ADR 0045).
    const math::mat4 shift = math::translate(math::mat4(1.0f), math::vec3(space.origin - s.previous.origin));
    const math::mat4 previous_view_projection = continuous ? s.previous.view_projection * shift : view.view_projection;
    const math::mat4 previous_jittered = continuous ? s.previous.jittered_view_projection * shift : jittered;
    // From this frame's jittered clip space to last frame's unjittered one (taa.slang), composed in double.
    const math::mat4 reprojection(math::dmat4(previous_view_projection) * math::inverse(math::dmat4(jittered)));
    s.previous = {view.view_projection, jittered, space.origin, space.frame, view.camera, width, height, true};

    // ---- persistent resources, sized for this frame.
    std::uint32_t meshlets = 0;
    std::array<std::uint32_t, draw_list_count> list_offsets{};
    for (std::uint32_t l = 0; l < draw_list_count; ++l) {
        list_offsets.at(l) = meshlets;
        meshlets += scene.list_capacity.at(l);
    }
    const std::uint32_t tiles_x = (width + tile_size - 1) / tile_size;
    const std::uint32_t tiles = tiles_x * ((height + tile_size - 1) / tile_size);
    constexpr rhi::BufferUsage rw = rhi::BufferUsage::storage;
    constexpr auto u32 = static_cast<std::uint32_t>(sizeof(std::uint32_t));
    constexpr auto u64 = static_cast<std::uint32_t>(2 * sizeof(std::uint32_t));
    for (const Result<>& ready :
         {s.ensure(s.counters, counter_count * u32, rw | rhi::BufferUsage::copy_src, u32, "cull counters"),
          s.ensure(s.draw_args, 2 * draw_list_count * sizeof(rhi::DrawIndirectArgs),
                   rw | rhi::BufferUsage::indirect | rhi::BufferUsage::copy_src, u32, "cull draw args"),
          s.ensure(s.dispatch_args, 4 * dispatch_record, rw | rhi::BufferUsage::indirect, u32, "cull dispatch args"),
          s.ensure(s.bin_args, std::uint64_t{bins} * dispatch_record, rw | rhi::BufferUsage::indirect, u32,
                   "shade bin args"),
          s.ensure(s.tasks, 2ull * scene.task_count * u64, rw, u64, "cull tasks"),
          s.ensure(s.recheck_instances, std::uint64_t{scene.instance_count} * u32, rw, u32, "cull recheck instances"),
          s.ensure(s.recheck_meshlets, std::uint64_t{meshlets} * u64, rw, u64, "cull recheck meshlets"),
          s.ensure(s.visible, 2ull * meshlets * u64, rw, u64, "cull draw lists"),
          s.ensure(s.bin_tiles, (bin_header + std::uint64_t{bins} * tiles) * u32, rw, u32, "shade bin tiles"),
          s.ensure_hiz(width, height)}) {
        if (!ready) {
            log::error("render", ready.error());
            return;
        }
    }
    const bool hiz_valid = continuous && s.hiz_written;

    CullView cull_view{.view_projection = jittered,
                       .occlusion_view_projection = previous_jittered,
                       .planes = frustum_planes(jittered),
                       .camera = math::vec4(view.position, view.near_plane),
                       .screen = {width, height},
                       .hiz_size = s.hiz_size,
                       .hiz_levels = s.hiz_levels,
                       .flags = hiz_valid ? cull_previous_hiz : 0u,
                       .instance_count = scene.instance_count,
                       .phase_stride = meshlets,
                       .task_capacity = scene.task_count};
    std::copy(list_offsets.begin(), list_offsets.end(), cull_view.list_offsets.begin());
    const rhi::BufferHandle view_buffer = s.views.at(s.device->current_frame().slot % s.views.size());
    if (void* mapped = s.device->map(view_buffer); mapped != nullptr) {
        std::memcpy(mapped, &cull_view, sizeof(cull_view));
    }

    const GraphResource view_data = graph.import(view_buffer, "cull view", rhi::ResourceState::shader_read);
    const GraphResource counters = s.import(graph, s.counters, "cull counters");
    const GraphResource draw_args = s.import(graph, s.draw_args, "cull draw args");
    const GraphResource dispatch_args = s.import(graph, s.dispatch_args, "cull dispatch args");
    const GraphResource bin_args = s.import(graph, s.bin_args, "shade bin args");
    const GraphResource tasks = s.import(graph, s.tasks, "cull tasks");
    const GraphResource recheck_instances = s.import(graph, s.recheck_instances, "cull recheck instances");
    const GraphResource recheck_meshlets = s.import(graph, s.recheck_meshlets, "cull recheck meshlets");
    const GraphResource visible = s.import(graph, s.visible, "cull draw lists");
    const GraphResource bin_tiles = s.import(graph, s.bin_tiles, "shade bin tiles");
    const GraphResource hiz = graph.import(
        s.hiz, "HiZ pyramid", s.hiz_written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
        rhi::ResourceState::shader_read);
    s.hiz_written = true;

    const GraphResource visibility =
        graph.create_texture({width, height, rhi::Format::rg32_uint,
                              rhi::TextureUsage::color_target | rhi::TextureUsage::sampled, "visibility"});
    const GraphResource motion = graph.create_texture({width, height, rhi::Format::rg16_float,
                                                       rhi::TextureUsage::color_target | rhi::TextureUsage::storage |
                                                           rhi::TextureUsage::sampled | rhi::TextureUsage::copy_src,
                                                       "motion"});
    const GraphResource depth =
        graph.create_texture({width, height, rhi::Format::depth32_float,
                              rhi::TextureUsage::depth_target | rhi::TextureUsage::sampled, "visibility depth"});
    const GraphResource radiance =
        graph.create_texture({width, height, rhi::Format::rgba16_float,
                              rhi::TextureUsage::storage | rhi::TextureUsage::sampled, "radiance"});
    if (frame.blackboard != nullptr) {
        frame.blackboard->set("visibility", visibility);
        frame.blackboard->set("depth", depth);
        frame.blackboard->set("motion", motion);
    }

    const GraphResource instances_resource = scene.instances;
    const rhi::BufferHandle materials_buffer = scene.materials;
    const std::array<std::uint32_t, draw_list_count> list_capacity = scene.list_capacity;
    // Every culling pass binds every handle; each declares only what its kernels touch.
    const CullParams cull_params{.phase = 0, .bin_count = bins};
    const auto bind = [=](PassContext& ctx, std::uint32_t phase) {
        CullParams p = cull_params;
        p.phase = phase;
        const auto storage = [&](GraphResource r) {
            return ctx.device.descriptor(ctx.buffer(r), rhi::View::storage);
        };
        p.view = ctx.device.descriptor(ctx.buffer(view_data));
        p.instances = ctx.device.descriptor(ctx.buffer(instances_resource));
        p.materials = ctx.device.descriptor(materials_buffer);
        p.hiz = ctx.device.descriptor(ctx.texture(hiz));
        p.counters = storage(counters);
        p.draw_args = storage(draw_args);
        p.dispatch_args = storage(dispatch_args);
        p.tasks = storage(tasks);
        p.recheck_instances = storage(recheck_instances);
        p.recheck_meshlets = storage(recheck_meshlets);
        p.visible = storage(visible);
        p.bin_tiles = storage(bin_tiles);
        ctx.cmd.set_parameters(p);
    };
    const auto read_meshes = scene.read_meshes;

    // ---- culling, phase 1: last frame's pyramid.
    graph.add_compute_pass(
        "cull: reset",
        [&](PassBuilder& b) {
            b.write(counters);
            b.write(draw_args);
            b.write(bin_tiles);
        },
        [=, pipeline = s.cull_reset](PassContext& ctx) {
            ctx.cmd.set_pipeline(pipeline);
            bind(ctx, 0);
            ctx.cmd.dispatch(1, 1);
        });
    const auto cull_instances = [&](std::uint32_t phase) {
        graph.add_compute_pass(
            phase == 0 ? "cull: instances" : "cull: instances (late)",
            [&](PassBuilder& b) {
                read_meshes(b);
                b.read(view_data);
                b.read(hiz);
                if (phase == 1) {
                    b.read(dispatch_args, rhi::ResourceState::indirect_argument);
                }
                b.write(counters);
                b.write(draw_args);
                b.write(tasks);
                b.write(recheck_instances);
                b.write(recheck_meshlets);
                b.write(visible);
            },
            [=, pipeline = s.cull_instances, count = scene.instance_count](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                bind(ctx, phase);
                if (phase == 1) {
                    ctx.cmd.dispatch_indirect(ctx.buffer(dispatch_args), dispatch_recheck_instances);
                } else if (count != 0) {
                    const auto [x, y] = rows_of((count + 63) / 64);
                    ctx.cmd.dispatch(x, y);
                }
            });
    };
    const auto args = [&](const char* name) {
        graph.add_compute_pass(
            name,
            [&](PassBuilder& b) {
                b.write(counters);
                b.write(dispatch_args);
            },
            [=, pipeline = s.cull_args](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                bind(ctx, 0);
                ctx.cmd.dispatch(1, 1);
            });
    };
    const auto cull_meshlets = [&](std::uint32_t phase) {
        graph.add_compute_pass(
            phase == 0 ? "cull: meshlets" : "cull: meshlets (late)",
            [&](PassBuilder& b) {
                read_meshes(b);
                b.read(view_data);
                b.read(hiz);
                b.read(dispatch_args, rhi::ResourceState::indirect_argument);
                b.write(counters);
                b.write(draw_args);
                b.write(tasks);
                b.write(recheck_meshlets);
                b.write(visible);
            },
            [=, meshlets_pipeline = s.cull_meshlets, recheck_pipeline = s.cull_recheck](PassContext& ctx) {
                ctx.cmd.set_pipeline(meshlets_pipeline);
                bind(ctx, phase);
                ctx.cmd.dispatch_indirect(ctx.buffer(dispatch_args), phase == 0 ? dispatch_tasks_1 : dispatch_tasks_2);
                if (phase == 1) {
                    // Appends to the same lists through atomics: no ordering needed between the two.
                    ctx.cmd.set_pipeline(recheck_pipeline);
                    bind(ctx, 1);
                    ctx.cmd.dispatch_indirect(ctx.buffer(dispatch_args), dispatch_recheck_meshlets);
                }
            });
    };
    // ---- visibility: opaque lists first, then alpha-tested ones (ADR 0060).
    const VisibilityParams vis_params{
        .view_projection = jittered,
        .previous_view_projection = previous_view_projection,
        .inverse_size = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)},
        .jitter = {view.jitter.x / static_cast<float>(width), view.jitter.y / static_cast<float>(height)}};
    const auto draw = [&](std::uint32_t phase) {
        graph.add_graphics_pass(
            phase == 0 ? "visibility" : "visibility (late)",
            [&](PassBuilder& b) {
                const rhi::LoadOp load = phase == 0 ? rhi::LoadOp::clear : rhi::LoadOp::load;
                b.color_target_uint(visibility, load, {no_geometry, no_geometry, 0u, 0u});
                b.color_target(motion, load, {0.0f, 0.0f, 0.0f, 0.0f});
                b.depth_target(depth, load, 0.0f);  // reverse-Z: far is 0
                scene.read_geometry(b);
                b.read(visible);
                b.read(draw_args, rhi::ResourceState::indirect_argument);
            },
            [=, opaque = s.visibility_opaque, masked = s.visibility_masked](PassContext& ctx) mutable {
                VisibilityParams p = vis_params;
                p.instances = ctx.device.descriptor(ctx.buffer(instances_resource));
                p.materials = ctx.device.descriptor(materials_buffer);
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
                    p.list_offset = list_offsets.at(l) + phase * meshlets;
                    ctx.cmd.set_parameters(p);
                    ctx.cmd.draw_indirect(ctx.buffer(draw_args),
                                          (phase * draw_list_count + l) * sizeof(rhi::DrawIndirectArgs));
                }
            });
    };
    cull_instances(0);
    args("cull: args");
    cull_meshlets(0);
    draw(0);

    // ---- the pyramid of phase 1's depth: phase 2's occluders now, phase 1's next frame.
    for (std::uint32_t level = 0; level < s.hiz_levels; ++level) {
        const GraphResource target = graph.mips(hiz, level);
        const GraphResource source = level == 0 ? depth : graph.mips(hiz, level - 1);
        const std::array<std::uint32_t, 2> source_size =
            level == 0 ? std::array<std::uint32_t, 2>{width, height}
                       : std::array<std::uint32_t, 2>{std::max(1u, s.hiz_size[0] >> (level - 1)),
                                                      std::max(1u, s.hiz_size[1] >> (level - 1))};
        HizParams hiz_params{
            .source_size = source_size,
            .target_size = {std::max(1u, s.hiz_size[0] >> level), std::max(1u, s.hiz_size[1] >> level)}};
        graph.add_compute_pass(
            std::format("hiz {}", level),
            [&](PassBuilder& b) {
                b.read(source);
                b.write(target);
            },
            [hiz_params, source, target, level, pipeline = s.hiz_reduce](PassContext& ctx) mutable {
                hiz_params.source =
                    level == 0 ? ctx.device.descriptor(ctx.texture(source))
                               : ctx.device.descriptor(ctx.texture(source), rhi::View::read, ctx.range(source));
                hiz_params.target = ctx.device.descriptor(ctx.texture(target), rhi::View::storage, ctx.range(target));
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(hiz_params);
                ctx.cmd.dispatch((hiz_params.target_size[0] + 7) / 8, (hiz_params.target_size[1] + 7) / 8);
            });
    }

    // ---- culling, phase 2: what phase 1 found occluded, against the new pyramid.
    args("cull: args (late)");
    cull_instances(1);
    args("cull: args (late meshlets)");
    cull_meshlets(1);
    draw(1);

    // ---- material binning: the tiles each bin shows, and their dispatches.
    ClassifyParams classify{.inverse_view_projection = view.inverse_view_projection,  // the sky's motion: unjittered
                            .previous_view_projection = previous_view_projection,
                            .size = {width, height},
                            .bin_count = bins,
                            .tile_capacity = tiles};
    const auto bind_classify = [=](PassContext& ctx) {
        ClassifyParams p = classify;
        p.visibility = ctx.device.descriptor(ctx.texture(visibility));
        p.motion = ctx.device.descriptor(ctx.texture(motion), rhi::View::storage);
        p.instances = ctx.device.descriptor(ctx.buffer(instances_resource));
        p.materials = ctx.device.descriptor(materials_buffer);
        p.bin_args = ctx.device.descriptor(ctx.buffer(bin_args), rhi::View::storage);
        p.bin_tiles = ctx.device.descriptor(ctx.buffer(bin_tiles), rhi::View::storage);
        ctx.cmd.set_parameters(p);
    };
    graph.add_compute_pass(
        "classify",
        [&](PassBuilder& b) {
            read_meshes(b);
            b.read(visibility);
            b.write(motion);
            b.write(bin_tiles);
        },
        [=, pipeline = s.classify](PassContext& ctx) {
            ctx.cmd.set_pipeline(pipeline);
            bind_classify(ctx);
            ctx.cmd.dispatch(tiles_x, (height + tile_size - 1) / tile_size);
        });
    graph.add_compute_pass(
        "classify: args",
        [&](PassBuilder& b) {
            b.write(bin_tiles);
            b.write(bin_args);
        },
        [=, pipeline = s.classify_args](PassContext& ctx) {
            ctx.cmd.set_pipeline(pipeline);
            bind_classify(ctx);
            ctx.cmd.dispatch(1, 1);
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

    // ---- the sun shadow maps (shadows.hpp): each cascade culled and drawn on its own.
    const ShadowRenderer::Output shadow_maps = s.shadows.record(graph, frame, view, scene, s.stats);

    // ---- ambient occlusion from the final depth (ADR 0096): the shading's ambient light reads it.
    const std::optional<GraphResource> occlusion = s.ambient_occlusion.record(graph, depth, view, width, height);
    if (occlusion && frame.blackboard != nullptr) {
        frame.blackboard->set("ambient_occlusion", *occlusion);
    }

    // ---- shade, a dispatch per bin: lights, the sky's ambient light, emission, the sky and aerial perspective.
    ShadeParams shade{.inverse_view_projection = inverse_jittered,  // the rays of the pixels the visibility pass drew
                      .camera_position = math::vec4(view.position, view.exposure),
                      .size = {width, height},
                      .light_count = scene.light_count,
                      .tile_capacity = tiles};
    if (exposure_state) {
        shade.flags = shade_auto_exposure | (auto_exposure->clamp_fp16 ? 0u : shade_no_fp16_clamp);
    }
    if (occlusion) {
        shade.flags |= shade_ambient_occlusion;
        // A debugging view: VEACH_SHOW_AO=1 draws the occlusion instead of the lit surfaces.
        shade.flags |= env_is_one("VEACH_SHOW_AO") ? shade_show_occlusion : 0u;
    }
    graph.add_compute_pass(
        "shade",
        [&](PassBuilder& b) {
            scene.read_geometry(b);
            scene.read_sky(b);
            b.read(visibility);
            b.read(bin_tiles);
            b.read(bin_args, rhi::ResourceState::indirect_argument);
            if (exposure_state) {
                b.read(*exposure_state);
            }
            if (shadow_maps.map) {
                b.read(*shadow_maps.map);
            }
            if (occlusion) {
                b.read(*occlusion);
            }
            b.write(radiance, rhi::ResourceState::shader_write);
        },
        [shade, pipelines = s.shade, shadow_frame = shadow_maps.frame, visibility, radiance, bin_tiles, bin_args,
         instances = scene.instances, materials = scene.materials, lights = scene.lights, sky = scene.sky,
         exposure = exposure_state, occlusion](PassContext& ctx) mutable {
            if (exposure) {
                shade.exposure_state = ctx.device.descriptor(ctx.buffer(*exposure));
            }
            if (occlusion) {
                shade.ambient_occlusion = ctx.device.descriptor(ctx.texture(*occlusion));
            }
            shade.visibility = ctx.device.descriptor(ctx.texture(visibility));
            shade.radiance = ctx.device.descriptor(ctx.texture(radiance), rhi::View::storage);
            shade.instances = ctx.device.descriptor(ctx.buffer(instances));
            shade.materials = ctx.device.descriptor(materials);
            shade.lights = ctx.device.descriptor(lights);
            shade.sky = ctx.device.descriptor(sky);
            shade.bin_tiles = ctx.device.descriptor(ctx.buffer(bin_tiles));
            shade.shadows = ctx.device.descriptor(shadow_frame);
            // Bins write disjoint pixels: no barrier between their dispatches.
            for (std::size_t bin = 0; bin < pipelines.size(); ++bin) {
                ctx.cmd.set_pipeline(pipelines[bin]);
                ctx.cmd.set_parameters(shade);
                ctx.cmd.dispatch_indirect(ctx.buffer(bin_args), bin * dispatch_record);
            }
        });

    // ---- temporal AA (ADR 0061): this frame blended with its reprojected history, in pre-exposed radiance, before
    // the backdrop, metering and the display transform.
    GraphResource image = radiance;  ///< what the passes below read: the resolved image with TAA
    const std::uint64_t device_frame = s.device->current_frame().index;
    if (view.temporal_aa) {
        if (auto ready = s.ensure_history(width, height); !ready) {
            log::error("render", ready.error());
        } else {
            Impl::History& h = s.history;
            const bool auto_exposed = exposure_state.has_value();
            // The history is last frame's, for this camera in this render frame at this size (a cut, a frame switch
            // or a resize starts over), under the same exposure control.
            const bool valid = h.valid && continuous && h.frame + 1 == device_frame && h.auto_exposure == auto_exposed;
            const std::uint32_t read = h.last;
            const std::uint32_t write = 1 - h.last;
            // Every frame leaves both pairs in shader_read; an invalid history's contents do not matter.
            const rhi::ResourceState before = valid ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined;
            const GraphResource history_color =
                graph.import(h.color.at(read), "TAA history", before, rhi::ResourceState::shader_read);
            const GraphResource history_meta =
                graph.import(h.meta.at(read), "TAA history ids", before, rhi::ResourceState::shader_read);
            const GraphResource next_color =
                graph.import(h.color.at(write), "TAA history (next)", rhi::ResourceState::undefined,
                             rhi::ResourceState::shader_read);
            const GraphResource next_meta =
                graph.import(h.meta.at(write), "TAA history ids (next)", rhi::ResourceState::undefined,
                             rhi::ResourceState::shader_read);
            const GraphResource resolved =
                graph.create_texture({width, height, rhi::Format::rgba16_float,
                                      rhi::TextureUsage::storage | rhi::TextureUsage::sampled, "TAA resolved"});
            TaaParams taa{.reprojection = reprojection,
                          .size = {width, height},
                          .near_plane = view.near_plane,
                          .exposure_ratio = valid && h.exposure > 0.0f ? view.exposure / h.exposure : 1.0f,
                          .flags = (valid ? taa_history_valid : 0u) | (auto_exposed ? taa_auto_exposure : 0u),
                          .min_weight = taa_min_weight,
                          .moving_weight = taa_moving_weight,
                          .moving_pixels = taa_moving_pixels,
                          .clip_gamma = taa_clip_gamma,
                          .edge_weight = taa_edge_weight,
                          .sharpness = taa_sharpness};
            graph.add_compute_pass(
                "taa",
                [&](PassBuilder& b) {
                    read_meshes(b);
                    b.read(radiance);
                    b.read(motion);
                    b.read(depth);
                    b.read(visibility);
                    b.read(history_color);
                    b.read(history_meta);
                    if (exposure_state) {
                        b.read(*exposure_state);
                    }
                    b.write(next_color, rhi::ResourceState::shader_write);
                    b.write(next_meta, rhi::ResourceState::shader_write);
                    b.write(resolved, rhi::ResourceState::shader_write);
                },
                [taa, pipeline = s.taa_pipeline, sampler = s.linear_clamp, radiance, motion, depth, visibility,
                 instances = scene.instances, history_color, history_meta, next_color, next_meta, resolved,
                 exposure = exposure_state](PassContext& ctx) mutable {
                    const auto storage = [&](GraphResource r) {
                        return ctx.device.descriptor(ctx.texture(r), rhi::View::storage);
                    };
                    taa.radiance = ctx.device.descriptor(ctx.texture(radiance));
                    taa.motion = ctx.device.descriptor(ctx.texture(motion));
                    taa.depth = ctx.device.descriptor(ctx.texture(depth));
                    taa.visibility = ctx.device.descriptor(ctx.texture(visibility));
                    taa.instances = ctx.device.descriptor(ctx.buffer(instances));
                    taa.history = ctx.device.descriptor(ctx.texture(history_color));
                    taa.history_meta = ctx.device.descriptor(ctx.texture(history_meta));
                    taa.linear_clamp = ctx.device.descriptor(sampler);
                    taa.history_out = storage(next_color);
                    taa.meta_out = storage(next_meta);
                    taa.resolved = storage(resolved);
                    if (exposure) {
                        taa.exposure_state = ctx.device.descriptor(ctx.buffer(*exposure));
                    }
                    ctx.cmd.set_pipeline(pipeline);
                    ctx.cmd.set_parameters(taa);
                    ctx.cmd.dispatch((taa.size[0] + 7) / 8, (taa.size[1] + 7) / 8);
                });
            h.last = write;
            h.valid = true;
            h.frame = device_frame;
            h.exposure = view.exposure;
            h.auto_exposure = auto_exposed;
            image = resolved;
            if (taa.sharpness > 0.0f) {
                const GraphResource sharpened =
                    graph.create_texture({width, height, rhi::Format::rgba16_float,
                                          rhi::TextureUsage::storage | rhi::TextureUsage::sampled, "TAA sharpened"});
                graph.add_compute_pass(
                    "taa sharpen",
                    [&](PassBuilder& b) {
                        b.read(resolved);
                        b.read(motion);
                        b.write(sharpened, rhi::ResourceState::shader_write);
                    },
                    [taa, pipeline = s.taa_sharpen_pipeline, resolved, motion, sharpened](PassContext& ctx) mutable {
                        taa.radiance = ctx.device.descriptor(ctx.texture(resolved));
                        taa.motion = ctx.device.descriptor(ctx.texture(motion));
                        taa.resolved = ctx.device.descriptor(ctx.texture(sharpened), rhi::View::storage);
                        ctx.cmd.set_pipeline(pipeline);
                        ctx.cmd.set_parameters(taa);
                        ctx.cmd.dispatch((taa.size[0] + 7) / 8, (taa.size[1] + 7) / 8);
                    });
                image = sharpened;
            }
        }
    } else {
        s.history.valid = false;
    }
    if (frame.blackboard != nullptr) {
        frame.blackboard->set("hdr_color", image);
    }

    // ---- the space backdrop (ADR 0091): beyond the sky, on the share of each pixel the shade pass marked (smoothed
    // by TAA), along unjittered rays, before metering.
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
                b.write(image, rhi::ResourceState::shader_write);
            },
            [composite, pipeline = s.backdrop_pipeline, image, sky = scene.sky,
             exposure = exposure_state](PassContext& ctx) mutable {
                if (exposure) {
                    composite.exposure_state = ctx.device.descriptor(ctx.buffer(*exposure));
                }
                composite.radiance = ctx.device.descriptor(ctx.texture(image), rhi::View::storage);
                composite.sky = ctx.device.descriptor(sky);
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(composite);
                ctx.cmd.dispatch((composite.size[0] + 7) / 8, (composite.size[1] + 7) / 8);
            });
    }

    // ---- bloom (docs/specs/bloom/spec.md): the glow of the image as shown, the backdrop included, before the display
    // transform blends it in. Metering below reads the image without it.
    const std::optional<BloomResult> bloom = s.bloom.record(graph, image, view, width, height);

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
        const GraphResource histogram_bins = *exposure_histogram;
        graph.add_compute_pass(
            "exposure histogram",
            [&](PassBuilder& b) {
                b.read(image);
                b.write(state, rhi::ResourceState::shader_write);  // read only; declared storage for its view
                b.write(histogram_bins, rhi::ResourceState::shader_write);
            },
            [exposure, pipeline = s.histogram_pipeline, image, state, histogram_bins](PassContext& ctx) mutable {
                exposure.radiance = ctx.device.descriptor(ctx.texture(image));
                exposure.histogram = ctx.device.descriptor(ctx.buffer(histogram_bins), rhi::View::storage);
                exposure.state = ctx.device.descriptor(ctx.buffer(state), rhi::View::storage);
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(exposure);
                ctx.cmd.dispatch((exposure.size[0] + 15) / 16, (exposure.size[1] + 15) / 16);
            });
        graph.add_compute_pass(
            "exposure resolve",
            [&](PassBuilder& b) {
                b.write(histogram_bins, rhi::ResourceState::shader_write);
                b.write(state, rhi::ResourceState::shader_write);
            },
            [exposure, pipeline = s.resolve_pipeline, state, histogram_bins](PassContext& ctx) mutable {
                exposure.histogram = ctx.device.descriptor(ctx.buffer(histogram_bins), rhi::View::storage);
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
    display.tonemap = (exposure_state ? 3u : 1u) | (bloom ? display_bloom : 0u);
    if (bloom) {
        display.bloom_texel = {1.0f / static_cast<float>((width + 1) / 2), 1.0f / static_cast<float>((height + 1) / 2)};
        display.bloom_intensity = std::min(view.bloom.intensity, 1.0f) * bloom->coverage;
    }
    graph.add_compute_pass(
        "display",
        [&](PassBuilder& b) {
            b.read(image);
            if (exposure_state) {
                b.read(*exposure_state);
            }
            if (bloom) {
                b.read(bloom->image);
            }
            b.write(frame.scene_color, rhi::ResourceState::shader_write);
        },
        [display, pipeline = s.display_pipeline, image, color = frame.scene_color, exposure = exposure_state,
         bloom = bloom ? std::optional<GraphResource>(bloom->image) : std::nullopt,
         sampler = s.bloom.sampler()](PassContext& ctx) mutable {
            if (exposure) {
                display.exposure_state = ctx.device.descriptor(ctx.buffer(*exposure));
            }
            if (bloom) {
                display.bloom = ctx.device.descriptor(ctx.texture(*bloom), rhi::View::read, ctx.range(*bloom));
                display.linear_clamp = ctx.device.descriptor(sampler);
            }
            display.radiance = ctx.device.descriptor(ctx.texture(image));
            display.output = ctx.device.descriptor(ctx.texture(color), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(display);
            ctx.cmd.dispatch((display.size[0] + 7) / 8, (display.size[1] + 7) / 8);
        });

    // ---- what culling did, frames_in_flight frames later (CullStats).
    if (frame.readbacks != nullptr) {
        const std::uint32_t instance_count = scene.instance_count;
        frame.readbacks->read(
            graph, counters, counter_count * sizeof(std::uint32_t),
            [&s, instance_count](Result<ReadbackData> data) {
                if (!data || data->bytes.size() < counter_count * sizeof(std::uint32_t)) {
                    return;
                }
                std::array<std::uint32_t, counter_count> c{};
                std::memcpy(c.data(), data->bytes.data(), sizeof(c));
                s.stats.frame = data->frame;
                s.stats.instances = instance_count;
                s.stats.instances_visible = c[counter_instances_visible];
                s.stats.instances_disoccluded = c[counter_instances_visible + 1];
                s.stats.instances_rechecked = c[counter_recheck_instances];
                s.stats.meshlets_culled = c[counter_meshlets_culled];
                s.stats.meshlets_rechecked = c[counter_recheck_meshlets];
                s.stats.meshlets_occluded = c[counter_meshlets_occluded];
            },
            "cull counters readback");
        frame.readbacks->read(
            graph, draw_args, 2 * draw_list_count * sizeof(rhi::DrawIndirectArgs),
            [&s](Result<ReadbackData> data) {
                if (!data || data->bytes.size() < 2 * draw_list_count * sizeof(rhi::DrawIndirectArgs)) {
                    return;
                }
                std::array<rhi::DrawIndirectArgs, 2 * draw_list_count> records{};
                std::memcpy(records.data(), data->bytes.data(), sizeof(records));
                s.stats.meshlets_drawn = {};
                for (std::uint32_t r = 0; r < records.size(); ++r) {
                    s.stats.meshlets_drawn.at(r / draw_list_count) += records.at(r).instance_count;
                }
            },
            "cull draw args readback");
    }

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
