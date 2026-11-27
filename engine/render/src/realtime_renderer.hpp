#pragma once

// The real-time renderer for now (docs/adr/0040-interim-realtime-path-visibility-buffer.md): a visibility buffer
// over the cooked meshlets, one compute pass that shades it with every punctual light (unshadowed), the sky's ambient
// light and emission, then the display transform. No ray tracing and nothing carried between frames. The scene
// renderer owns the scene on the GPU and hands it over each frame.

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
    std::function<void(PassBuilder&)> read_geometry;  ///< declares the mesh buffers, textures and records
    std::span<const std::uint32_t> meshlet_counts;    ///< per instance
    rhi::BufferHandle instances;
    rhi::BufferHandle materials;
    rhi::BufferHandle lights;
    std::uint32_t light_count = 0;
    rhi::BufferHandle sky;                       ///< SkyFrame (ADR 0038)
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
