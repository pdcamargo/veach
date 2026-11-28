#pragma once

// Large-scale sky occlusion for the real-time renderer (docs/specs/sky-occlusion/spec.md, shaders/sky_occlusion.slang,
// shaders/veach/sky_occlusion.slang): a height map of the scene around the camera rendered from above through the
// GPU-driven culling (the highest and the lowest surface of each texel), kept for a few frames; then, per
// cell of 8 x 8 pixels, the arc of the sky the scene hides in 8 azimuths, whether what the pixel sees is sunlit
// (the sun shadow maps), and the share of the sky's ambient light that leaves. The shade pass reads it through
// SkyOcclusionFrame; ambient_light() applies it.

#include <veach/core/error.hpp>
#include <veach/core/math.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>
#include <veach/rhi/types.hpp>

#include "realtime_renderer.hpp"
#include "shadows.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace veach::render {

/// Mirrors SkyOcclusionFrame in shaders/veach/sky_occlusion.slang.
struct SkyOcclusionFrame {
    math::vec4 center{0.0f};  ///< xyz: the map's center (render space, snapped); w: its half-width (m)
    math::vec4 up{0.0f};      ///< xyz; w: meters per texel
    math::vec4 axis_x{0.0f};  ///< xyz: azimuth 0; w: the map's depth range (m)
    math::vec4 axis_y{0.0f};  ///< xyz: azimuth 90 degrees; w: how far its bottom lies below its center (m)
    math::vec4 reach{0.0f};   ///< x, y: the fade's start and end (m from the camera); z: the map's mips
    std::array<math::vec4, 2> lights{};  ///< toward each shadowed light, by ShadowFrame slot; w: 1 if used
    rhi::DescriptorHandle ratios;        ///< rgba16_float, a texel per cell of 8 x 8 pixels
    rhi::DescriptorHandle distances;     ///< r16_float, a texel per cell
    std::array<std::uint32_t, 2> extent{};
    std::array<std::uint32_t, 6> pad{};
};
static_assert(sizeof(SkyOcclusionFrame) == 160);
static_assert(offsetof(SkyOcclusionFrame, ratios) == 112 && offsetof(SkyOcclusionFrame, extent) == 128);

/// The height map's frame: centered on `center` (render space), with axes that depend only on `up`. Public for tests.
struct SkyMapFit {
    math::mat4 clip_from_render{1.0f};   ///< orthographic from above, reverse-Z: depth 1 at the top
    std::array<math::vec4, 5> planes{};  ///< culling: its four sides and its bottom
    math::vec3 center{0.0f};
    math::vec3 up{0.0f, 1.0f, 0.0f};
    math::vec3 axis_x{1.0f, 0.0f, 0.0f};
    math::vec3 axis_y{0.0f, 0.0f, -1.0f};
    float half_width = 0.0f;   ///< meters
    float texel = 0.0f;        ///< meters
    float below = 0.0f;        ///< meters from the center down to the bottom
    float depth_range = 0.0f;  ///< meters from the bottom to the top
};
[[nodiscard]] SkyMapFit fit_sky_map(const math::vec3& center, const math::vec3& up, float half_width,
                                    std::uint32_t resolution);
/// Where the map around a camera at `camera` (render space) is centered, absolute: on a whole texel of a grid fixed in
/// the render frame (`origin` is render space's origin), so static heights do not move as the camera does.
[[nodiscard]] math::dvec3 snap_sky_map(const math::vec3& camera, const math::vec3& up, const math::dvec3& origin,
                                       float texel);

class SkyOcclusionRenderer {
public:
    SkyOcclusionRenderer();
    ~SkyOcclusionRenderer();
    SkyOcclusionRenderer(const SkyOcclusionRenderer&) = delete;
    SkyOcclusionRenderer& operator=(const SkyOcclusionRenderer&) = delete;
    SkyOcclusionRenderer(SkyOcclusionRenderer&&) = delete;
    SkyOcclusionRenderer& operator=(SkyOcclusionRenderer&&) = delete;

    [[nodiscard]] Result<> prepare(rhi::Device& device, const SceneShaders& shaders);

    struct Output {
        rhi::BufferHandle frame;                  ///< SkyOcclusionFrame, in upload memory: always valid after prepare
        bool active = false;                      ///< the shade pass applies it (shade_sky_occlusion)
        std::array<GraphResource, 2> textures{};  ///< with active: what the shade pass reads
    };
    /// Draws the height map around `view` when it is stale, and traces it from the visibility pass's `depth` and
    /// `visibility` (rasterized with `inverse_view_projection`'s inverse), with the sun shadow maps `shadows` and the
    /// scene's sky.
    Output record(RenderGraph& graph, const FrameContext& frame, const RenderView& view, const RealtimeScene& scene,
                  GraphResource depth, GraphResource visibility, const math::mat4& inverse_view_projection,
                  const ShadowRenderer::Output& shadows,
                  std::uint32_t width, std::uint32_t height);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
