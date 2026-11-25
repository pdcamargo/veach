#pragma once

// The reference path tracer (docs/adr/0034-lights-and-reference-path-tracer.md)
// as a render feature. It draws the primary view when its camera has a
// PathTracer component, accumulating while nothing changes. The shader comes
// from the app's library (veach_add_path_tracer), compiled with its materials.
//
//   renderer.add_feature(std::make_unique<veach::render::PathTracerFeature>(
//       lab_shaders::path_tracer(), lab_shaders::material_kinds, assets));

#include <veach/core/math.hpp>
#include <veach/render/renderer.hpp>
#include <veach/rhi/types.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace veach {
class AssetServer;
}

namespace veach::render {

/// A material kind (ADR 0033), in kind order. EMISSIVE kinds are sampled as lights even with a zero factor.
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
    math::vec4 custom1{0.0f};
};
static_assert(sizeof(MaterialRecord) == 128);
static_assert(offsetof(MaterialRecord, roughness) == 48);
static_assert(offsetof(MaterialRecord, base_color_texture) == 64);
static_assert(offsetof(MaterialRecord, custom0) == 96);

/// The scene shaders, compiled with an app's materials (veach_add_path_tracer): the reference path tracer and the
/// real-time renderer's visibility and shading passes (ADR 0037).
struct SceneShaders {
    const rhi::ShaderLibrary& path_tracer;
    const rhi::ShaderLibrary& visibility;
    const rhi::ShaderLibrary& shade;
};

class PathTracerFeature final : public RenderFeature {
public:
    PathTracerFeature(SceneShaders shaders, std::span<const MaterialKind> kinds, AssetServer& assets);
    ~PathTracerFeature() override;
    PathTracerFeature(const PathTracerFeature&) = delete;
    PathTracerFeature& operator=(const PathTracerFeature&) = delete;
    PathTracerFeature(PathTracerFeature&&) = delete;
    PathTracerFeature& operator=(PathTracerFeature&&) = delete;

    [[nodiscard]] std::string_view name() const override { return "veach.path_tracer"; }
    [[nodiscard]] Result<> prepare(rhi::Device& device) override;
    void record(RenderGraph& graph, const FrameContext& frame) override;

    /// The next frame starts a new accumulation (MCP path_trace does this).
    void reset();
    /// Samples per pixel accumulated so far, including the frame being recorded.
    [[nodiscard]] std::uint64_t samples() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
