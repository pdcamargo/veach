#include "bloom_renderer.hpp"

#include <veach/core/profile.hpp>
#include <veach/render/bloom.hpp>

#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace veach::render {

namespace {

/// Mirrors BloomParams in shaders/bloom.slang.
struct alignas(16) BloomParams {
    rhi::DescriptorHandle source;
    rhi::DescriptorHandle coarse;
    rhi::DescriptorHandle target;
    rhi::DescriptorHandle linear_clamp;
    std::array<float, 2> source_texel{};
    std::array<float, 2> coarse_texel{};
    std::array<std::uint32_t, 2> extent{};
    float blend = 1.0f;
    std::uint32_t flags = 0;
};
static_assert(rhi::parameters_match<BloomParams>(
    veach_render_shaders::layout::bloom,
    {VEACH_PARAM(BloomParams, source), VEACH_PARAM(BloomParams, coarse), VEACH_PARAM(BloomParams, target),
     VEACH_PARAM(BloomParams, linear_clamp), VEACH_PARAM(BloomParams, source_texel),
     VEACH_PARAM(BloomParams, coarse_texel), VEACH_PARAM(BloomParams, extent), VEACH_PARAM(BloomParams, blend),
     VEACH_PARAM(BloomParams, flags)}));

constexpr std::uint32_t bloom_karis = 1u;

/// The finest screen-relative texel that gets the full weight: level 0 at 1080p (2 pixels of 1080). Finer levels (at
/// higher resolutions) fade out over an octave, so they add detail, not light, to the glow.
constexpr float finest_scale = 2.0f / 1080.0f;

}  // namespace

namespace {

/// A level's weight from its texel's size on the screen (a fraction of the image height).
float level_weight(float scale, float radius) {
    const float above = std::clamp(1.0f - std::log2(scale / radius), 0.0f, 1.0f);
    const float below = std::clamp(1.0f + std::log2(scale / finest_scale), 0.0f, 1.0f);
    return above * below;
}

/// The weight a chain with a level at every octave would have in all: the integral of level_weight over log2(scale).
float full_weight(float radius) {
    constexpr int steps = 512;
    const float from = std::log2(finest_scale) - 1.0f;
    const float to = std::log2(radius) + 1.0f;
    if (!(to > from)) {
        return 0.0f;
    }
    const float step = (to - from) / static_cast<float>(steps);
    float sum = 0.0f;
    for (int i = 0; i < steps; ++i) {
        sum += level_weight(std::exp2(from + (static_cast<float>(i) + 0.5f) * step), radius);
    }
    return sum * step;
}

}  // namespace

BloomChain bloom_chain(std::uint32_t width, std::uint32_t height, float radius) {
    BloomChain chain;
    if (width < 4 || height < 4 || !(radius > 0.0f)) {
        return chain;
    }
    const std::uint32_t half_width = (width + 1) / 2;
    const std::uint32_t half_height = (height + 1) / 2;
    std::array<float, BloomChain::max_levels> w{};
    float total = 0.0f;
    for (std::uint32_t k = 0; k < BloomChain::max_levels; ++k) {
        if (std::min(rhi::mip_size(half_width, k), rhi::mip_size(half_height, k)) < 2) {
            break;
        }
        const float scale = std::exp2(static_cast<float>(k + 1)) / static_cast<float>(height);
        if (scale >= 2.0f * radius) {
            break;  // weighs nothing, and neither does any coarser level
        }
        w.at(k) = level_weight(scale, radius);
        chain.scale.at(k) = scale;
        total += w.at(k);
        chain.levels = k + 1;
    }
    if (total <= 0.0f) {
        return BloomChain{};
    }
    float coarser = 0.0f;  ///< the weight of this level and every coarser one
    for (std::uint32_t k = chain.levels; k-- > 0;) {
        chain.weights.at(k) = w.at(k) / total;
        coarser += w.at(k);
        chain.blend.at(k) = coarser > 0.0f ? w.at(k) / coarser : 1.0f;
    }
    chain.blend.at(chain.levels - 1) = 1.0f;
    chain.coverage = std::min(1.0f, total / full_weight(radius));
    return chain;
}

struct BloomRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle downsample;
    rhi::PipelineHandle upsample;
    rhi::SamplerHandle linear_clamp;

    Impl() = default;
    ~Impl() {
        if (device == nullptr) {
            return;
        }
        for (rhi::PipelineHandle p : {downsample, upsample}) {
            if (p.is_valid()) {
                device->destroy(p);
            }
        }
        if (linear_clamp.is_valid()) {
            device->destroy(linear_clamp);
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

BloomRenderer::BloomRenderer() : impl_(std::make_unique<Impl>()) {}

BloomRenderer::~BloomRenderer() = default;

Result<> BloomRenderer::prepare(rhi::Device& device) {
    Impl& s = *impl_;
    s.device = &device;
    for (const auto& [entry, out] :
         {std::pair{"bloom_downsample", &s.downsample}, std::pair{"bloom_upsample", &s.upsample}}) {
        auto pipeline =
            device.create_compute_pipeline({&veach_render_shaders::bloom(), entry, {8, 8, 1}, std::string(entry)});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        *out = *pipeline;
    }
    auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::clamp});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.linear_clamp = *sampler;
    return {};
}

rhi::SamplerHandle BloomRenderer::sampler() const {
    return impl_->linear_clamp;
}

std::optional<BloomResult> BloomRenderer::record(RenderGraph& graph, GraphResource image, const RenderView& view,
                                                 std::uint32_t width, std::uint32_t height) {
    VEACH_ZONE("BloomRenderer::record");
    if (!(view.bloom.intensity > 0.0f)) {
        return std::nullopt;
    }
    const BloomChain chain = bloom_chain(width, height, view.bloom.radius);
    if (chain.levels == 0) {
        return std::nullopt;
    }
    const Impl& s = *impl_;
    const std::uint32_t half_width = (width + 1) / 2;
    const std::uint32_t half_height = (height + 1) / 2;
    const auto size = [&](std::uint32_t level) {
        return std::array<std::uint32_t, 2>{rhi::mip_size(half_width, level), rhi::mip_size(half_height, level)};
    };
    const auto texel = [](std::array<std::uint32_t, 2> extent) {
        return std::array<float, 2>{1.0f / static_cast<float>(extent[0]), 1.0f / static_cast<float>(extent[1])};
    };
    constexpr rhi::TextureUsage rw = rhi::TextureUsage::storage | rhi::TextureUsage::sampled;
    const GraphResource down =
        graph.create_texture({half_width, half_height, rhi::Format::rgba16_float, rw, "bloom down", chain.levels});

    // One compute pass over level `level` of the chain; `source` and `coarse` are read through views of their mips
    // (the image, a whole texture, through its full range).
    const auto pass = [&](std::string name, rhi::PipelineHandle pipeline, std::uint32_t level, GraphResource source,
                          std::optional<GraphResource> coarse, GraphResource target, BloomParams p) {
        p.extent = size(level);
        graph.add_compute_pass(
            std::move(name),
            [&](PassBuilder& b) {
                b.read(source);
                if (coarse) {
                    b.read(*coarse);
                }
                b.write(target);
            },
            [p, pipeline, source, coarse, target, sampler = s.linear_clamp](PassContext& ctx) mutable {
                p.source = ctx.device.descriptor(ctx.texture(source), rhi::View::read, ctx.range(source));
                if (coarse) {
                    p.coarse = ctx.device.descriptor(ctx.texture(*coarse), rhi::View::read, ctx.range(*coarse));
                }
                p.target = ctx.device.descriptor(ctx.texture(target), rhi::View::storage, ctx.range(target));
                p.linear_clamp = ctx.device.descriptor(sampler);
                ctx.cmd.set_pipeline(pipeline);
                ctx.cmd.set_parameters(p);
                ctx.cmd.dispatch((p.extent[0] + 7) / 8, (p.extent[1] + 7) / 8);
            });
    };

    // ---- down: the image into level 0 (Karis's average), then each level into the next.
    for (std::uint32_t level = 0; level < chain.levels; ++level) {
        const GraphResource source = level == 0 ? image : graph.mips(down, level - 1);
        const std::array<std::uint32_t, 2> from =
            level == 0 ? std::array<std::uint32_t, 2>{width, height} : size(level - 1);
        pass(std::format("bloom: down {}", level), s.downsample, level, source, std::nullopt, graph.mips(down, level),
             {.source_texel = texel(from), .flags = level == 0 ? bloom_karis : 0u});
    }
    if (chain.levels == 1) {
        return BloomResult{graph.mips(down, 0), chain.coverage};
    }

    // ---- up: from the coarsest level back to level 0, each a blend of the coarser result and its own level.
    const GraphResource up =
        graph.create_texture({half_width, half_height, rhi::Format::rgba16_float, rw, "bloom up", chain.levels - 1});
    for (std::uint32_t level = chain.levels - 1; level-- > 0;) {
        const GraphResource coarse =
            level + 2 == chain.levels ? graph.mips(down, level + 1) : graph.mips(up, level + 1);
        pass(std::format("bloom: up {}", level), s.upsample, level, graph.mips(down, level), coarse,
             graph.mips(up, level), {.coarse_texel = texel(size(level + 1)), .blend = chain.blend.at(level)});
    }
    return BloomResult{graph.mips(up, 0), chain.coverage};
}

}  // namespace veach::render
