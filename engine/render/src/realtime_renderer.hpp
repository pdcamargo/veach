#pragma once

// The real-time renderer (docs/adr/0093-gpu-driven-core.md, ADR 0060): GPU-driven culling of the GPU scene's
// instances and meshlets in two phases against a HiZ pyramid, a visibility buffer drawn from the compacted lists with
// motion vectors, the suns' cascaded shadow maps (shadows.hpp, ADR 0095), shading binned by material kind (every
// punctual light, the suns through their shadow maps; the sky's ambient light; emission; the sky and aerial
// perspective), temporal anti-aliasing (docs/specs/temporal-aa/spec.md, ADR 0096: a jittered projection, a history
// resolve on pre-exposed radiance), the space backdrop, then the display transform. No ray tracing. The scene renderer
// owns the scene on the GPU and hands it over each frame.

#include <veach/core/error.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace veach::render {

/// Draw lists by material class and meshlet size: opaque meshlets of up to 16, 32, 64 and 124 triangles, then the same
/// for alpha-tested ones. Each list's draws run as many vertices as its largest meshlet needs (gpu_cull.slang).
inline constexpr std::uint32_t draw_bucket_count = 4;
inline constexpr std::uint32_t draw_list_count = 2 * draw_bucket_count;
[[nodiscard]] constexpr std::uint32_t draw_list_of(bool masked, std::uint32_t triangles) {
    const std::uint32_t bucket = triangles <= 16 ? 0u : (triangles <= 32 ? 1u : (triangles <= 64 ? 2u : 3u));
    return (masked ? draw_bucket_count : 0u) + bucket;
}

/// The scene as the scene renderer put it on the GPU this frame.
struct RealtimeScene {
    std::function<void(PassBuilder&)> read_meshes;    ///< declares the mesh buffers, instances and materials
    std::function<void(PassBuilder&)> read_geometry;  ///< read_meshes, the textures and the lights
    GraphResource instances;                          ///< the GPU scene's records
    std::uint32_t instance_count = 0;
    std::uint32_t task_count = 0;  ///< cull_meshlets tasks if every instance with several meshlets survived
    std::array<std::uint32_t, draw_list_count> list_capacity{};  ///< meshlets per draw list if all survived
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

    /// `kind_count`: the material kinds the shading library has an entry point for (shade_kind_0...).
    [[nodiscard]] Result<> prepare(rhi::Device& device, const SceneShaders& shaders, std::uint32_t kind_count);
    /// Records the view's passes, ending with scene_color.
    void record(RenderGraph& graph, const FrameContext& frame, const RenderView& view, const RealtimeScene& scene);
    /// What culling did, frames_in_flight frames ago (SceneRenderer::cull_stats).
    [[nodiscard]] const CullStats& stats() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
