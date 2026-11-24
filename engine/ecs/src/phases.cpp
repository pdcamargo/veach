#include <veach/ecs/phases.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/profile.hpp>

#include <string>

namespace veach {

namespace {

constexpr std::array<PhaseInfo, phase_count> phase_table{{
    {PhaseId::startup, "Startup", Cadence::startup},
    {PhaseId::input, "Input", Cadence::per_frame},
    {PhaseId::simulation, "Simulation", Cadence::per_fixed_step},
    {PhaseId::post_simulation, "PostSimulation", Cadence::per_fixed_step},
    {PhaseId::transform_propagation, "TransformPropagation", Cadence::per_fixed_step},
    {PhaseId::pre_extract, "PreExtract", Cadence::per_frame},
    {PhaseId::extract, "Extract", Cadence::per_frame},
    {PhaseId::render, "Render", Cadence::per_frame},
    {PhaseId::shutdown, "Shutdown", Cadence::shutdown},
}};

}  // namespace

std::span<const PhaseInfo> phases() {
    return phase_table;
}

const PhaseInfo& phase_info(PhaseId id) {
    return phase_table.at(static_cast<std::size_t>(id));
}

std::optional<PhaseId> find_phase(std::string_view name) {
    for (const PhaseInfo& info : phase_table) {
        if (info.name == name) {
            return info.id;
        }
    }
    return std::nullopt;
}

Entity phase_entity(const World& world, PhaseId id) {
    switch (id) {
        case PhaseId::startup: return world.entity<phase::Startup>();
        case PhaseId::input: return world.entity<phase::Input>();
        case PhaseId::simulation: return world.entity<phase::Simulation>();
        case PhaseId::post_simulation: return world.entity<phase::PostSimulation>();
        case PhaseId::transform_propagation: return world.entity<phase::TransformPropagation>();
        case PhaseId::pre_extract: return world.entity<phase::PreExtract>();
        case PhaseId::extract: return world.entity<phase::Extract>();
        case PhaseId::render: return world.entity<phase::Render>();
        case PhaseId::shutdown: return world.entity<phase::Shutdown>();
    }
    VEACH_PANIC("invalid phase id {}", static_cast<int>(id));
}

void PhaseRunner::register_phases(World& world) {
    // Phase tags are deliberately *not* flecs::Phase entities, so Flecs's builtin
    // pipeline never picks up user systems. Each phase gets its own pipeline,
    // ordered by declaration order, and the engine runs them explicitly.
    for (const PhaseInfo& info : phase_table) {
        const Entity tag = phase_entity(world, info.id);
        const std::string pipeline_name = std::string("veach::pipeline::") + std::string(info.name);
        const auto pipeline = world.pipeline(pipeline_name.c_str())
                                  .with(flecs::System)
                                  .with(tag)
                                  .without(flecs::Disabled)
                                  .up(flecs::DependsOn)
                                  .without(flecs::Disabled)
                                  .up(flecs::ChildOf)
                                  .build();
        pipelines_.at(static_cast<std::size_t>(info.id)) = pipeline.id();
    }
}

void PhaseRunner::run(World& world, PhaseId id, float delta_seconds) const {
    VEACH_ZONE_TEXT("phase", phase_info(id).name.data(), phase_info(id).name.size());
    const ecs::entity_t pipeline = pipelines_.at(static_cast<std::size_t>(id));
    VEACH_ASSERT(pipeline != 0, "phases were not registered");
    world.run_pipeline(pipeline, delta_seconds);
}

}  // namespace veach
