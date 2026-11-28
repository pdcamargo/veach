// SP-05's stages registered on the engine's stage runtime (VCH-112). Glue only: the stage functions are in system.cpp.

#include <veach/space/gen/stages.hpp>

#include <string>

namespace space::gen {

namespace pg = veach::procgen;

std::uint64_t stage_hash(const UniverseRecord& r) {
    pg::RecordHasher h(universe_stage_id, universe_stage_version);
    h.add(r.seed);
    h.add_double(r.habitability_bias);
    return h.value();
}

veach::Result<Stages> register_stages(pg::StageRegistry& registry, const UniverseRecord& settings) {
    using HashUniverse = std::uint64_t (*)(const UniverseRecord&);
    using HashSystem = std::uint64_t (*)(const SystemRecord&);
    using HashBodies = std::uint64_t (*)(const BodyPhysicsSet&);
    auto universe =
        registry.add<UniverseRecord>({.id = std::string(universe_stage_id), .version = universe_stage_version},
                                     HashUniverse{&stage_hash}, [settings](const pg::StageContext& ctx) {
                                         UniverseRecord u = settings;
                                         u.seed = ctx.universe_seed;
                                         return u;
                                     });
    if (!universe) {
        return std::unexpected(universe.error());
    }
    auto system = registry.add<SystemRecord>(
        {.id = std::string(system_stage_id), .version = system_stage_version}, HashSystem{&stage_hash},
        [](const pg::StageContext& ctx, const UniverseRecord& u) {
            return generate_system(StageContext{&u, SystemAddress{ctx.address}});
        },
        pg::from(*universe, pg::root_address));
    if (!system) {
        return std::unexpected(system.error());
    }
    auto bodies = registry.add<BodyPhysicsSet>(
        {.id = std::string(body_physics_stage_id), .version = body_physics_stage_version}, HashBodies{&stage_hash},
        [](const pg::StageContext& ctx, const UniverseRecord& u, const SystemRecord& s) {
            return derive_all_body_physics(StageContext{&u, SystemAddress{ctx.address}}, s);
        },
        pg::from(*universe, pg::root_address), pg::from(*system));
    if (!bodies) {
        return std::unexpected(bodies.error());
    }
    return Stages{*universe, *system, *bodies};
}

}  // namespace space::gen
