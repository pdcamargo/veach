#pragma once

// The records of shaders/gpu_cull.slang (ADR 0093), shared by the camera's culling (realtime_renderer.cpp) and the sun
// shadows' per-cascade culling (shadows.cpp).

#include <veach/core/math.hpp>
#include <veach/rhi/shader.hpp>
#include <veach/rhi/types.hpp>

#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace veach::render {

/// Mirrors CullView in shaders/gpu_cull.slang (one element of a structured buffer).
struct CullView {
    math::mat4 view_projection{1.0f};
    math::mat4 occlusion_view_projection{1.0f};
    std::array<math::vec4, 5> planes{};
    math::vec4 camera{0.0f};
    std::array<std::uint32_t, 2> screen{};
    std::array<std::uint32_t, 2> hiz_size{};
    std::uint32_t hiz_levels = 0;
    std::uint32_t flags = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t phase_stride = 0;
    std::array<std::uint32_t, 8> list_offsets{};
    std::uint32_t task_capacity = 0;
    std::array<std::uint32_t, 3> pad{};
    std::array<math::vec4, 6> receiver_planes{};  ///< cull_shadow_view: the receivers' slice of the camera's frustum
    math::vec4 sweep{0.0f};                       ///< cull_shadow_view: away from the light; w: smallest caster radius
};
static_assert(sizeof(CullView) == 416);
static_assert(offsetof(CullView, screen) == 224 && offsetof(CullView, list_offsets) == 256 &&
              offsetof(CullView, receiver_planes) == 304);
constexpr std::uint32_t cull_previous_hiz = 1u;
/// A shadow cascade (shadows.cpp): no normal cones, and only casters big enough whose shadows reach the receivers.
constexpr std::uint32_t cull_shadow_view = 2u;

/// Mirrors CullParams in shaders/gpu_cull.slang.
struct alignas(16) CullParams {
    rhi::DescriptorHandle view;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle hiz;
    rhi::DescriptorHandle counters;
    rhi::DescriptorHandle draw_args;
    rhi::DescriptorHandle dispatch_args;
    rhi::DescriptorHandle tasks;
    rhi::DescriptorHandle recheck_instances;
    rhi::DescriptorHandle recheck_meshlets;
    rhi::DescriptorHandle visible;
    rhi::DescriptorHandle bin_tiles;
    std::uint32_t phase = 0;
    std::uint32_t bin_count = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};
static_assert(rhi::parameters_match<CullParams>(
    veach_render_shaders::layout::gpu_cull,
    {VEACH_PARAM(CullParams, view), VEACH_PARAM(CullParams, instances), VEACH_PARAM(CullParams, materials),
     VEACH_PARAM(CullParams, hiz), VEACH_PARAM(CullParams, counters), VEACH_PARAM(CullParams, draw_args),
     VEACH_PARAM(CullParams, dispatch_args), VEACH_PARAM(CullParams, tasks), VEACH_PARAM(CullParams, recheck_instances),
     VEACH_PARAM(CullParams, recheck_meshlets), VEACH_PARAM(CullParams, visible), VEACH_PARAM(CullParams, bin_tiles),
     VEACH_PARAM(CullParams, phase), VEACH_PARAM(CullParams, bin_count), VEACH_PARAM(CullParams, pad0),
     VEACH_PARAM(CullParams, pad1)}));

// gpu_cull.slang's counters, records and sizes.
constexpr std::uint32_t counter_count = 64;
constexpr std::uint32_t counter_recheck_instances = 2;
constexpr std::uint32_t counter_recheck_meshlets = 3;
constexpr std::uint32_t counter_instances_visible = 4;
constexpr std::uint32_t counter_meshlets_culled = 6;
constexpr std::uint32_t counter_meshlets_occluded = 7;
constexpr std::uint32_t dispatch_record = 16;  ///< bytes per DispatchIndirectArgs record (12, padded)
constexpr std::uint64_t dispatch_tasks_1 = 0 * dispatch_record;
constexpr std::uint64_t dispatch_recheck_instances = 1 * dispatch_record;
constexpr std::uint64_t dispatch_recheck_meshlets = 2 * dispatch_record;
constexpr std::uint64_t dispatch_tasks_2 = 3 * dispatch_record;
constexpr std::uint32_t groups_per_row = 32768;

/// Mirrors VisibilityParams in shaders/realtime_visibility.slang (the visibility pass and the shadow casters' pass).
struct alignas(16) VisibilityParams {
    math::mat4 view_projection{1.0f};
    math::mat4 previous_view_projection{1.0f};
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle visible;
    std::array<float, 2> inverse_size{};
    std::uint32_t list_offset = 0;
    std::uint32_t pad0 = 0;
    std::array<float, 2> jitter{};  ///< UV: the projection's sub-pixel offset this frame (ADR 0061); 0 for shadows
};
static_assert(rhi::parameters_match<VisibilityParams>(
    veach_render_shaders::layout::realtime_visibility,
    {VEACH_PARAM(VisibilityParams, view_projection), VEACH_PARAM(VisibilityParams, previous_view_projection),
     VEACH_PARAM(VisibilityParams, instances), VEACH_PARAM(VisibilityParams, materials),
     VEACH_PARAM(VisibilityParams, visible), VEACH_PARAM(VisibilityParams, inverse_size),
     VEACH_PARAM(VisibilityParams, list_offset), VEACH_PARAM(VisibilityParams, pad0),
     VEACH_PARAM(VisibilityParams, jitter)}));

/// Indirect-style group counts for a direct dispatch of `groups` groups: rows of groups_per_row.
inline std::array<std::uint32_t, 2> rows_of(std::uint32_t groups) {
    return {std::min(groups, groups_per_row), std::max(1u, (groups + groups_per_row - 1) / groups_per_row)};
}

}  // namespace veach::render
