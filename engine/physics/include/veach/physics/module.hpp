#pragma once

// The ECS binding of veach::physics (docs/specs/physics/spec.md): components that hold parameters and ids, and the
// PostSimulation step. Physics state lives in the module's PhysicsWorld, never in components. Everything ECS-facing
// is in src/module.cpp, so the binding can follow the frames API without touching the physics core.
//
//   engine.import<veach::PhysicsModule>();
//   physics_world(world).set_gravity(...);                 // the gravity hook, from gameplay or the toolbox
//   auto planet = world.entity("planet").set(ReferenceFrame{.kind = FrameKind::rotating, .up = FrameUp::radial});
//   planet.set(PhysicsScene{.kind = physics::SceneKind::surface});      // a scene in that frame
//   world.entity("crate").child_of(planet).set(Transform{.translation = {0.0, 6.371e6 + 1.0, 0.0}})
//        .set(RigidBody{.shape = BodyShape::box, .half_extents = {0.5f, 0.5f, 0.5f}, .mass = 500.0});
//
// A RigidBody entity is a direct child of a frame entity with a PhysicsScene (or of nothing: the root world's
// default scene); its Transform is its pose in that frame. Each fixed step, in PostSimulation after the frame pose
// sources (FrameSpin), the module steps the world and writes the pose of every dynamic and kinematic body back to
// its Transform, for the end of the step, as ADR 0085 asks of pose writers.

#include <veach/ecs/flecs.hpp>
#include <veach/physics/physics.hpp>

#include <cstdint>
#include <memory>

namespace veach {

/// A physics scene in the frame of the entity it is on (a ReferenceFrame entity). Scriptable.
struct PhysicsScene {
    physics::SceneKind kind = physics::SceneKind::surface;
    std::uint32_t max_bodies = 1024;
};

/// The scene the module made for a PhysicsScene entity. Written by the module; never write it.
struct PhysicsSceneId {
    std::uint32_t id = 0;
};

enum class BodyShape : std::uint8_t { box, sphere, capsule };

/// A rigid body's parameters. Scriptable. Setting it again rebuilds the body at the entity's Transform.
struct RigidBody {
    physics::MotionType motion = physics::MotionType::dynamic;
    BodyShape shape = BodyShape::box;
    math::vec3 half_extents{0.5f};  ///< box
    float radius = 0.5f;            ///< sphere, capsule
    float half_height = 0.5f;       ///< capsule: half the cylinder's height
    double mass = 0.0;              ///< kg; 0 computes it from the shape (density 1000 kg/m³)
    float friction = 0.2f;
    float restitution = 0.0f;
    bool allow_sleeping = true;
    bool lock_rotation = false;
};

/// The body the module made for a RigidBody. Written by the module; never write it.
struct PhysicsBody {
    std::uint32_t scene = 0;
    std::uint32_t body = 0;
};

struct PhysicsModule {
    explicit PhysicsModule(World& world);

    std::shared_ptr<physics::PhysicsWorld> physics;
};

/// The PhysicsModule's world. Panics if the module was not imported.
[[nodiscard]] physics::PhysicsWorld& physics_world(const World& world);
/// Null if the module was not imported.
[[nodiscard]] physics::PhysicsWorld* find_physics_world(const World& world);
/// The default scene of the root world, made at import.
inline constexpr physics::SceneId default_scene{0};

}  // namespace veach
