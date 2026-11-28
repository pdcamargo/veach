#pragma once

// SP-05's stages on the engine's stage runtime (engine/procgen, docs/specs/procgen/spec.md): G0 `space:universe`,
// S1 `space:system`, B1 `space:body.physics` and `space:body.atmosphere` (VCH-115, atmosphere.hpp), registered with
// their ids, versions, stability class and canonical hashes. The stage functions are generate_system() and
// derive_all_body_physics() in system.hpp; the runtime gives them their seeds (veach::procgen::stage_seed, the same as
// stage_seed() there) and memoizes their records.
//
//   veach::procgen::StageRegistry registry;
//   const space::gen::Stages stages = *space::gen::register_stages(registry, {.habitability_bias = 0.25});
//   veach::procgen::StageRuntime runtime(registry, {.universe_seed = 42});
//   auto system = runtime.get(stages.system, address.value);    // Result<std::shared_ptr<const SystemRecord>>

#include <veach/procgen/stage.hpp>
#include <veach/space/gen/atmosphere.hpp>
#include <veach/space/gen/system.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace space::gen {

inline constexpr std::string_view universe_stage_id = "space:universe";
inline constexpr std::uint32_t universe_stage_version = 1;

using BodyPhysicsSet = std::vector<BodyPhysics>;

struct Stages {
    veach::procgen::Stage<UniverseRecord> universe;      ///< G0, at the root address (0)
    veach::procgen::Stage<SystemRecord> system;          ///< S1, at system addresses
    veach::procgen::Stage<BodyPhysicsSet> body_physics;  ///< B1, at system addresses: every body of the system
    veach::procgen::Stage<AtmosphereSet> atmosphere;     ///< after B1, at system addresses: every body's air
};

/// Registers G0, S1, B1 and the atmosphere stage (frozen, canonical). G0's record is `settings` with the runtime's
/// universe seed: creator settings are genesis, so a registry belongs to one universe
/// (docs/adr/0055-universe-identity-and-versioning.md).
[[nodiscard]] veach::Result<Stages> register_stages(veach::procgen::StageRegistry& registry,
                                                    const UniverseRecord& settings);

std::uint64_t stage_hash(const UniverseRecord& r);

}  // namespace space::gen
