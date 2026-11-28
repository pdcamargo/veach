#pragma once

// Screen-space ambient occlusion for the real-time renderer (ADR 0096, docs/specs/ambient-light/spec.md): GTAO from
// the visibility pass's depth at full resolution, then an edge-aware blur (shaders/ambient_occlusion.slang). The
// shade pass multiplies the sky's ambient light by it (veach/ambient.slang); direct light never sees it.

#include <veach/core/error.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace veach::render {

class AmbientOcclusionRenderer {
public:
    AmbientOcclusionRenderer();
    ~AmbientOcclusionRenderer();
    AmbientOcclusionRenderer(const AmbientOcclusionRenderer&) = delete;
    AmbientOcclusionRenderer& operator=(const AmbientOcclusionRenderer&) = delete;
    AmbientOcclusionRenderer(AmbientOcclusionRenderer&&) = delete;
    AmbientOcclusionRenderer& operator=(AmbientOcclusionRenderer&&) = delete;

    [[nodiscard]] Result<> prepare(rhi::Device& device);
    /// Records the passes for `view` from its reverse-Z `depth` (width x height). Returns the occlusion, an r16_float
    /// texture of the visible fraction (1: open), or nothing when the view turns it off.
    [[nodiscard]] std::optional<GraphResource> record(RenderGraph& graph, GraphResource depth, const RenderView& view,
                                                      std::uint32_t width, std::uint32_t height);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
