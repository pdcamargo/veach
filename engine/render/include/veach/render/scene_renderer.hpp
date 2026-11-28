#pragma once

// The scene renderer: the scene on the GPU (meshes, meshlets, materials, lights) drawn by the camera's choice of
// renderer. A PhotoMode component on the camera selects photo mode, the reference path tracer
// (docs/adr/0041-photo-mode.md), which accumulates while nothing changes and is the only user of ray tracing:
// BLASes and the TLAS exist only for it. Otherwise the real-time renderer draws the view, GPU-driven
// (docs/adr/0093-gpu-driven-core.md). Both read the same GPU scene and write linear radiance that the display pass
// turns into scene_color (ADR 0037). The shaders come from the app's library (veach_add_scene_shaders), compiled with
// its materials.
//
//   renderer.add_feature(std::make_unique<veach::render::SceneRenderer>(
//       veach::render::SceneShaders{lab_shaders::path_tracer(), lab_shaders::realtime_visibility(),
//                                   lab_shaders::realtime_shade_bins()},
//       lab_shaders::material_kinds, assets));

#include <veach/core/math.hpp>
#include <veach/render/renderer.hpp>
#include <veach/rhi/types.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace veach {
class AssetServer;
}

namespace veach::render {

/// A material kind (ADR 0035), in kind order. EMISSIVE kinds are sampled as lights even with a zero factor.
struct MaterialKind {
    std::string_view name;
    bool emissive = false;
};

/// Mirrors MaterialRecord in shaders/veach/material.slang: 128 bytes, float4/uint2/scalars only.
struct MaterialRecord {
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    float alpha_cutoff = 0.5f;
    float normal_scale = 1.0f;
    math::vec4 base_color{1.0f};
    math::vec4 emission_metallic{0.0f};
    float roughness = 1.0f;
    float emissive_weight = 0.0f;
    rhi::DescriptorHandle sampler;
    rhi::DescriptorHandle base_color_texture;
    rhi::DescriptorHandle metallic_roughness_texture;
    rhi::DescriptorHandle normal_texture;
    rhi::DescriptorHandle emissive_texture;
    math::vec4 custom0{0.0f};
    rhi::DescriptorHandle array_texture0;  ///< a 2D array (Texture2DArray in Slang)
    rhi::DescriptorHandle array_texture1;
};
static_assert(sizeof(MaterialRecord) == 128);
static_assert(offsetof(MaterialRecord, roughness) == 48);
static_assert(offsetof(MaterialRecord, base_color_texture) == 64);
static_assert(offsetof(MaterialRecord, custom0) == 96);

/// The scene shaders, compiled with an app's materials (veach_add_scene_shaders): photo mode's path tracer and the
/// real-time renderer's visibility pass and shading, an entry point per material kind (ADR 0041, ADR 0093).
struct SceneShaders {
    const rhi::ShaderLibrary& path_tracer;
    const rhi::ShaderLibrary& visibility;
    const rhi::ShaderLibrary& shade;  ///< realtime_shade_bins
};

/// What the real-time renderer's culling kept and dropped in one frame (ADR 0093). Read back from the GPU, so it
/// describes the frame `frame`, frames_in_flight frames before the one being recorded; zeros before the first.
struct CullStats {
    std::uint64_t frame = 0;
    std::uint32_t instances = 0;                    ///< in the GPU scene
    std::uint32_t instances_visible = 0;            ///< passed phase 1 (frustum and last frame's pyramid)
    std::uint32_t instances_rechecked = 0;          ///< phase 1 found them occluded: tested again in phase 2
    std::uint32_t instances_disoccluded = 0;        ///< of those, visible against this frame's pyramid
    std::uint32_t meshlets_culled = 0;              ///< outside the frustum, or facing away (normal cone)
    std::uint32_t meshlets_rechecked = 0;           ///< phase 1 found them occluded
    std::uint32_t meshlets_occluded = 0;            ///< hidden in phase 2: not drawn
    std::array<std::uint32_t, 2> meshlets_drawn{};  ///< by phase 1 and by phase 2
    /// Sun shadows (ADR 0095): meshlets drawn into each cascade, by layer (light slot x cascades + cascade).
    std::vector<std::uint32_t> shadow_meshlets_drawn;
};

class SceneRenderer final : public RenderFeature {
public:
    SceneRenderer(SceneShaders shaders, std::span<const MaterialKind> kinds, AssetServer& assets);
    ~SceneRenderer() override;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;
    SceneRenderer(SceneRenderer&&) = delete;
    SceneRenderer& operator=(SceneRenderer&&) = delete;

    [[nodiscard]] std::string_view name() const override { return "veach.scene_renderer"; }
    [[nodiscard]] Result<> prepare(rhi::Device& device) override;
    void record(RenderGraph& graph, const FrameContext& frame) override;

    /// Photo mode's next frame starts a new accumulation (MCP photo_mode does this).
    void reset();
    /// Photo mode's samples per pixel accumulated so far, including the frame being recorded.
    [[nodiscard]] std::uint64_t samples() const;
    /// The real-time renderer's culling, frames_in_flight frames ago.
    [[nodiscard]] const CullStats& cull_stats() const;
    /// Bytes of instance records the last frame uploaded to the GPU scene (0 when nothing changed).
    [[nodiscard]] std::uint64_t instance_bytes_uploaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
