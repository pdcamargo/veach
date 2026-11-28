#include "ambient_occlusion.hpp"

#include <veach/core/profile.hpp>

#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <functional>
#include <string>

namespace veach::render {

namespace {

/// Mirrors AoParams in shaders/ambient_occlusion.slang.
struct alignas(16) AoParams {
    math::vec4 projection{0.0f};  ///< 1 / P00, 1 / P11, the near plane, the radius (m)
    math::vec4 settings{0.0f};    ///< falloff start (fraction of the radius), power, pyramid mips
    rhi::DescriptorHandle source;
    rhi::DescriptorHandle edges;
    rhi::DescriptorHandle target;
    rhi::DescriptorHandle trace;
    std::array<std::uint32_t, 2> size{};    ///< the full-resolution view
    std::array<std::uint32_t, 2> extent{};  ///< this pass's target
};
static_assert(rhi::parameters_match<AoParams>(veach_render_shaders::layout::ambient_occlusion,
                                              {VEACH_PARAM(AoParams, projection), VEACH_PARAM(AoParams, settings),
                                               VEACH_PARAM(AoParams, source), VEACH_PARAM(AoParams, edges),
                                               VEACH_PARAM(AoParams, target), VEACH_PARAM(AoParams, trace),
                                               VEACH_PARAM(AoParams, size), VEACH_PARAM(AoParams, extent)}));

/// Mips of the view-depth pyramid: mip 4 serves samples 2^(4 + 3.3) pixels away, past the radius in pixels of a
/// surface a meter from the camera at 1440p.
constexpr std::uint32_t pyramid_mips = 5;
/// The falloff begins at this fraction of the radius (XeGTAO's 1 - 0.615) and ends at the radius.
constexpr float falloff_start = 0.385f;
/// The power on the visible fraction at intensity 1. GTAO's horizons stop at the radius and see only the depth
/// buffer's front layer; the power restores the contrast a full-sphere estimate gives (XeGTAO's default).
constexpr float default_power = 2.2f;
/// Edge-aware blur passes at half resolution, before the upsample.
constexpr std::uint32_t denoise_passes = 2;

}  // namespace

struct AmbientOcclusionRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle view_depth;
    rhi::PipelineHandle depth_mip;
    rhi::PipelineHandle trace;
    rhi::PipelineHandle denoise;
    rhi::PipelineHandle upsample;

    Impl() = default;
    ~Impl() {
        if (device == nullptr) {
            return;
        }
        for (rhi::PipelineHandle p : {view_depth, depth_mip, trace, denoise, upsample}) {
            if (p.is_valid()) {
                device->destroy(p);
            }
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

AmbientOcclusionRenderer::AmbientOcclusionRenderer() : impl_(std::make_unique<Impl>()) {}

AmbientOcclusionRenderer::~AmbientOcclusionRenderer() = default;

Result<> AmbientOcclusionRenderer::prepare(rhi::Device& device) {
    Impl& s = *impl_;
    s.device = &device;
    for (const auto& [entry, out] : {std::pair{"ao_view_depth", &s.view_depth}, std::pair{"ao_depth_mip", &s.depth_mip},
                                     std::pair{"ao_trace", &s.trace}, std::pair{"ao_denoise", &s.denoise},
                                     std::pair{"ao_upsample", &s.upsample}}) {
        auto pipeline = device.create_compute_pipeline(
            {&veach_render_shaders::ambient_occlusion(), entry, {8, 8, 1}, std::string(entry)});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    return {};
}

std::optional<GraphResource> AmbientOcclusionRenderer::record(RenderGraph& graph, GraphResource depth,
                                                              const RenderView& view, std::uint32_t width,
                                                              std::uint32_t height) {
    VEACH_ZONE("AmbientOcclusionRenderer::record");
    const AmbientOcclusion& settings = view.ambient_occlusion;
    if (!settings.enabled || settings.radius <= 0.0f || settings.intensity <= 0.0f) {
        return std::nullopt;
    }
    const Impl& s = *impl_;
    const std::uint32_t mips =
        std::min(pyramid_mips, static_cast<std::uint32_t>(std::bit_width(std::max(width, height))));
    const std::array<std::uint32_t, 2> full{width, height};
    const std::array<std::uint32_t, 2> half{(width + 1) / 2, (height + 1) / 2};
    const AoParams base{
        .projection =
            math::vec4(1.0f / view.projection[0][0], 1.0f / view.projection[1][1], view.near_plane, settings.radius),
        .settings = math::vec4(falloff_start, default_power * settings.intensity, static_cast<float>(mips), 0.0f),
        .size = full};

    constexpr rhi::TextureUsage rw = rhi::TextureUsage::storage | rhi::TextureUsage::sampled;
    const GraphResource pyramid =
        graph.create_texture({width, height, rhi::Format::r32_float, rw, "ao view depth", mips});
    const GraphResource traced = graph.create_texture({half[0], half[1], rhi::Format::rg16_float, rw, "ao trace"});
    const std::array<GraphResource, 2> blurred{
        graph.create_texture({half[0], half[1], rhi::Format::r16_float, rw, "ao blur a"}),
        graph.create_texture({half[0], half[1], rhi::Format::r16_float, rw, "ao blur b"})};
    const GraphResource occlusion =
        graph.create_texture({width, height, rhi::Format::r16_float, rw, "ambient occlusion"});

    // One compute pass of `pipeline` over `extent`; `bind` fills the handles.
    const auto pass = [&](std::string name, rhi::PipelineHandle pipeline, std::array<std::uint32_t, 2> extent,
                          const std::function<void(PassBuilder&)>& declare,
                          std::function<void(PassContext&, AoParams&)> bind) {
        AoParams p = base;
        p.extent = extent;
        graph.add_compute_pass(std::move(name), declare,
                               [p, pipeline, bind = std::move(bind)](PassContext& ctx) mutable {
                                   bind(ctx, p);
                                   ctx.cmd.set_pipeline(pipeline);
                                   ctx.cmd.set_parameters(p);
                                   ctx.cmd.dispatch((p.extent[0] + 7) / 8, (p.extent[1] + 7) / 8);
                               });
    };

    // ---- the view-depth pyramid.
    for (std::uint32_t level = 0; level < mips; ++level) {
        const GraphResource target = graph.mips(pyramid, level);
        const GraphResource source = level == 0 ? depth : graph.mips(pyramid, level - 1);
        pass(
            level == 0 ? std::string("ao: view depth") : std::format("ao: depth mip {}", level),
            level == 0 ? s.view_depth : s.depth_mip, {rhi::mip_size(width, level), rhi::mip_size(height, level)},
            [&](PassBuilder& b) {
                b.read(source);
                b.write(target);
            },
            [source, target, level](PassContext& ctx, AoParams& p) {
                p.source = level == 0 ? ctx.device.descriptor(ctx.texture(source))
                                      : ctx.device.descriptor(ctx.texture(source), rhi::View::read, ctx.range(source));
                p.target = ctx.device.descriptor(ctx.texture(target), rhi::View::storage, ctx.range(target));
            });
    }

    // ---- the trace, at half resolution.
    pass(
        "ao: trace", s.trace, half,
        [&](PassBuilder& b) {
            b.read(pyramid);
            b.write(traced);
        },
        [pyramid, traced](PassContext& ctx, AoParams& p) {
            p.source = ctx.device.descriptor(ctx.texture(pyramid));
            p.trace = ctx.device.descriptor(ctx.texture(traced), rhi::View::storage);
        });

    // ---- the blur: the trace into the first target, then back and forth.
    GraphResource input = traced;
    for (std::uint32_t i = 0; i < denoise_passes; ++i) {
        const GraphResource output = blurred.at(i % 2);
        pass(
            std::format("ao: denoise {}", i), s.denoise, half,
            [&](PassBuilder& b) {
                b.read(input);
                if (input.id != traced.id) {
                    b.read(traced);
                }
                b.write(output);
            },
            [input, traced, output](PassContext& ctx, AoParams& p) {
                p.source = ctx.device.descriptor(ctx.texture(input));
                p.edges = ctx.device.descriptor(ctx.texture(traced));
                p.target = ctx.device.descriptor(ctx.texture(output), rhi::View::storage);
            });
        input = output;
    }

    // ---- to full resolution, guided by depth.
    pass(
        "ao: upsample", s.upsample, full,
        [&](PassBuilder& b) {
            b.read(pyramid);
            b.read(input);
            b.write(occlusion);
        },
        [pyramid, input, occlusion](PassContext& ctx, AoParams& p) {
            p.source = ctx.device.descriptor(ctx.texture(pyramid));
            p.edges = ctx.device.descriptor(ctx.texture(input));
            p.target = ctx.device.descriptor(ctx.texture(occlusion), rhi::View::storage);
        });
    return occlusion;
}

}  // namespace veach::render
