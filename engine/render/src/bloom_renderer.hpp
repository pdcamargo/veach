#pragma once

// Bloom for the real-time renderer (docs/specs/bloom/spec.md): a 13-tap downsample and tent upsample chain at half
// resolution and below (shaders/bloom.slang), planned by render::bloom_chain. The display pass blends its result into
// the image (display.slang).

#include <veach/core/error.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace veach::render {

/// The chain's result: mip 0 of an rgba16_float chain at half resolution, a mips() view to read through
/// PassContext::range, and the share of the glow it carries (BloomChain::coverage).
struct BloomResult {
    GraphResource image;
    float coverage = 1.0f;
};

class BloomRenderer {
public:
    BloomRenderer();
    ~BloomRenderer();
    BloomRenderer(const BloomRenderer&) = delete;
    BloomRenderer& operator=(const BloomRenderer&) = delete;
    BloomRenderer(BloomRenderer&&) = delete;
    BloomRenderer& operator=(BloomRenderer&&) = delete;

    [[nodiscard]] Result<> prepare(rhi::Device& device);
    /// Records the chain over `image` (pre-exposed rgba16_float, width x height). Nothing when the view turns bloom
    /// off or the image is too small.
    [[nodiscard]] std::optional<BloomResult> record(RenderGraph& graph, GraphResource image, const RenderView& view,
                                                    std::uint32_t width, std::uint32_t height);
    /// The linear clamp sampler the display pass reads the result with.
    [[nodiscard]] rhi::SamplerHandle sampler() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
