#pragma once

// GPU records the scene renderer uploads and both renderers read (ADR 0034, ADR 0037, ADR 0040). Each mirrors a
// Slang struct in shaders/veach/scene.slang; the sizes are asserted here.

#include <veach/core/math.hpp>
#include <veach/rhi/shader.hpp>
#include <veach/rhi/types.hpp>

#include "veach_render_shaders.hpp"

#include <array>
#include <cstdint>

namespace veach::render {

// Flag bits, as in shaders/veach/material.slang.
constexpr std::uint32_t instance_has_alpha = 1u << 0;  ///< InstanceRecord::flags, as in shaders/path_tracer.slang
// TLAS instance masks, as in shaders/veach/scene.slang: visibility rays stop at any opaque hit and test coverage
// only on the alpha-tested instances.
constexpr std::uint8_t instance_mask_opaque = 1;
constexpr std::uint8_t instance_mask_alpha = 2;
constexpr std::uint32_t scene_has_alpha = 1u << 0;  ///< SceneData::flags: some instance is alpha-tested
constexpr std::uint32_t has_base_color_texture = 1u << 0;
constexpr std::uint32_t has_metallic_roughness_texture = 1u << 1;
constexpr std::uint32_t has_normal_texture = 1u << 2;
constexpr std::uint32_t has_emissive_texture = 1u << 3;
constexpr std::uint32_t alpha_mask = 1u << 4;
constexpr std::uint32_t alpha_blend = 1u << 5;
constexpr std::uint32_t double_sided = 1u << 6;
constexpr std::uint32_t emissive = 1u << 7;

/// Mirrors InstanceRecord in shaders/veach/scene.slang.
struct InstanceRecord {
    rhi::DescriptorHandle vertices;
    rhi::DescriptorHandle indices;
    rhi::DescriptorHandle slots;
    std::uint32_t material_base = 0;
    std::uint32_t flags = 0;
    std::array<math::vec4, 3> object_to_world{};
    std::array<math::vec4, 3> normal_to_world{};
    rhi::DescriptorHandle meshlets;
    rhi::DescriptorHandle meshlet_vertices;
    rhi::DescriptorHandle meshlet_triangles;
    std::uint32_t meshlet_count = 0;
    std::uint32_t pad = 0;
    std::array<math::vec4, 3> previous_object_to_world{};  ///< last frame's object_to_world (ADR 0040)
};
static_assert(sizeof(InstanceRecord) == 208);

/// Mirrors LightRecord.
struct LightRecord {
    math::vec4 position{0.0f};
    math::vec4 direction{0.0f};
    math::vec4 radiance{0.0f};
    float cos_inner = 1.0f;
    float cos_outer = 0.0f;
    std::uint32_t kind = 0;
    float size = 0.0f;  ///< RenderLight::size: sphere radius or angular radius; 0 = punctual
};
static_assert(sizeof(LightRecord) == 64);

/// Mirrors EmissiveTriangle.
struct EmissiveTriangle {
    std::uint32_t instance = 0;
    std::uint32_t primitive = 0;
    float cdf = 0.0f;
    float pad = 0.0f;
    math::vec4 p0{0.0f};  ///< world corners: w of p0 = emissive weight, w of p1 = 1 if double sided
    math::vec4 p1{0.0f};
    math::vec4 p2{0.0f};
};
static_assert(sizeof(EmissiveTriangle) == 64);

/// Mirrors AliasEntry in shaders/veach/realtime.slang: ReSTIR's light table (ADR 0041).
struct AliasEntry {
    float threshold = 1.0f;
    std::uint32_t alias = 0;
    float pdf = 0.0f;
    std::uint32_t pad = 0;
};
static_assert(sizeof(AliasEntry) == 16);

/// The rows of the upper 3x4 of a column-major matrix.
inline std::array<math::vec4, 3> rows_of(const math::mat4& m) {
    return {math::vec4(m[0][0], m[1][0], m[2][0], m[3][0]), math::vec4(m[0][1], m[1][1], m[2][1], m[3][1]),
            math::vec4(m[0][2], m[1][2], m[2][2], m[3][2])};
}

/// Mirrors Params in shaders/display.slang.
struct alignas(16) DisplayParams {
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle output;
    std::array<std::uint32_t, 2> size{};
    float exposure = 1.0f;
    std::uint32_t tonemap = 1;
    std::array<std::uint32_t, 2> input_size{};
    std::array<std::uint32_t, 2> pad{};
};
static_assert(rhi::parameters_match<DisplayParams>(
    veach_render_shaders::layout::display,
    {VEACH_PARAM(DisplayParams, radiance), VEACH_PARAM(DisplayParams, output), VEACH_PARAM(DisplayParams, size),
     VEACH_PARAM(DisplayParams, exposure), VEACH_PARAM(DisplayParams, tonemap), VEACH_PARAM(DisplayParams, input_size),
     VEACH_PARAM(DisplayParams, pad)}));

}  // namespace veach::render
