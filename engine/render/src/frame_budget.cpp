#include <veach/render/frame_budget.hpp>

#include <utility>

namespace veach::render {
namespace {

constexpr std::uint64_t mib = 1024ull * 1024ull;

// ADR 0049, on the reference machine (M5 Max) at 1440p output. Indexed by Subsystem and MemoryKind.
constexpr TierBudget performance{
    .name = "performance",
    .fps = 60.0,
    .frame_ms = 1000.0 / 60.0,
    .render_scale = 0.5f,
    .light_candidates = 8,
    .spatial_samples = 1,
    //    visibility, direct, gi,  reflections, sky,  fog, transparency, post, reference
    .ms = {2.5, 4.5, 4.5, 2.0, 0.25, 0.5, 0.5, 1.5, 0.0},
    //       geometry,  acceleration, textures,   render targets, gi,       sky,     other
    .bytes = {512 * mib, 768 * mib, 1536 * mib, 640 * mib, 160 * mib, 8 * mib, 64 * mib},
    .memory_bytes = 4096 * mib,
};

constexpr TierBudget quality{
    .name = "quality",
    .fps = 30.0,
    .frame_ms = 1000.0 / 30.0,
    .render_scale = 0.75f,
    .light_candidates = 12,
    .spatial_samples = 2,
    .ms = {3.0, 10.0, 10.0, 5.0, 0.5, 1.0, 1.0, 2.0, 0.0},
    .bytes = {512 * mib, 768 * mib, 1536 * mib, 1280 * mib, 256 * mib, 8 * mib, 64 * mib},
    .memory_bytes = 5120 * mib,
};

struct Prefix {
    std::string_view prefix;
    Subsystem subsystem;
};

// Pass names, matched by prefix (the first match wins).
constexpr std::array pass_prefixes{
    Prefix{"visibility", Subsystem::visibility},
    Prefix{"surface", Subsystem::visibility},
    Prefix{"scene: ", Subsystem::visibility},
    Prefix{"clear", Subsystem::visibility},
    Prefix{"restir: ", Subsystem::direct_lighting},
    Prefix{"shade", Subsystem::direct_lighting},
    Prefix{"denoise: ", Subsystem::direct_lighting},
    Prefix{"accumulate", Subsystem::direct_lighting},
    Prefix{"gi: ", Subsystem::gi},
    Prefix{"candela: ", Subsystem::gi},
    Prefix{"sky ", Subsystem::sky},
    Prefix{"aerial perspective", Subsystem::sky},
    Prefix{"taau", Subsystem::post},
    Prefix{"metalfx", Subsystem::post},
    Prefix{"display", Subsystem::post},
    Prefix{"present", Subsystem::post},
    Prefix{"screenshot", Subsystem::post},
    Prefix{"real-time counters", Subsystem::post},
    Prefix{"path tracer", Subsystem::reference},
};

struct MemoryPrefix {
    std::string_view prefix;
    MemoryKind kind;
};

constexpr std::array memory_prefixes{
    MemoryPrefix{"mesh BLAS", MemoryKind::acceleration},
    MemoryPrefix{"scene TLAS", MemoryKind::acceleration},
    MemoryPrefix{"triangle BLAS", MemoryKind::acceleration},
    MemoryPrefix{"triangle TLAS", MemoryKind::acceleration},
    MemoryPrefix{"scene texture", MemoryKind::textures},
    MemoryPrefix{"mesh ", MemoryKind::geometry},
    MemoryPrefix{"meshlet", MemoryKind::geometry},
    MemoryPrefix{"scene ", MemoryKind::geometry},
    MemoryPrefix{"triangle vertices", MemoryKind::geometry},
    MemoryPrefix{"light alias table", MemoryKind::geometry},
    MemoryPrefix{"candela", MemoryKind::gi},
    MemoryPrefix{"sky", MemoryKind::sky},
    MemoryPrefix{"aerial perspective", MemoryKind::sky},
    // Per-view targets and the frame's records.
    MemoryPrefix{"visibility", MemoryKind::render_targets},
    MemoryPrefix{"surface ", MemoryKind::render_targets},
    MemoryPrefix{"motion", MemoryKind::render_targets},
    MemoryPrefix{"radiance", MemoryKind::render_targets},
    MemoryPrefix{"accumulation", MemoryKind::render_targets},
    MemoryPrefix{"denoise", MemoryKind::render_targets},
    MemoryPrefix{"indirect light", MemoryKind::render_targets},
    MemoryPrefix{"reservoirs", MemoryKind::render_targets},
    MemoryPrefix{"upscaled", MemoryKind::render_targets},
    MemoryPrefix{"scene_color", MemoryKind::render_targets},
    MemoryPrefix{"present", MemoryKind::render_targets},
    MemoryPrefix{"screenshot", MemoryKind::render_targets},
    MemoryPrefix{"realtime frame", MemoryKind::render_targets},
    MemoryPrefix{"real-time counters", MemoryKind::render_targets},
    MemoryPrefix{"path tracer", MemoryKind::render_targets},
};

}  // namespace

const TierBudget& tier_budget(QualityTier tier) {
    return tier == QualityTier::quality ? quality : performance;
}

std::optional<QualityTier> tier_from_string(std::string_view name) {
    if (name == "performance") {
        return QualityTier::performance;
    }
    if (name == "quality") {
        return QualityTier::quality;
    }
    return std::nullopt;
}

std::string_view to_string(Subsystem subsystem) {
    constexpr std::array<std::string_view, subsystem_count> names{
        "visibility", "direct_lighting", "gi", "reflections", "sky", "fog", "transparency", "post", "reference"};
    return names.at(std::to_underlying(subsystem));
}

std::string_view to_string(MemoryKind kind) {
    constexpr std::array<std::string_view, memory_kind_count> names{
        "geometry", "acceleration", "textures", "render_targets", "gi", "sky", "other"};
    return names.at(std::to_underlying(kind));
}

std::optional<Subsystem> subsystem_of_pass(std::string_view pass) {
    for (const Prefix& p : pass_prefixes) {
        if (pass.starts_with(p.prefix)) {
            return p.subsystem;
        }
    }
    return std::nullopt;
}

std::optional<MemoryKind> memory_kind_of(std::string_view resource) {
    for (const MemoryPrefix& p : memory_prefixes) {
        if (resource.starts_with(p.prefix)) {
            return p.kind;
        }
    }
    return std::nullopt;
}

}  // namespace veach::render
