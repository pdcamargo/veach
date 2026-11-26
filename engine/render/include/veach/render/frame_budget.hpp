#pragma once

// The frame and memory budgets of ADR 0049: two quality tiers, each with a GPU-time slice and a GPU-memory slice
// per subsystem, and which render-graph pass and which GPU resource belongs to which subsystem. The MCP tool
// `frame_budget` measures a frame against them; tests check that every pass and resource has a subsystem.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace veach::render {

enum class QualityTier : std::uint8_t { performance, quality };

/// Where GPU time goes. `reference` (the path tracer) is not part of a real-time frame and has no slice.
enum class Subsystem : std::uint8_t {
    visibility,       ///< visibility buffer, surfaces, TLAS
    direct_lighting,  ///< ReSTIR, the shading pass with its shadow rays, the denoiser
    gi,               ///< Candela, or the ambient stopgap when GI is off
    reflections,
    sky,           ///< the atmosphere's lookup tables
    fog,           ///< volumetric fog
    transparency,  ///< transparent surfaces and particles
    post,          ///< upscaling, display, presentation, readbacks
    reference,     ///< the path tracer: not budgeted
};
inline constexpr std::size_t subsystem_count = 9;

/// Where GPU memory goes.
enum class MemoryKind : std::uint8_t {
    geometry,        ///< vertices, indices, meshlets, instance and material records
    acceleration,    ///< BLASes and TLAS with their scratch
    textures,        ///< material textures
    render_targets,  ///< per-view targets, history, reservoirs, readbacks, swapchain
    gi,              ///< Candela's radiance cache
    sky,             ///< the atmosphere's lookup tables
    other,
};
inline constexpr std::size_t memory_kind_count = 7;

struct TierBudget {
    std::string_view name;
    double fps = 0.0;
    double frame_ms = 0.0;
    float render_scale = 1.0f;  ///< direct lighting settings that fit the slices on Bistro (ADR 0049)
    std::uint32_t light_candidates = 0;
    std::uint32_t spatial_samples = 0;
    float gi_ray_rate = 0.25f;                             ///< Candela's gather rays per render pixel
    std::array<double, subsystem_count> ms{};              ///< by Subsystem; the rest of frame_ms is headroom
    std::array<std::uint64_t, memory_kind_count> bytes{};  ///< by MemoryKind
    std::uint64_t memory_bytes = 0;                        ///< total; the rest is headroom
};

[[nodiscard]] const TierBudget& tier_budget(QualityTier tier);
[[nodiscard]] std::optional<QualityTier> tier_from_string(std::string_view name);
[[nodiscard]] std::string_view to_string(Subsystem subsystem);
[[nodiscard]] std::string_view to_string(MemoryKind kind);

/// The subsystem a render-graph pass belongs to, by its name; nullopt for a pass the budget does not know (a test
/// checks there are none).
[[nodiscard]] std::optional<Subsystem> subsystem_of_pass(std::string_view pass);
/// The memory kind of a GPU resource, by its debug name; nullopt for one the budget does not know.
[[nodiscard]] std::optional<MemoryKind> memory_kind_of(std::string_view resource);

}  // namespace veach::render
