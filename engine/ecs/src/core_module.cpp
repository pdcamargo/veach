#include <veach/ecs/components.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/reflection.hpp>

namespace veach {

namespace {

void compose_global(const Transform& local, const GlobalTransform* parent, GlobalTransform& global) {
    global.matrix = parent ? parent->matrix * local.matrix() : local.matrix();
}

}  // namespace

void propagate_transforms(World& world) {
    // A cached query (cascade needs one) is an entity in the world: free it, or every call leaves one behind.
    auto query = world.query_builder<const Transform, const GlobalTransform*, GlobalTransform>()
                     .term_at(1)
                     .parent()
                     .cascade()
                     .build();
    query.each(compose_global);
    query.destruct();
}

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
    world.component<GlobalTransform>().member(&GlobalTransform::matrix, "matrix");
    world.component<Transform>()
        .member(&Transform::translation, "translation")
        .member(&Transform::rotation, "rotation")
        .member(&Transform::scale, "scale")
        .add(flecs::With, world.id<GlobalTransform>())  // every Transform gets a GlobalTransform
        .add<Scriptable>();

    // Parents before children (cascade over ChildOf). Per fixed step for gameplay, and again before
    // extraction so poses written in per-frame phases reach the renderer the same frame.
    for (const PhaseId phase : {PhaseId::transform_propagation, PhaseId::pre_extract}) {
        world
            .system<const Transform, const GlobalTransform*, GlobalTransform>(
                phase == PhaseId::pre_extract ? "TransformPropagationBeforeExtract" : "TransformPropagation")
            .kind(phase_entity(world, phase))
            .term_at(1)
            .parent()
            .cascade()
            .each(compose_global);
    }
    world.component<Random>().member(&Random::seed, "seed").member(&Random::rng, "rng").add(flecs::Singleton);
}

}  // namespace veach
