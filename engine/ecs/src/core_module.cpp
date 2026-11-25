#include <veach/ecs/components.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/reflection.hpp>

namespace veach {

CoreModule::CoreModule(World& world) {
    world.module<CoreModule>("veach::core");

    register_builtin_reflection(world);

    // Phase tags live under veach.phase so tools can list them.
    for (const PhaseInfo& info : phases()) {
        static_cast<void>(phase_entity(world, info.id));
    }

    world.component<Scriptable>();
    world.component<Optional>();

    world.component<Time>()
        .member(&Time::frame, "frame")
        .member(&Time::fixed_step, "fixed_step")
        .member(&Time::delta, "delta")
        .member(&Time::elapsed, "elapsed")
        .add(flecs::Singleton);

    world.component<Rng>().member(&Rng::state, "state").member(&Rng::increment, "increment");
    world.component<Transform>()
        .member(&Transform::translation, "translation")
        .member(&Transform::rotation, "rotation")
        .member(&Transform::scale, "scale")
        .add<Scriptable>();
    world.component<Random>().member(&Random::seed, "seed").member(&Random::rng, "rng").add(flecs::Singleton);
}

}  // namespace veach
