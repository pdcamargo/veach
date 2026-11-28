#pragma once

// Reference frames (docs/specs/frames/spec.md, ADR 0045). A frame is an entity with ReferenceFrame: its pose,
// velocity and spin relative to its parent frame, in the parent's axes. Ordinary entities live in their nearest frame
// ancestor (ChildOf), and their FrameTransform is relative to it. The root world, frame 0, is flat and inertial: a
// world without frames is exactly the single-frame world of before.
//
//   system barycenter (inertial) -> planet orbit frame (inertial) -> body-fixed frame (rotating, radial up) -> base
//
// Pose sources write ReferenceFrame in PostSimulation or earlier, for the end of the step (Time.elapsed +
// Time.delta), from the tick: orbits and spins (FrameSpin, or the toolbox's orrery), gameplay for ships. The frame
// pass refreshes FrameLink once per tick; composition between any two frames goes through their lowest common
// ancestor, in double. Rendering is camera-relative (engine/render); nothing here depends on a camera.

#include <veach/core/error.hpp>
#include <veach/core/math.hpp>
#include <veach/ecs/flecs.hpp>

#include <cstdint>

namespace veach {

enum class FrameKind : std::uint8_t {
    inertial,  ///< its axes don't turn: angular_velocity is ignored
    rotating,  ///< its axes turn at angular_velocity (a body-fixed frame, a spinning station)
};

/// What "up" means in the frame (veach::up).
enum class FrameUp : std::uint8_t {
    axis_y,  ///< the frame's +Y everywhere (ships, stations, flat worlds)
    radial,  ///< away from the frame's origin (a body-centered frame)
};

/// A reference frame, relative to its parent frame (its nearest ReferenceFrame ancestor, or the root world), in the
/// parent's axes. A frame entity has no Transform: this is its pose. Scriptable.
struct ReferenceFrame {
    FrameKind kind = FrameKind::inertial;
    FrameUp up = FrameUp::axis_y;
    math::dvec3 position{0.0};                    ///< the frame's origin, m
    math::dquat orientation{1.0, 0.0, 0.0, 0.0};  ///< the frame's axes (normalized on use)
    math::dvec3 linear_velocity{0.0};             ///< of the origin, m/s
    math::dvec3 angular_velocity{0.0};            ///< rad/s; rotating frames only
};

/// A pose source for a rotating frame: it turns about its own +Y at a constant rate, from exact tick time (never
/// integrated): orientation = tilt * rotation(+Y, phase + rate * t). Written into ReferenceFrame in PostSimulation for
/// the end of the step, and when set. Needs a ReferenceFrame (added with it), which it makes `rotating`. Scriptable.
struct FrameSpin {
    math::dquat tilt{1.0, 0.0, 0.0, 0.0};  ///< the spin axis's orientation in the parent frame
    double rate = 0.0;                     ///< rad/s, right-handed about +Y
    double phase = 0.0;                    ///< rad, at t = 0
};

/// The frame tree, refreshed by the frame pass once per tick (TransformPropagation, PreExtract). Never write it.
struct FrameLink {
    ecs::entity_t parent = 0;   ///< the parent frame; 0 is the root world
    std::uint32_t depth = 0;    ///< 1 for a frame directly in the root world
    math::dmat3 rotation{1.0};  ///< ReferenceFrame::orientation as a matrix
};

/// The pose of one frame in another: x_to = position + rotation x_from, and for velocities
/// v_to = velocity + angular_velocity × (rotation x_from) + rotation v_from. All in the target frame's axes.
struct FramePose {
    math::dvec3 position{0.0};
    math::dmat3 rotation{1.0};
    math::dvec3 velocity{0.0};
    math::dvec3 angular_velocity{0.0};

    [[nodiscard]] math::dvec3 point(const math::dvec3& p) const { return position + rotation * p; }
    [[nodiscard]] math::dvec3 direction(const math::dvec3& d) const { return rotation * d; }
    /// The velocity, in the target frame, of something at `p` moving at `v`, both in the source frame.
    [[nodiscard]] math::dvec3 velocity_of(const math::dvec3& p, const math::dvec3& v) const {
        return velocity + math::cross(angular_velocity, rotation * p) + rotation * v;
    }
    /// The pose of the target frame in the source frame.
    [[nodiscard]] FramePose inverse() const;
};

/// outer ∘ inner: the pose of C in A from the pose of B in A (outer) and of C in B (inner).
[[nodiscard]] FramePose compose(const FramePose& outer, const FramePose& inner);

/// The pose of `frame` in its parent frame, from its ReferenceFrame.
[[nodiscard]] FramePose local_pose(const ReferenceFrame& frame);

/// Registers the frame components and systems. Part of CoreModule.
void register_frames(World& world);
/// The frame pass (FrameLink for every frame). Part of propagate_transforms and of the engine's propagation systems.
void update_frames(World& world);

/// The frame `entity` lives in: for an ordinary entity its nearest ReferenceFrame ancestor, for a frame its parent
/// frame. A null Entity is the root world.
[[nodiscard]] Entity frame_of(const World& world, Entity entity);

/// The pose of frame `from` in frame `to` (null Entity: the root world), composed in double through their lowest
/// common ancestor. Fails with ecs.not_a_frame if either is neither null nor a live ReferenceFrame.
[[nodiscard]] Result<FramePose> frame_pose(const World& world, Entity from, Entity to);

/// "Up" at `position` in `frame`: its +Y in the root world and axis_y frames (math::world_up), the direction away
/// from its origin in radial frames (+Y at the origin itself). Unit length.
[[nodiscard]] math::dvec3 up(const World& world, Entity frame, const math::dvec3& position);

/// Where `entity` is (its FrameTransform translation; a frame's origin) relative to `from`'s position, in the frame
/// `from` lives in, computed in double. Fails with ecs.not_a_frame for a dead entity.
[[nodiscard]] Result<math::dvec3> offset(const World& world, Entity from, Entity entity);

/// Hands `entity` to another frame: re-parents it directly under `to_frame` (a null Entity: the root world) and
/// rewrites its Transform so that it does not move, converting in double. With `velocity` (the entity's velocity in
/// its current frame), rewrites it to the new frame: in a rotating frame v_rot = v_inertial − ω × r. Uses the
/// entity's FrameTransform, so run it after propagation of the current state (in Simulation, it is the state at the
/// start of the step). The mechanism only: when to hand off is the caller's policy. Fails with ecs.not_a_frame
/// (to_frame is not a frame), ecs.unknown_entity (entity dead or without Transform) or ecs.handoff_name_conflict.
[[nodiscard]] Result<> handoff(World& world, Entity entity, Entity to_frame, math::dvec3* velocity = nullptr);

}  // namespace veach
