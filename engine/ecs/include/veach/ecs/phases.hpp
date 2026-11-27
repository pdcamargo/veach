#pragma once

// The fixed phases (ADR 0018, docs/specs/ecs-and-phases/spec.md). User systems
// attach to a phase; only the engine decides when phases run:
//
//   Startup                                   once, before the first frame
//   Input                                     every frame
//   Simulation → PostSimulation →
//     TransformPropagation                    every fixed step (1/60 s)
//   PreExtract → Extract → Render             every frame
//   Shutdown                                  once, before teardown
//
// C++:  world.system<Spin>("Spin").kind<veach::phase::Simulation>().each(...);
// Luau: veach.system("Spin", "Simulation", veach.query(Spin), function(e, spin) ... end)

#include <veach/ecs/flecs.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace veach {

namespace phase {
struct Startup {};
struct Input {};
struct Simulation {};
struct PostSimulation {};
struct TransformPropagation {};
struct PreExtract {};
struct Extract {};
struct Render {};
struct Shutdown {};
}  // namespace phase

enum class PhaseId : std::uint8_t {
    startup,
    input,
    simulation,
    post_simulation,
    transform_propagation,
    pre_extract,
    extract,
    render,
    shutdown,
};
inline constexpr std::size_t phase_count = 9;

enum class Cadence : std::uint8_t { startup, per_frame, per_fixed_step, shutdown };

struct PhaseInfo {
    PhaseId id;
    std::string_view name;  ///< As written in scripts and tools, e.g. "Simulation".
    Cadence cadence;
};

/// All phases in execution order.
[[nodiscard]] std::span<const PhaseInfo> phases();
[[nodiscard]] const PhaseInfo& phase_info(PhaseId id);
[[nodiscard]] std::optional<PhaseId> find_phase(std::string_view name);
/// The tag entity systems use with .kind(...).
[[nodiscard]] Entity phase_entity(const World& world, PhaseId id);

/// Owns one Flecs pipeline per phase so the engine can run phases individually.
/// Engine-internal: user code never runs phases.
class PhaseRunner {
public:
    void register_phases(World& world);
    void run(World& world, PhaseId id, float delta_seconds) const;

private:
    std::array<ecs::entity_t, phase_count> pipelines_{};
};

}  // namespace veach
