#pragma once

// The real-time renderer (ADR 0037, ADR 0040, ADR 0043): visibility buffer, surfaces with motion and
// disocclusion, lighting, then the upscaler and the display transform, with per-view history. The scene renderer
// owns the scene on the GPU and hands it over each frame.

#include <veach/core/error.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>

#include <functional>
#include <memory>
#include <span>

namespace veach::render {

/// The scene as the scene renderer put it on the GPU this frame.
struct RealtimeScene {
    GraphResource tlas;
    rhi::AccelHandle tlas_handle;
    std::function<void(PassBuilder&)> read_scene;     ///< declares everything a ray-tracing pass reads
    std::function<void(PassBuilder&)> read_geometry;  ///< declares what the visibility pass pulls vertices from
    std::span<const std::uint32_t> meshlet_counts;    ///< per instance
    rhi::BufferHandle instances;
    rhi::BufferHandle materials;
    rhi::BufferHandle lights;
    rhi::BufferHandle emissive;
    rhi::BufferHandle alias;        ///< ReSTIR's light table: light_count + emissive_count AliasEntry (ADR 0041)
    std::uint64_t lights_hash = 0;  ///< changes when any light does: reservoirs are dropped
    std::uint32_t light_count = 0;
    std::uint32_t emissive_count = 0;
    float emissive_total_weight = 0.0f;
    std::uint32_t scene_flags = 0;
    rhi::BufferHandle sky;                       ///< SkyFrame (ADR 0050)
    std::function<void(PassBuilder&)> read_sky;  ///< declares it and the sky's LUTs
};

class RealtimeRenderer {
public:
    RealtimeRenderer();
    ~RealtimeRenderer();
    RealtimeRenderer(const RealtimeRenderer&) = delete;
    RealtimeRenderer& operator=(const RealtimeRenderer&) = delete;
    RealtimeRenderer(RealtimeRenderer&&) = delete;
    RealtimeRenderer& operator=(RealtimeRenderer&&) = delete;

    [[nodiscard]] Result<> prepare(rhi::Device& device, const SceneShaders& shaders);
    /// Records the view's passes, ending with scene_color.
    void record(RenderGraph& graph, const FrameContext& frame, const RenderView& view, const RealtimeScene& scene);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
