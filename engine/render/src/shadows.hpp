#pragma once

// Sun shadows for the real-time renderer (docs/specs/sun-shadows/spec.md, ADR 0095, ADR 0100, ADR 0062): cascaded
// shadow maps for the brightest directional lights, each cascade culled by the GPU-driven culling (gpu_cull.slang,
// without occlusion or normal cones) into compacted draw lists of its own and drawn depth-only, alpha-tested casters
// with their coverage test. The shading function looks them up through ShadowFrame (shaders/veach/shadows.slang).

#include <veach/core/error.hpp>
#include <veach/core/math.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>
#include <veach/render/shadow_cascades.hpp>
#include <veach/rhi/types.hpp>

#include "realtime_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace veach::render {

/// Mirrors ShadowCascade in shaders/veach/shadows.slang.
struct ShadowCascade {
    math::mat4 clip_from_render{1.0f};  ///< orthographic, reverse-Z: depth 1 toward the light
    math::vec4 scale{0.0f};             ///< x: texel size (m); y: meters per unit of depth; z: radius (m)
};
static_assert(sizeof(ShadowCascade) == 80);

/// Mirrors ShadowFrame in shaders/veach/shadows.slang.
struct ShadowFrame {
    std::array<ShadowCascade, max_shadowed_lights * max_shadow_cascades> cascades{};  ///< by layer of the map
    rhi::DescriptorHandle map;      ///< Texture2DArray<float>, depth32
    rhi::DescriptorHandle compare;  ///< comparison sampler, greater_equal, linear
    std::uint32_t light_count = 0;
    std::uint32_t cascade_count = 0;
    std::array<std::uint32_t, 2> lights{};  ///< LightRecord indices
    math::vec4 view_axis{0.0f};             ///< the camera's forward; w: minus its dot with the camera's position
    math::vec4 splits{0.0f};                ///< each cascade's far view depth (m)
    float fade_start = 0.0f;
    float fade_end = 0.0f;
    float resolution = 0.0f;
    float normal_offset = 0.0f;  ///< texels
    float depth_bias = 0.0f;     ///< texels
    float max_penumbra = 0.0f;   ///< texels
    float blend = 0.0f;
    std::uint32_t occluder_base = 0;    ///< where layer 0's pyramid starts in `occluders` (uints)
    rhi::DescriptorHandle occluders;    ///< StructuredBuffer<uint>: each layer's occluder pyramid, level 0 first
    std::uint32_t occluder_size = 0;    ///< 0: none
    std::uint32_t occluder_stride = 0;  ///< uints from one layer's pyramid to the next
};
static_assert(sizeof(ShadowFrame) == 752);
static_assert(offsetof(ShadowFrame, map) == 640 && offsetof(ShadowFrame, lights) == 664 &&
              offsetof(ShadowFrame, view_axis) == 672 && offsetof(ShadowFrame, blend) == 728);

class ShadowRenderer {
public:
    ShadowRenderer();
    ~ShadowRenderer();
    ShadowRenderer(const ShadowRenderer&) = delete;
    ShadowRenderer& operator=(const ShadowRenderer&) = delete;
    ShadowRenderer(ShadowRenderer&&) = delete;
    ShadowRenderer& operator=(ShadowRenderer&&) = delete;

    [[nodiscard]] Result<> prepare(rhi::Device& device, const SceneShaders& shaders);

    struct Output {
        rhi::BufferHandle frame;                 ///< ShadowFrame, in upload memory: always valid after prepare
        std::optional<GraphResource> map;        ///< the shadow maps, when some light casts; the shading reads it
        std::optional<GraphResource> occluders;  ///< the culling pyramids, which the shading reads too
    };
    /// Fits, culls and draws this frame's cascades for `view`; what each cascade drew reaches
    /// `stats.shadow_meshlets_drawn` frames_in_flight frames later (`stats` must outlive the frames in flight).
    /// `depth` is the camera's depth buffer once visibility has drawn it, and `inverse_depth_projection` takes its
    /// pixels back to render space (the jittered view's inverse, ADR 0097): casters that shadow none of its pixels are
    /// not drawn (the receiver mask). Without it every caster in a cascade's slice is.
    Output record(RenderGraph& graph, const FrameContext& frame, const RenderView& view, const RealtimeScene& scene,
                  GraphResource depth, const math::mat4& inverse_depth_projection, CullStats& stats);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
