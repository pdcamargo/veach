#include <veach/ecs/components.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/reflection.hpp>

#include <cstddef>

namespace veach {

namespace {

/// Terms: 0 Transform, 1 the nearest ancestor's FrameTransform (cascade), 2 out, 3 the nearest ancestor that is a
/// ReferenceFrame. Both look up through ancestors without them, and every entity of a table shares its parent, so
/// the frame is resolved once per table.
void propagate(Iter& it) {
    while (it.next()) {
        const auto local = it.field<const Transform>(0);
        auto out = it.field<FrameTransform>(2);
        // The entity's frame is its nearest frame ancestor. It composes with the nearest ancestor pose only if that
        // pose is inside the same frame: propagation stops at frames, so a frame's children start from the identity.
        const ecs::entity_t frame = it.is_set(3) ? it.src(3).id() : 0;
        const FrameTransform* parent = it.is_set(1) ? &it.field<const FrameTransform>(1)[0] : nullptr;
        if (parent != nullptr && parent->frame != frame) {
            parent = nullptr;
        }
        for (const auto i : it) {
            FrameTransform& o = out[i];
            const math::mat3 basis = local[i].basis();
            if (parent != nullptr) {
                // The basis in float32, as the float32 mat4 product computed it; the translation in double.
                o.basis = parent->basis * basis;
                o.translation = parent->translation + math::dmat3(parent->basis) * local[i].translation;
            } else {
                o.basis = basis;
                o.translation = local[i].translation;
            }
            o.frame = frame;
        }
    }
}

template <class Builder>
Builder& propagation_terms(Builder& builder) {
    builder.term_at(1)
        .parent()
        .cascade()
        .template with<ReferenceFrame>()
        .parent()
        .optional()
        .template without<ReferenceFrame>();  // frames carry no Transform; if one does, it is not propagated
    return builder;
}

}  // namespace

void propagate_transforms(World& world) {
    update_frames(world);
    // A cached query (cascade needs one) is an entity in the world: free it, or every call leaves one behind.
    auto builder = world.query_builder<const Transform, const FrameTransform*, FrameTransform>();
    auto query = propagation_terms(builder).build();
    query.run(propagate);
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
    world.component<FrameTransform>()
        .member(&FrameTransform::translation, "translation")
        .member(&FrameTransform::basis, "basis")
        .member(flecs::Entity, "frame", 1, offsetof(FrameTransform, frame));
    world.component<Transform>()
        .member(&Transform::translation, "translation")
        .member(&Transform::rotation, "rotation")
        .member(&Transform::scale, "scale")
        .add(flecs::With, world.id<FrameTransform>())  // every Transform gets a FrameTransform
        .add<Scriptable>();
    register_frames(world);

    // The frame pass, then parents before children (cascade over ChildOf). Per fixed step for gameplay, and again
    // before extraction so poses written in per-frame phases reach the renderer the same frame.
    for (const PhaseId phase : {PhaseId::transform_propagation, PhaseId::pre_extract}) {
        const bool pre_extract = phase == PhaseId::pre_extract;
        world.system(pre_extract ? "FramePassBeforeExtract" : "FramePass")
            .kind(phase_entity(world, phase))
            .run([](Iter& it) {
                World w = it.world();
                update_frames(w);
            });
        auto system = world.system<const Transform, const FrameTransform*, FrameTransform>(
            pre_extract ? "TransformPropagationBeforeExtract" : "TransformPropagation");
        propagation_terms(system).kind(phase_entity(world, phase)).run(propagate);
    }
    world.component<Random>().member(&Random::seed, "seed").member(&Random::rng, "rng").add(flecs::Singleton);
}

}  // namespace veach
