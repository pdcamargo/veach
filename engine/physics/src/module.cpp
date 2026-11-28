// The ECS binding (veach/physics/module.hpp): the one file that knows both the World and the physics core. Bound to
// the frames of VCH-111 (ADR 0045, 0085): a PhysicsScene on a ReferenceFrame entity is a scene in that frame, and a
// body's Transform, relative to that frame, is its pose.

#include <veach/core/jobs.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/reflection.hpp>
#include <veach/ecs/services.hpp>
#include <veach/ecs/transform.hpp>
#include <veach/physics/module.hpp>

#include <format>

namespace veach {
namespace {

physics::Shape shape_of(const RigidBody& r) {
    switch (r.shape) {
        case BodyShape::box: return physics::Shape::box(r.half_extents);
        case BodyShape::sphere: return physics::Shape::sphere(r.radius);
        case BodyShape::capsule: return physics::Shape::capsule(r.half_height, r.radius);
    }
    return physics::Shape::box(r.half_extents);
}

/// The scene an entity's body goes to: its frame's PhysicsScene, or the default scene of the root world.
std::optional<physics::SceneId> scene_for(const World& world, Entity e) {
    const Entity parent = e.parent();
    const Entity frame = frame_of(world, e);
    if (parent && parent != frame) {
        log::error("physics", Error{ErrorCode::physics_invalid_transfer,
                                    std::format("'{}' has a RigidBody but its parent is not its reference frame",
                                                e.path("::", "").c_str()),
                                    "Make a rigid body a direct child of the frame entity whose PhysicsScene it "
                                    "belongs to (or of nothing, for the root world's scene)."});
        return std::nullopt;
    }
    if (!frame) {
        return default_scene;
    }
    if (const auto* id = frame.try_get<PhysicsSceneId>()) {
        return physics::SceneId{id->id};
    }
    log::error("physics", Error{ErrorCode::physics_invalid_transfer,
                                std::format("'{}' has a RigidBody in frame '{}', which has no PhysicsScene",
                                            e.path("::", "").c_str(), frame.path("::", "").c_str()),
                                "Set a PhysicsScene on the frame entity first."});
    return std::nullopt;
}

void remove_body(physics::PhysicsWorld& physics, Entity e) {
    if (const auto* b = e.try_get<PhysicsBody>()) {
        if (physics.contains(physics::BodyId{b->body})) {
            physics.remove_body(physics::BodyId{b->body});
        }
        e.remove<PhysicsBody>();
    }
}

void create_body(physics::PhysicsWorld& physics, Entity e, const RigidBody& r, const Transform& t) {
    remove_body(physics, e);
    const auto scene = scene_for(e.world(), e);
    if (!scene) {
        return;
    }
    physics::BodySettings s;
    s.shape = shape_of(r);
    s.motion = r.motion;
    s.position = t.translation - physics.origin_at(*scene);
    s.rotation = math::dquat(t.rotation);
    s.friction = r.friction;
    s.restitution = r.restitution;
    s.allow_sleeping = r.allow_sleeping;
    s.lock_rotation = r.lock_rotation;
    if (r.mass > 0.0) {
        s.mass = r.mass;
    }
    auto body = physics.add_body(*scene, s);
    if (!body) {
        log::error("physics", body.error());
        return;
    }
    e.set<PhysicsBody>({.scene = static_cast<std::uint32_t>(*scene), .body = static_cast<std::uint32_t>(*body)});
}

}  // namespace

PhysicsModule::PhysicsModule(World& world) {
    world.module<PhysicsModule>();
    physics::initialize();  // Jolt's process-wide state, once, at module start (ADR 0075)
    Services* services = find_services(world);
    JobSystem* jobs = services != nullptr ? services->find<JobSystem>() : nullptr;
    physics = std::make_shared<physics::PhysicsWorld>(physics::WorldSettings{.jobs = jobs});
    auto made = physics->create_scene({.name = "world"});
    VEACH_ASSERT(made.has_value() && *made == default_scene, "physics: the default scene must be scene 0");

    world.component<physics::SceneKind>();
    world.component<physics::MotionType>();
    world.component<BodyShape>();
    world.component<PhysicsScene>()
        .member(&PhysicsScene::kind, "kind")
        .member(&PhysicsScene::max_bodies, "max_bodies")
        .add<Scriptable>();
    world.component<PhysicsSceneId>().member(&PhysicsSceneId::id, "id").add<Scriptable>();
    world.component<RigidBody>()
        .member(&RigidBody::motion, "motion")
        .member(&RigidBody::shape, "shape")
        .member(&RigidBody::half_extents, "half_extents")
        .member(&RigidBody::radius, "radius")
        .member(&RigidBody::half_height, "half_height")
        .member(&RigidBody::mass, "mass")
        .member(&RigidBody::friction, "friction")
        .member(&RigidBody::restitution, "restitution")
        .member(&RigidBody::allow_sleeping, "allow_sleeping")
        .member(&RigidBody::lock_rotation, "lock_rotation")
        .add<Scriptable>();
    world.component<PhysicsBody>()
        .member(&PhysicsBody::scene, "scene")
        .member(&PhysicsBody::body, "body")
        .add<Scriptable>();

    std::shared_ptr<physics::PhysicsWorld> state = physics;
    world.observer<const PhysicsScene>("PhysicsSceneSet")
        .event(flecs::OnSet)
        .each([state](Entity e, const PhysicsScene& p) {
            if (e.has<PhysicsSceneId>()) {
                return;  // one scene per frame; its settings are fixed once made
            }
            auto scene = state->create_scene({.name = e.path("::", "").c_str(),
                                              .kind = p.kind,
                                              .frame = {.frame = e.id(), .angular_velocity = math::dvec3(0.0)},
                                              .max_bodies = p.max_bodies});
            if (!scene) {
                log::error("physics", scene.error());
                return;
            }
            e.set<PhysicsSceneId>({static_cast<std::uint32_t>(*scene)});
        });
    world.observer<const PhysicsScene>("PhysicsSceneRemove")
        .event(flecs::OnRemove)
        .each([state](Entity e, const PhysicsScene&) {
            if (const auto* id = e.try_get<PhysicsSceneId>()) {
                state->destroy_scene(physics::SceneId{id->id});
            }
        });
    world.observer<const RigidBody, const Transform>("RigidBodySet")
        .event(flecs::OnSet)
        .each([state](Entity e, const RigidBody& r, const Transform& t) { create_body(*state, e, r, t); });
    world.observer<const RigidBody>("RigidBodyRemove").event(flecs::OnRemove).each([state](Entity e, const RigidBody&) {
        remove_body(*state, e);
    });

    // The step, once per fixed step in PostSimulation (ADR 0058): after gameplay wrote forces in Simulation and after
    // the frame pose sources registered before this module (CoreModule's FrameSpin), before TransformPropagation.
    // Scenes take their frame's spin for the fictitious forces of free flight; dynamic and kinematic bodies' poses
    // go back to their Transforms, for the end of the step.
    world.system<const PhysicsBody, Transform>("PhysicsStep")
        .kind(phase_entity(world, PhaseId::post_simulation))
        .run([state](Iter& it) {
            VEACH_ZONE("PhysicsStep");
            const World w = it.world();
            w.each([&](const PhysicsSceneId& id, const ReferenceFrame& frame) {
                // The frame's angular velocity is in its parent's axes; the scene works in the frame's own.
                const math::dvec3 own = frame.kind == FrameKind::rotating
                                            ? math::inverse(math::normalize(frame.orientation)) * frame.angular_velocity
                                            : math::dvec3(0.0);
                const physics::SceneId scene{id.id};
                physics::SceneFrame f = state->scene_info(scene).frame;
                if (f.angular_velocity != own) {
                    f.angular_velocity = own;
                    state->set_frame(scene, f);
                }
            });
            state->step();
            while (it.next()) {
                const auto bodies = it.field<const PhysicsBody>(0);
                const auto transforms = it.field<Transform>(1);
                for (const auto i : it) {
                    const physics::BodyId body{bodies[i].body};
                    if (!state->contains(body) || state->settings(body).motion == physics::MotionType::static_body) {
                        continue;
                    }
                    const physics::BodyState s = state->state(body);
                    transforms[i].translation = state->origin_at(state->scene_of(body)) + s.position;
                    transforms[i].rotation = s.rotation;
                }
            }
        });
}

physics::PhysicsWorld* find_physics_world(const World& world) {
    const auto* module = world.try_get<PhysicsModule>();
    return module != nullptr ? module->physics.get() : nullptr;
}

physics::PhysicsWorld& physics_world(const World& world) {
    physics::PhysicsWorld* p = find_physics_world(world);
    VEACH_ASSERT(p != nullptr, "physics_world: import veach::PhysicsModule first");
    return *p;
}

}  // namespace veach
