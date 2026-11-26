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
static_assert(rhi::parameters_match<RealtimeParams>(
    veach_render_shaders::layout::candela,
    {VEACH_PARAM(RealtimeParams, frame), VEACH_PARAM(RealtimeParams, argument), VEACH_PARAM(RealtimeParams, pad)}));

/// Mirrors PackedGiReservoir in shaders/veach/realtime.slang: Candela's GI reservoir.
struct PackedGiReservoir {
    std::array<std::uint32_t, 8> words{};
};
static_assert(sizeof(PackedGiReservoir) == 32);

/// Candela's radiance cache (ADR 0051, shaders/veach/radiance_cache.slang): slots, the size of a cell at level 0 and
/// of a cell relative to its distance from the camera, frames a value averages over.
constexpr std::uint32_t cache_capacity = 1u << 21;
constexpr float cache_cell_size = 0.05f;
constexpr float cache_cell_scale = 1.0f / 80.0f;
constexpr std::uint32_t cache_max_frames = 3;    ///< the frame average (Candela.adaptive_cache off)
constexpr std::uint32_t cache_max_samples = 64;  ///< the adaptive history (Candela.adaptive_cache on)
constexpr float cache_anchor_step = 4.0f;        ///< Candela.stable_levels: the camera snapped to this grid, meters
/// The real-time counters: ReSTIR's three, then Candela's (RealtimeFrame::counters).
constexpr std::size_t counter_count = 16;

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
    math::vec4 cache_anchor{0.0f};  ///< xyz: the cache's level anchor; w: 1 = adaptive averaging
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
    /// The denoiser (ADR 0042), in veach/realtime.slang's order: noisy diffuse and specular (read, write each), last
    /// frame's diffuse and specular history, this frame's (write, then read), last frame's moments, this frame's
    /// (write, read), the a-trous ping-pong (diffuse 0, 1, specular 0, 1: read, write each), the denoised image (read,
    /// write).
    std::array<rhi::DescriptorHandle, 23> denoiser;
    /// [0] ReSTIR's shadow rays, [1] lit pixels, [2] directional lights' rays, [3..10] Candela's cache
    /// (veach/radiance_cache.slang's cache_stat_*)
    rhi::DescriptorHandle counters;
    rhi::DescriptorHandle sky;       ///< SkyFrame (ADR 0050)
    rhi::DescriptorHandle indirect;  ///< the GI pass's indirect light on the diffuse lobe (read, then write)
    rhi::DescriptorHandle indirect_out;
    /// Candela (ADR 0051): the radiance cache's keys, sums and values, then the GI reservoirs: temporal (write, read),
    /// final (write), last frame's final (read), and a spare.
    std::array<rhi::DescriptorHandle, 3> cache;
    std::array<rhi::DescriptorHandle, 5> gi_reservoirs;
    std::array<std::uint32_t, 2> render_size{};
    std::uint32_t frame = 0;
    std::uint32_t flags = 0;
    std::uint32_t light_count = 0;
    std::uint32_t emissive_count = 0;
    float emissive_total_weight = 0.0f;
    std::uint32_t scene_flags = 0;
    std::uint32_t gi = 0;  ///< GlobalIllumination
    std::uint32_t ao_rays = 0;
    float ao_radius = 1.0f;
    std::uint32_t debug_skip = 0;
    std::uint32_t view = 0;
    std::uint32_t light_candidates = 0;
    std::uint32_t spatial_samples = 0;
    std::uint32_t temporal_reuse = 0;
    std::uint32_t accumulated = 0;
    std::uint32_t denoise = 0;
    std::uint32_t cache_capacity = 0;
    float cache_scale = 0.0f;
    float cache_cell_size = 0.0f;
    float cache_cell_scale = 0.0f;
    std::uint32_t cache_max_frames = 0;
    std::uint32_t gi_rate = 4;
    std::uint32_t gi_spatial_samples = 0;
    std::uint32_t gi_cache_bounces = 0;
    std::uint32_t gi_reservoirs_valid = 0;
    std::uint32_t gi_gather = 0;  ///< CandelaGather
};
static_assert(offsetof(RealtimeFrame, tlas) == 320);
static_assert(offsetof(RealtimeFrame, surfaces) == 368);
static_assert(offsetof(RealtimeFrame, previous) == 480);
static_assert(offsetof(RealtimeFrame, alias) == 536);
static_assert(offsetof(RealtimeFrame, denoiser) == 600);
static_assert(offsetof(RealtimeFrame, render_size) == 880);
static_assert(sizeof(RealtimeFrame) == 992);

/// Mirrors PackedReservoir in shaders/veach/realtime.slang (ADR 0041).
struct PackedReservoir {
    math::vec4 position_light{0.0f};
    math::vec4 normal_w{0.0f};
    math::vec4 radiance_m{0.0f};
};
static_assert(sizeof(PackedReservoir) == 48);

constexpr std::uint32_t frame_history_valid = 1;  ///< RealtimeFrame::flags, as in veach/realtime.slang
constexpr std::uint32_t frame_reservoirs_valid = 2;
constexpr std::uint32_t frame_denoiser_history_valid = 4;
constexpr std::uint32_t atrous_iterations = 5;  ///< steps 1, 2, 4, 8, 16 (ADR 0042)
constexpr std::uint32_t jitter_phases = 16;     ///< Halton(2,3) phases (ADR 0040)

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
        Persistent depth;                                   ///< MetalFX only: it reads the visibility pass's depth
        rhi::TemporalScalerId scaler;                       ///< MetalFX's temporal scaler, for these sizes
        std::array<std::array<Persistent, 7>, 2> surfaces;  ///< ping-pong: this frame's and last frame's
        Persistent radiance;
        std::array<Persistent, 2> upscaled;                ///< output resolution, ping-pong (TAAU history)
        Persistent accumulation;                           ///< RealtimeView::accumulated
        std::uint32_t accumulated = 0;                     ///< frames in it
        PersistentBuffer temporal_reservoirs;              ///< ReSTIR: initial + temporal, this frame
        std::array<PersistentBuffer, 2> final_reservoirs;  ///< ReSTIR: after spatial reuse, ping-pong
        // The denoiser (ADR 0042): noisy lobes, histories and moments (ping-pong), the a-trous ping-pong, the result.
        Persistent noisy_diffuse;
        Persistent noisy_specular;
        std::array<Persistent, 2> diffuse_history;
        std::array<Persistent, 2> specular_history;
        std::array<Persistent, 2> moments;
        std::array<Persistent, 2> atrous_diffuse;
        std::array<Persistent, 2> atrous_specular;
        Persistent denoised;
        Persistent indirect;  ///< the GI pass's output (RealtimeFrame::indirect)
        // Candela's GI reservoirs (ADR 0051), made on first use: gather + temporal, then final (ping-pong).
        PersistentBuffer gi_temporal;
        std::array<PersistentBuffer, 2> gi_final;
        bool gi_valid = false;             ///< last frame's final GI reservoirs belong to this view
        bool denoised_last_frame = false;  ///< the denoiser's history is this view's
        std::uint64_t lights_hash = 0;     ///< the lights last frame's reservoirs sampled
        bool history = false;              ///< last frame's targets belong to this view
        std::uint32_t frame = 0;           ///< frames since the targets were made; its parity picks the set
        math::mat4 previous_view_projection{1.0f};
        math::mat4 previous_view{1.0f};
        math::vec3 previous_position{0.0f};
    };

    rhi::Device* device = nullptr;
    rhi::PipelineHandle visibility_pipeline;
    rhi::PipelineHandle surface_pipeline;
    rhi::PipelineHandle shade_pipeline;
    rhi::PipelineHandle gi_ambient_pipeline;
    rhi::PipelineHandle cache_update_pipeline;
    rhi::PipelineHandle cache_resolve_pipeline;
    rhi::PipelineHandle gather_pipeline;
    rhi::PipelineHandle gi_temporal_pipeline;
    rhi::PipelineHandle gi_misses_pipeline;
    rhi::PipelineHandle gi_surface_pipeline;
    rhi::PipelineHandle gi_spatial_pipeline;
    /// Candela's radiance cache: world space, shared by every view, made on first use.
    std::array<PersistentBuffer, 3> cache;  ///< keys, sums, values
    rhi::PipelineHandle restir_temporal_pipeline;
    rhi::PipelineHandle restir_spatial_pipeline;
    rhi::PipelineHandle accumulate_pipeline;
    rhi::PipelineHandle denoise_temporal_pipeline;
    rhi::PipelineHandle denoise_variance_pipeline;
    rhi::PipelineHandle denoise_atrous_pipeline;
    rhi::PipelineHandle denoise_combine_pipeline;
    rhi::PipelineHandle taau_pipeline;
    rhi::PipelineHandle display_pipeline;
    rhi::SamplerHandle linear_clamp;
    std::unordered_map<std::uint64_t, ViewState> views;
    std::uint32_t frame = 0;  ///< seeds the random numbers
    std::vector<rhi::BufferHandle> frame_buffers;
    bool warned_metalfx = false;
    /// temporal.AC5: shadow rays and lit pixels, counted on the GPU, copied each frame into that frame slot's
    /// readback buffer; the totals are logged at shutdown.
    rhi::BufferHandle counters;
    std::vector<rhi::BufferHandle> counter_readbacks;
    std::size_t counted_slot = 0;
    bool counted = false;
    std::uint64_t candela_frames = 0;  ///< frames Candela ran, for its counters' per-frame figures

    ~Impl() {
        if (device == nullptr) {
            return;
        }
        device->wait_idle();
        release_frame_buffers();
        if (counted) {
            if (const auto* totals = static_cast<const std::uint32_t*>(device->map(counter_readbacks[counted_slot]));
                totals != nullptr && totals[1] > 0) {
                log::info("render",
                          "real-time direct light: {:.3f} shadow rays per lit pixel for ReSTIR's sample, {:.3f} for "
                          "directional lights ({} pixels)",
                          static_cast<double>(totals[0]) / totals[1], static_cast<double>(totals[2]) / totals[1],
                          totals[1]);
            }
            if (const auto* totals = static_cast<const std::uint32_t*>(device->map(counter_readbacks[counted_slot]));
                totals != nullptr && candela_frames > 0 && totals[3] > 0) {
                const double frames = static_cast<double>(candela_frames);
                const double lookups = std::max(1.0, static_cast<double>(totals[8]) + totals[9] + totals[10]);
                log::info("render",
                          "candela cache: per frame {:.0f} estimates added, {:.0f} new cells, {:.0f} dropped (slots "
                          "taken), {:.0f} evictions, {:.0f} cells held; gather lookups {:.1f}% own level, {:.1f}% "
                          "parent, {:.1f}% shaded ({} frames)",
                          totals[3] / frames, totals[4] / frames, totals[5] / frames, totals[6] / frames,
                          totals[7] / frames, 100.0 * totals[8] / lookups, 100.0 * totals[9] / lookups,
                          100.0 * totals[10] / lookups, candela_frames);
            }
        }
        for (PersistentBuffer& b : cache) {
            if (b.handle.is_valid()) {
                device->destroy(b.handle);
            }
        }
        for (rhi::BufferHandle* b : {&counters}) {
            if (b->is_valid()) {
                device->destroy(*b);
            }
        }
        for (rhi::BufferHandle& b : counter_readbacks) {
            device->destroy(b);
        }
        for (auto& [id, v] : views) {
            destroy(v);
        }
        for (rhi::PipelineHandle* p : {&visibility_pipeline,
                                       &surface_pipeline,
                                       &gi_ambient_pipeline,
                                       &cache_update_pipeline,
                                       &cache_resolve_pipeline,
                                       &gather_pipeline,
                                       &gi_misses_pipeline,
                                       &gi_surface_pipeline,
                                       &gi_temporal_pipeline,
                                       &gi_spatial_pipeline,
                                       &shade_pipeline,
                                       &restir_temporal_pipeline,
                                       &restir_spatial_pipeline,
                                       &accumulate_pipeline,
                                       &denoise_temporal_pipeline,
                                       &denoise_variance_pipeline,
                                       &denoise_atrous_pipeline,
                                       &denoise_combine_pipeline,
                                       &taau_pipeline,
                                       &display_pipeline}) {
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
        destroy(v.depth);
        if (v.scaler.is_valid()) {
            device->destroy_temporal_scaler(v.scaler);
            v.scaler = {};
        }
        for (auto& set : v.surfaces) {
            for (Persistent& p : set) {
                destroy(p);
            }
        }
        destroy(v.radiance);
        destroy(v.denoised);
        destroy(v.indirect);
        for (Persistent& p : v.upscaled) {
            destroy(p);
        }
        destroy(v.accumulation);
        for (Persistent* p : denoiser_targets(v)) {
            destroy(*p);
        }
        destroy(v.temporal_reservoirs);
        for (PersistentBuffer& b : v.final_reservoirs) {
            destroy(b);
        }
        destroy(v.gi_temporal);
        for (PersistentBuffer& b : v.gi_final) {
            destroy(b);
        }
    }
    static std::array<Persistent*, 12> denoiser_targets(ViewState& v) {
        return {&v.noisy_diffuse,       &v.noisy_specular,      &v.diffuse_history[0], &v.diffuse_history[1],
                &v.specular_history[0], &v.specular_history[1], &v.moments[0],         &v.moments[1],
                &v.atrous_diffuse[0],   &v.atrous_diffuse[1],   &v.atrous_specular[0], &v.atrous_specular[1]};
    }
    void destroy(PersistentBuffer& b) {
        if (b.handle.is_valid()) {
            device->destroy(b.handle);
        }
        b = {};
    }
    bool create(PersistentBuffer& b, std::uint64_t size, const char* name,
                std::uint32_t stride = sizeof(PackedReservoir), std::span<const std::byte> initial = {}) {
        auto buffer = device->create_buffer(
            {.size = size, .usage = rhi::BufferUsage::storage, .debug_name = name, .stride = stride}, initial);
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
        for (Persistent* p : denoiser_targets(v)) {
            ok = ok && create(*p, {rw, rh, rhi::Format::rgba16_float, rw_usage, "denoiser"});
        }
        ok = ok && create(v.denoised, {rw, rh, rhi::Format::rgba16_float, rw_usage, "denoised"});
        ok = ok && create(v.indirect, {rw, rh, rhi::Format::rgba16_float, rw_usage, "indirect light"});
        const std::uint64_t reservoirs = std::uint64_t{rw} * rh * sizeof(PackedReservoir);
        ok = ok && create(v.temporal_reservoirs, reservoirs, "reservoirs (temporal)");
        for (PersistentBuffer& b : v.final_reservoirs) {
            ok = ok && create(b, reservoirs, "reservoirs");
        }
        if (mode != UpscalerMode::none) {
            // MetalFX writes its output as a render target; TAAU as storage.
            const auto usage = mode == UpscalerMode::metalfx ? rw_usage | rhi::TextureUsage::color_target : rw_usage;
            for (Persistent& p : v.upscaled) {
                ok = ok && create(p, {ow, oh, rhi::Format::rgba16_float, usage, "upscaled"});
            }
        }
        if (ok && mode == UpscalerMode::metalfx) {
            ok = create(v.depth, {rw, rh, rhi::Format::depth32_float,
                                  rhi::TextureUsage::depth_target | rhi::TextureUsage::sampled, "visibility depth"});
            auto scaler = device->create_temporal_scaler(
                {.input_width = rw, .input_height = rh, .output_width = ow, .output_height = oh});
            if (!scaler) {
                log::error("render", scaler.error());
                return false;
            }
            v.scaler = *scaler;
        }
        return ok;
    }

    /// Candela's cache (once) and this view's GI reservoirs. False if the GPU refused one.
    bool ensure_candela(ViewState& v) {
        bool ok = true;
        if (!cache[0].handle.is_valid()) {
            const std::vector<std::byte> zeros(std::size_t{cache_capacity} * 16);
            const std::span<const std::byte> all(zeros);
            ok = create(cache[0], cache_capacity * 4ull, "candela cache keys", 4, all.first(cache_capacity * 4ull)) &&
                 create(cache[1], cache_capacity * 16ull, "candela cache sums", 4, all) &&
                 create(cache[2], cache_capacity * 16ull, "candela cache values", 16, all);
        }
        if (ok && !v.gi_temporal.handle.is_valid()) {
            const std::uint64_t size = std::uint64_t{v.render_width} * v.render_height * sizeof(PackedGiReservoir);
            ok = create(v.gi_temporal, size, "candela reservoirs (temporal)", sizeof(PackedGiReservoir)) &&
                 create(v.gi_final[0], size, "candela reservoirs", sizeof(PackedGiReservoir)) &&
                 create(v.gi_final[1], size, "candela reservoirs", sizeof(PackedGiReservoir));
            v.gi_valid = false;
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
        const std::array<std::uint32_t, 3> group = std::string_view(entry) == "cache_resolve"
                                                       ? std::array<std::uint32_t, 3>{64, 1, 1}
                                                       : std::array<std::uint32_t, 3>{8, 8, 1};
        auto pipeline = device.create_compute_pipeline({&library, entry, group, entry});
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
          std::tuple{&shaders.shade, "gi_ambient", &s.gi_ambient_pipeline},
          std::tuple{&shaders.candela, "cache_update", &s.cache_update_pipeline},
          std::tuple{&shaders.candela, "cache_resolve", &s.cache_resolve_pipeline},
          std::tuple{&shaders.candela, "gather", &s.gather_pipeline},
          std::tuple{&shaders.candela, "misses", &s.gi_misses_pipeline},
          std::tuple{&shaders.candela, "surface", &s.gi_surface_pipeline},
          std::tuple{&shaders.candela, "temporal", &s.gi_temporal_pipeline},
          std::tuple{&shaders.candela, "spatial", &s.gi_spatial_pipeline},
          std::tuple{&shaders.shade, "shade", &s.shade_pipeline},
          std::tuple{&shaders.shade, "accumulate", &s.accumulate_pipeline},
          std::tuple{&shaders.shade, "denoise_temporal", &s.denoise_temporal_pipeline},
          std::tuple{&shaders.shade, "denoise_variance", &s.denoise_variance_pipeline},
          std::tuple{&shaders.shade, "denoise_atrous", &s.denoise_atrous_pipeline},
          std::tuple{&shaders.shade, "denoise_combine", &s.denoise_combine_pipeline},
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
    const std::array<std::uint32_t, counter_count> zeros{};
    auto counters = device.create_buffer({.size = sizeof(zeros),
                                          .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src,
                                          .debug_name = "real-time counters",
                                          .stride = sizeof(std::uint32_t)},
                                         std::as_bytes(std::span(zeros)));
    if (!counters) {
        return std::unexpected(counters.error());
    }
    s.counters = *counters;
    for (std::uint32_t i = 0; i < device.frames_in_flight(); ++i) {
        auto readback = device.create_buffer({.size = sizeof(zeros),
                                              .usage = rhi::BufferUsage::copy_dst,
                                              .memory = rhi::MemoryType::readback,
                                              .debug_name = "real-time counters readback"});
        if (!readback) {
            return std::unexpected(readback.error());
        }
        s.counter_readbacks.push_back(*readback);
    }
    return {};
}

void RealtimeRenderer::record(RenderGraph& graph, const FrameContext& frame, const RenderView& view,
                              const RealtimeScene& scene) {
    VEACH_ZONE("RealtimeRenderer::record");
    Impl& s = *impl_;
    s.release_frame_buffers();  // last frame's: their submission is made, so destroy() waits for the GPU
    const RealtimeLighting& lighting = view.realtime;

    // Resolution and upscaler (ADR 0040, ADR 0043): automatic is MetalFX where the device has it, else TAAU.
    const bool metalfx_available = s.device->capabilities().temporal_upscaler;
    UpscalerMode mode = view.upscaling.mode;
    if (mode == UpscalerMode::automatic) {
        mode = metalfx_available ? UpscalerMode::metalfx : UpscalerMode::temporal;
    } else if (mode == UpscalerMode::metalfx && !metalfx_available) {
        if (!s.warned_metalfx) {
            log::warn("render",
                      "this device has no MetalFX temporal scaler; using Veach's own (Upscaling.mode = "
                      "\"temporal\")");
            s.warned_metalfx = true;
        }
        mode = UpscalerMode::temporal;
    }
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
    const auto rw_pair = [&](Persistent& p, std::size_t at) {
        f.denoiser.at(at) = device.descriptor(p.handle);
        f.denoiser.at(at + 1) = device.descriptor(p.handle, rhi::View::storage);
    };
    rw_pair(v.noisy_diffuse, 0);
    rw_pair(v.noisy_specular, 2);
    f.denoiser[4] = device.descriptor(v.diffuse_history[previous].handle);
    f.denoiser[5] = device.descriptor(v.specular_history[previous].handle);
    f.denoiser[6] = device.descriptor(v.diffuse_history[current].handle, rhi::View::storage);
    f.denoiser[7] = device.descriptor(v.specular_history[current].handle, rhi::View::storage);
    f.denoiser[8] = device.descriptor(v.diffuse_history[current].handle);
    f.denoiser[9] = device.descriptor(v.specular_history[current].handle);
    f.denoiser[10] = device.descriptor(v.moments[previous].handle);
    f.denoiser[11] = device.descriptor(v.moments[current].handle, rhi::View::storage);
    f.denoiser[12] = device.descriptor(v.moments[current].handle);
    rw_pair(v.atrous_diffuse[0], 13);
    rw_pair(v.atrous_diffuse[1], 15);
    rw_pair(v.atrous_specular[0], 17);
    rw_pair(v.atrous_specular[1], 19);
    rw_pair(v.denoised, 21);
    f.counters = device.descriptor(s.counters, rhi::View::storage);
    f.sky = device.descriptor(scene.sky);
    f.indirect = device.descriptor(v.indirect.handle);
    f.indirect_out = device.descriptor(v.indirect.handle, rhi::View::storage);
    f.gi = static_cast<std::uint32_t>(lighting.gi);
    // Candela (ADR 0051): its cache and this view's GI reservoirs, made on first use.
    const bool candela = lighting.gi == GlobalIllumination::candela;
    if (candela && !s.ensure_candela(v)) {
        return;
    }
    for (std::size_t i = 0; i < f.cache.size(); ++i) {
        f.cache.at(i) = candela ? device.descriptor(s.cache.at(i).handle, rhi::View::storage)
                                : device.null_descriptor(rhi::DescriptorKind::storage_buffer);
    }
    f.gi_reservoirs.fill(device.null_descriptor(rhi::DescriptorKind::buffer));
    if (candela) {
        f.gi_reservoirs[0] = device.descriptor(v.gi_temporal.handle, rhi::View::storage);
        f.gi_reservoirs[1] = device.descriptor(v.gi_temporal.handle);
        f.gi_reservoirs[2] = device.descriptor(v.gi_final[current].handle, rhi::View::storage);
        f.gi_reservoirs[3] = device.descriptor(v.gi_final[previous].handle);
    }
    const Candela& gi_settings = view.candela;
    f.cache_capacity = cache_capacity;
    f.cache_scale = 65536.0f * view.exposure;  // fixed point relative to what the camera shows
    f.cache_cell_size = cache_cell_size;
    f.cache_cell_scale = cache_cell_scale;
    f.cache_max_frames = gi_settings.adaptive_cache ? cache_max_samples : cache_max_frames;
    // Cell sizes grow with distance from this point: the camera itself, or with stable_levels the camera snapped to a
    // coarse grid, so moving does not change which level (and cell) a surface reads until it crosses a grid line.
    math::vec3 anchor = view.position;
    if (gi_settings.stable_levels) {
        anchor = math::floor(view.position / cache_anchor_step + 0.5f) * cache_anchor_step;
    }
    f.cache_anchor = math::vec4(anchor, gi_settings.adaptive_cache ? 1.0f : 0.0f);
    f.gi_rate = gi_settings.ray_rate >= 1.0f ? 1u : (gi_settings.ray_rate >= 0.5f ? 2u : 4u);
    f.gi_spatial_samples = std::min<std::uint32_t>(gi_settings.spatial_samples, 4);
    f.gi_cache_bounces = std::min<std::uint32_t>(gi_settings.cache_bounces, 3);
    f.gi_reservoirs_valid = candela && v.gi_valid && v.history ? 1u : 0u;
    f.gi_gather = static_cast<std::uint32_t>(gi_settings.gather);
    v.gi_valid = candela;
    f.render_size = {rw, rh};
    f.frame = s.frame++;
    // The denoiser runs for the final image; its history is valid only if it ran last frame too (ADR 0042).
    const bool denoise = lighting.denoise && lighting.view == RealtimeView::final;
    f.flags = (v.history ? frame_history_valid : 0u) | (reservoirs_valid ? frame_reservoirs_valid : 0u) |
              (v.history && v.denoised_last_frame ? frame_denoiser_history_valid : 0u);
    v.denoised_last_frame = denoise;
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
    f.denoise = denoise ? 1u : 0u;

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
    // Depth lives only in the visibility pass, unless MetalFX reads it afterwards.
    const GraphResource depth = mode == UpscalerMode::metalfx
                                    ? s.import(graph, v.depth, "visibility depth")
                                    : graph.create_texture({rw, rh, rhi::Format::depth32_float,
                                                            rhi::TextureUsage::depth_target, "visibility depth"});
    v.depth.written = v.depth.handle.is_valid();
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
    const GraphResource counters = graph.import(
        s.counters, "real-time counters", s.counted ? rhi::ResourceState::copy_src : rhi::ResourceState::undefined);
    const GraphResource noisy_diffuse = s.import(graph, v.noisy_diffuse, "noisy diffuse");
    const GraphResource noisy_specular = s.import(graph, v.noisy_specular, "noisy specular");
    v.noisy_diffuse.written = true;
    v.noisy_specular.written = true;
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

    const auto dispatch_with = [&](std::string name, rhi::PipelineHandle pipeline, std::uint32_t argument,
                                   const auto& declare) {
        RealtimeParams pass_params = params;
        pass_params.argument = argument;
        graph.add_compute_pass(
            std::move(name),
            [&](PassBuilder& b) {
                b.read(frame_data);
                declare(b);
            },
            [pass_params, pipeline, rw, rh](PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(pass_params);
                ctx.cmd.dispatch((rw + 7) / 8, (rh + 7) / 8);
            });
    };
    const auto dispatch = [&](const char* name, rhi::PipelineHandle pipeline, const auto& declare) {
        dispatch_with(name, pipeline, 0, declare);
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
    // ---- indirect light: Candela (ADR 0051), or GI off: the ambient stopgap (VCH-017).
    const bool gi = lighting.gi != GlobalIllumination::direct_only;
    const GraphResource indirect = s.import(graph, v.indirect, "indirect light");
    if (candela) {
        std::array<GraphResource, 3> cache{};
        constexpr std::array<const char*, 3> cache_names{"candela cache keys", "candela cache sums",
                                                         "candela cache values"};
        for (std::size_t i = 0; i < cache.size(); ++i) {
            cache.at(i) = s.import(graph, s.cache.at(i), cache_names.at(i));
            s.cache.at(i).written = true;
        }
        const GraphResource gi_temporal = s.import(graph, v.gi_temporal, "candela reservoirs (temporal)");
        const GraphResource gi_final = s.import(graph, v.gi_final[current], "candela reservoirs");
        const GraphResource gi_previous = s.import(graph, v.gi_final[previous], "candela reservoirs (last frame)");
        v.gi_temporal.written = true;
        v.gi_final[current].written = true;
        // The cache is read and written through storage views everywhere.
        const auto use_cache = [&](PassBuilder& b) {
            for (const GraphResource& r : cache) {
                b.write(r, rhi::ResourceState::shader_write);
            }
            b.write(counters, rhi::ResourceState::shader_write);
        };
        ++s.candela_frames;
        const auto groups = [&](std::string name, rhi::PipelineHandle pipeline, std::uint32_t x, std::uint32_t y,
                                const auto& declare) {
            graph.add_compute_pass(
                std::move(name),
                [&](PassBuilder& b) {
                    b.read(frame_data);
                    declare(b);
                },
                [params, pipeline, x, y](PassContext& ctx) {
                    ctx.cmd.set_pipeline(pipeline);
                    ctx.cmd.set_parameters(params);
                    ctx.cmd.dispatch(x, y);
                });
        };
        constexpr std::uint32_t update_block = 5;  // candela.slang's: one path per 5x5 pixels
        const std::uint32_t blocks_x = (rw + update_block - 1) / update_block;
        const std::uint32_t blocks_y = (rh + update_block - 1) / update_block;
        if (!gi_settings.freeze_cache) {
            groups("candela: cache update", s.cache_update_pipeline, (blocks_x + 7) / 8, (blocks_y + 7) / 8,
                   [&](PassBuilder& b) {
                       scene.read_scene(b);
                       for (const GraphResource& r : current_targets) {
                           b.read(r);
                       }
                       use_cache(b);
                   });
            groups("candela: cache resolve", s.cache_resolve_pipeline, cache_capacity / 64, 1, use_cache);
        }
        if (gi_settings.gather == CandelaGather::surface_cache) {
            dispatch("candela: surface", s.gi_surface_pipeline, [&](PassBuilder& b) {
                for (const GraphResource& r : current_targets) {
                    b.read(r);
                }
                use_cache(b);
                b.write(indirect, rhi::ResourceState::shader_write);
            });
        } else {
            dispatch("candela: gather", s.gather_pipeline, [&](PassBuilder& b) {
                scene.read_scene(b);
                for (const GraphResource& r : current_targets) {
                    b.read(r);
                }
                use_cache(b);
                b.write(gi_temporal, rhi::ResourceState::shader_write);
            });
            dispatch("candela: misses", s.gi_misses_pipeline, [&](PassBuilder& b) {
                scene.read_scene(b);
                for (const GraphResource& r : current_targets) {
                    b.read(r);
                }
                use_cache(b);
                b.write(gi_temporal, rhi::ResourceState::shader_write);
            });
            dispatch("candela: temporal", s.gi_temporal_pipeline, [&](PassBuilder& b) {
                scene.read_scene(b);
                for (const GraphResource& r : current_targets) {
                    b.read(r);
                }
                b.read(gi_previous);
                use_cache(b);
                b.write(gi_temporal, rhi::ResourceState::shader_write);
            });
            dispatch("candela: spatial", s.gi_spatial_pipeline, [&](PassBuilder& b) {
                scene.read_scene(b);
                for (const GraphResource& r : current_targets) {
                    b.read(r);
                }
                b.read(gi_temporal);
                use_cache(b);
                b.write(gi_final, rhi::ResourceState::shader_write);
                b.write(indirect, rhi::ResourceState::shader_write);
            });
        }
        v.indirect.written = true;
    } else if (gi) {
        dispatch("gi: ambient", s.gi_ambient_pipeline, [&](PassBuilder& b) {
            scene.read_scene(b);
            for (const GraphResource& r : current_targets) {
                b.read(r);
            }
            b.write(indirect, rhi::ResourceState::shader_write);
        });
        v.indirect.written = true;
    }
    dispatch("shade", s.shade_pipeline, [&](PassBuilder& b) {
        scene.read_scene(b);
        for (const GraphResource& r : current_targets) {
            b.read(r);
        }
        if (gi) {
            b.read(indirect);
        }
        b.read(final_reservoirs);
        b.write(counters, rhi::ResourceState::shader_write);
        b.write(radiance, rhi::ResourceState::shader_write);
        b.write(noisy_diffuse, rhi::ResourceState::shader_write);
        b.write(noisy_specular, rhi::ResourceState::shader_write);
    });
    // The counters into this frame slot's readback (the totals are logged at shutdown).
    s.counted_slot = s.device->current_frame().slot % s.counter_readbacks.size();
    s.counted = true;
    const GraphResource counter_readback =
        graph.import(s.counter_readbacks[s.counted_slot], "real-time counters readback", rhi::ResourceState::undefined);
    graph.add_transfer_pass(
        "real-time counters",
        [&](PassBuilder& b) {
            b.read(counters, rhi::ResourceState::copy_src);
            b.write(counter_readback, rhi::ResourceState::copy_dst);
        },
        [source = s.counters, target = s.counter_readbacks[s.counted_slot]](PassContext& ctx) {
            ctx.cmd.copy_buffer(source, target, counter_count * sizeof(std::uint32_t));
        });
    if (accumulating) {
        dispatch("accumulate", s.accumulate_pipeline, [&](PassBuilder& b) {
            b.read(radiance);
            b.write(accumulation, rhi::ResourceState::shader_write);
        });
        v.accumulation.written = true;
        ++v.accumulated;
    }

    // ---- the denoiser (ADR 0042): temporal accumulation, variance, a-trous, remodulation.
    GraphResource lit = radiance;  // what the upscaler reads
    rhi::TextureHandle lit_handle = v.radiance.handle;
    if (denoise) {
        const GraphResource diffuse_history = s.import(graph, v.diffuse_history[current], "denoiser diffuse history");
        const GraphResource specular_history =
            s.import(graph, v.specular_history[current], "denoiser specular history");
        const GraphResource moments = s.import(graph, v.moments[current], "denoiser moments");
        const GraphResource previous_diffuse = s.import(graph, v.diffuse_history[previous], "denoiser diffuse history");
        const GraphResource previous_specular =
            s.import(graph, v.specular_history[previous], "denoiser specular history");
        const GraphResource previous_moments = s.import(graph, v.moments[previous], "denoiser moments");
        const std::array<GraphResource, 2> atrous_diffuse{s.import(graph, v.atrous_diffuse[0], "a-trous diffuse"),
                                                          s.import(graph, v.atrous_diffuse[1], "a-trous diffuse")};
        const std::array<GraphResource, 2> atrous_specular{s.import(graph, v.atrous_specular[0], "a-trous specular"),
                                                           s.import(graph, v.atrous_specular[1], "a-trous specular")};
        const GraphResource denoised = s.import(graph, v.denoised, "denoised");
        for (Persistent* p :
             {&v.diffuse_history[current], &v.specular_history[current], &v.moments[current], &v.atrous_diffuse[0],
              &v.atrous_diffuse[1], &v.atrous_specular[0], &v.atrous_specular[1], &v.denoised}) {
            p->written = true;
        }
        dispatch("denoise: temporal", s.denoise_temporal_pipeline, [&](PassBuilder& b) {
            b.read(noisy_diffuse);
            b.read(noisy_specular);
            b.read(current_targets[0]);
            b.read(current_targets[1]);
            b.read(current_targets[6]);
            b.read(previous_diffuse);
            b.read(previous_specular);
            b.read(previous_moments);
            b.write(diffuse_history, rhi::ResourceState::shader_write);
            b.write(specular_history, rhi::ResourceState::shader_write);
            b.write(moments, rhi::ResourceState::shader_write);
        });
        dispatch("denoise: variance", s.denoise_variance_pipeline, [&](PassBuilder& b) {
            b.read(current_targets[0]);
            b.read(current_targets[1]);
            b.read(diffuse_history);
            b.read(specular_history);
            b.read(moments);
            b.write(atrous_diffuse[0], rhi::ResourceState::shader_write);
            b.write(atrous_specular[0], rhi::ResourceState::shader_write);
        });
        for (std::uint32_t i = 0; i < atrous_iterations; ++i) {
            dispatch_with(std::format("denoise: a-trous {}", 1u << i), s.denoise_atrous_pipeline, i,
                          [&](PassBuilder& b) {
                              b.read(current_targets[0]);
                              b.read(current_targets[1]);
                              b.read(atrous_diffuse[i % 2]);
                              b.read(atrous_specular[i % 2]);
                              b.write(atrous_diffuse[(i + 1) % 2], rhi::ResourceState::shader_write);
                              b.write(atrous_specular[(i + 1) % 2], rhi::ResourceState::shader_write);
                          });
        }
        constexpr std::uint32_t filtered = atrous_iterations % 2;  // the ping-pong buffer the last iteration wrote
        dispatch_with("denoise: combine", s.denoise_combine_pipeline, filtered, [&](PassBuilder& b) {
            scene.read_sky(b);
            for (const GraphResource& r : current_targets) {
                b.read(r);
            }
            b.read(radiance);
            b.read(atrous_diffuse[filtered]);
            b.read(atrous_specular[filtered]);
            b.write(denoised, rhi::ResourceState::shader_write);
        });
        lit = denoised;
        lit_handle = v.denoised.handle;
    }

    // ---- upscaling (ADR 0043), unless a debug view shows render pixels as they are.
    const bool image = lighting.view == RealtimeView::final || lighting.view == RealtimeView::noisy;
    GraphResource shown = accumulating ? accumulation : lit;
    std::array<std::uint32_t, 2> shown_size{rw, rh};
    if (mode == UpscalerMode::metalfx && image) {
        // MetalFX keeps its own history; one output texture is enough. Its jitter is ours (the sample's offset from
        // the pixel center, +y down): against a native-resolution TAA image of FlightHelmet that measured mean FLIP
        // 0.0131, the opposite sign 0.0138.
        Persistent& out = v.upscaled[0];
        const GraphResource out_resource = s.import(graph, out, "upscaled");
        out.written = true;
        const rhi::CommandList::TemporalScale metalfx_frame{.color = lit_handle,
                                                            .depth = v.depth.handle,
                                                            .motion = surfaces[6].handle,
                                                            .output = out.handle,
                                                            .jitter_x = jitter.x,
                                                            .jitter_y = jitter.y,
                                                            .reset = !v.history,
                                                            .depth_reversed = true};
        graph.add_transfer_pass(
            "metalfx",
            [&](PassBuilder& b) {
                b.read(lit);
                b.read(depth);
                b.read(current_targets[6]);
                b.write(out_resource, rhi::ResourceState::shader_write);
            },
            [metalfx_frame, scaler = v.scaler](PassContext& ctx) { ctx.cmd.temporal_scale(scaler, metalfx_frame); });
        shown = out_resource;
        shown_size = {ow, oh};
    } else if (mode == UpscalerMode::temporal && image) {
        Persistent& out = v.upscaled[current];
        Persistent& history = v.upscaled[previous];
        const GraphResource out_resource = s.import(graph, out, "upscaled");
        const GraphResource history_resource = s.import(graph, history, "upscaled history");
        out.written = true;
        TaauParams taau;
        taau.color = device.descriptor(lit_handle);
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
                b.read(lit);
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
