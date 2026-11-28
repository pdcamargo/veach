#include <veach/ecs/frames.hpp>

#include <veach/core/dmath.hpp>
#include <veach/core/log.hpp>
#include <veach/core/time.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/phases.hpp>

#include <cstddef>
#include <format>
#include <utility>

namespace veach {

namespace {

constexpr math::dvec3 y_axis{0.0, 1.0, 0.0};

bool has_frame(const World& world, ecs::entity_t e) {
    return ecs_has_id(world.c_ptr(), e, world.id<ReferenceFrame>());
}

bool is_frame(const World& world, ecs::entity_t e) {
    return e != 0 && ecs_is_alive(world.c_ptr(), e) && has_frame(world, e);
}

ecs::entity_t parent_of(const World& world, ecs::entity_t e) {
    return ecs_get_target(world.c_ptr(), e, EcsChildOf, 0);
}

/// The nearest ReferenceFrame strictly above `e`, walking ChildOf.
ecs::entity_t frame_above(const World& world, ecs::entity_t e) {
    for (ecs::entity_t p = parent_of(world, e); p != 0; p = parent_of(world, p)) {
        if (has_frame(world, p)) {
            return p;
        }
    }
    return 0;
}

/// The frame's parent frame and depth: from FrameLink when the frame pass has linked it, else walked live.
FrameLink link_of(const World& world, ecs::entity_t frame) {
    if (const auto* link = static_cast<const FrameLink*>(ecs_get_id(world.c_ptr(), frame, world.id<FrameLink>()));
        link != nullptr && link->depth != 0) {
        return *link;
    }
    FrameLink live{.parent = frame_above(world, frame), .depth = 1};
    for (ecs::entity_t p = live.parent; p != 0; p = frame_above(world, p)) {
        ++live.depth;
    }
    return live;
}

FramePose pose_in_parent(const World& world, ecs::entity_t frame) {
    return local_pose(
        *static_cast<const ReferenceFrame*>(ecs_get_id(world.c_ptr(), frame, world.id<ReferenceFrame>())));
}

std::unexpected<Error> not_a_frame(const World& world, ecs::entity_t e) {
    const char* name = e != 0 && ecs_is_alive(world.c_ptr(), e) ? ecs_get_name(world.c_ptr(), e) : nullptr;
    return make_error(ErrorCode::ecs_not_a_frame,
                      std::format("entity {} ('{}') is not a reference frame", e, name ? name : "?"),
                      "Pass an entity with ReferenceFrame, or nil (a null Entity) for the root world; "
                      "frame_of(entity) gives the frame an entity lives in.");
}

/// The pose at time t (seconds) of a FrameSpin frame: orientation and angular velocity.
void spin(const FrameSpin& s, double t, ReferenceFrame& out) {
    const double half = 0.5 * (s.phase + s.rate * t);
    const math::dquat tilt = math::normalize(s.tilt);
    out.kind = FrameKind::rotating;
    out.orientation = tilt * math::dquat(dmath::cos(half), 0.0, dmath::sin(half), 0.0);
    out.angular_velocity = tilt * math::dvec3(0.0, s.rate, 0.0);
}

/// Simulated time of the world's current state: the start of the step being simulated.
double now(const World& world) {
    const Time* time = world.try_get<Time>();
    return time ? static_cast<double>(time->fixed_step) * fixed_delta_seconds : 0.0;
}

}  // namespace

FramePose compose(const FramePose& outer, const FramePose& inner) {
    const math::dvec3 offset = outer.rotation * inner.position;
    return {.position = outer.position + offset,
            .rotation = outer.rotation * inner.rotation,
            .velocity = outer.velocity + math::cross(outer.angular_velocity, offset) + outer.rotation * inner.velocity,
            .angular_velocity = outer.angular_velocity + outer.rotation * inner.angular_velocity};
}

FramePose FramePose::inverse() const {
    const math::dmat3 rt = math::transpose(rotation);
    const math::dvec3 p = rt * position;
    const math::dvec3 w = rt * angular_velocity;
    return {.position = -p, .rotation = rt, .velocity = -(rt * velocity) + math::cross(w, p), .angular_velocity = -w};
}

FramePose local_pose(const ReferenceFrame& frame) {
    return {.position = frame.position,
            .rotation = math::mat3_cast(math::normalize(frame.orientation)),
            .velocity = frame.linear_velocity,
            .angular_velocity = frame.kind == FrameKind::rotating ? frame.angular_velocity : math::dvec3(0.0)};
}

void register_frames(World& world) {
    world.component<FrameKind>();
    world.component<FrameUp>();
    world.component<FrameLink>()
        .member(flecs::Entity, "parent", 1, offsetof(FrameLink, parent))
        .member(&FrameLink::depth, "depth");
    world.component<ReferenceFrame>()
        .member(&ReferenceFrame::kind, "kind")
        .member(&ReferenceFrame::up, "up")
        .member(&ReferenceFrame::position, "position")
        .member(&ReferenceFrame::orientation, "orientation")
        .member(&ReferenceFrame::linear_velocity, "linear_velocity")
        .member(&ReferenceFrame::angular_velocity, "angular_velocity")
        .add(flecs::With, world.id<FrameLink>())
        .add<Scriptable>();
    world.component<FrameSpin>()
        .member(&FrameSpin::tilt, "tilt")
        .member(&FrameSpin::rate, "rate")
        .member(&FrameSpin::phase, "phase")
        .add(flecs::With, world.id<ReferenceFrame>())
        .add<Scriptable>();

    // A frame's pose is its ReferenceFrame; a Transform on it would be ignored, so say so once, when it happens.
    world.observer<const ReferenceFrame, const Transform>("FrameHasTransform")
        .event(flecs::OnAdd)
        .each([](Entity e, const ReferenceFrame&, const Transform&) {
            log::error("ecs", Error{ErrorCode::ecs_frame_has_transform,
                                    std::format("'{}' has both ReferenceFrame and Transform; the Transform is ignored",
                                                e.path("::", "").c_str()),
                                    "Move a frame by writing its ReferenceFrame, and put what it carries (a hull, "
                                    "a mesh) on child entities."});
        });

    // FrameSpin, the engine's pose source: when either is set, for the current state (it owns orientation and angular
    // velocity), then each step for the step's end.
    world.observer<const FrameSpin, ReferenceFrame>("FrameSpinSet")
        .event(flecs::OnSet)
        .each(
            [](Iter& it, std::size_t, const FrameSpin& s, ReferenceFrame& frame) { spin(s, now(it.world()), frame); });
    world.system<const FrameSpin, ReferenceFrame>("FrameSpin")
        .kind(phase_entity(world, PhaseId::post_simulation))
        .each([](Iter& it, std::size_t, const FrameSpin& s, ReferenceFrame& frame) {
            spin(s, now(it.world()) + fixed_delta_seconds, frame);
        });
}

void update_frames(World& world) {
    world.each([&](Entity e, const ReferenceFrame&, FrameLink& link) {
        link.depth = 0;
        link = link_of(world, e.id());
    });
}

Entity frame_of(const World& world, Entity entity) {
    if (!entity || !entity.is_alive()) {
        return Entity{};
    }
    return world.entity(frame_above(world, entity.id()));
}

Result<FramePose> frame_pose(const World& world, Entity from, Entity to) {
    const ecs::entity_t a0 = from ? from.id() : 0;
    const ecs::entity_t b0 = to ? to.id() : 0;
    if (a0 != 0 && !is_frame(world, a0)) {
        return not_a_frame(world, a0);
    }
    if (b0 != 0 && !is_frame(world, b0)) {
        return not_a_frame(world, b0);
    }
    // Climb both sides to their lowest common ancestor, accumulating each frame's pose in the current node.
    ecs::entity_t a = a0;
    ecs::entity_t b = b0;
    FrameLink la = a ? link_of(world, a) : FrameLink{};
    FrameLink lb = b ? link_of(world, b) : FrameLink{};
    FramePose in_a;  // `from` in node a
    FramePose in_b;  // `to` in node b
    const auto up_a = [&] {
        in_a = compose(pose_in_parent(world, a), in_a);
        a = la.parent;
        la = a ? link_of(world, a) : FrameLink{};
    };
    const auto up_b = [&] {
        in_b = compose(pose_in_parent(world, b), in_b);
        b = lb.parent;
        lb = b ? link_of(world, b) : FrameLink{};
    };
    while (la.depth > lb.depth) {
        up_a();
    }
    while (lb.depth > la.depth) {
        up_b();
    }
    while (a != b) {
        up_a();
        up_b();
    }
    // `to` from `from` through the ancestor: subtract positions there first, then rotate, so two frames far from
    // the ancestor but near each other keep their relative precision.
    const math::dmat3 rt = math::transpose(in_b.rotation);
    const math::dvec3 d = in_a.position - in_b.position;
    return FramePose{.position = rt * d,
                     .rotation = rt * in_a.rotation,
                     .velocity = rt * (in_a.velocity - in_b.velocity - math::cross(in_b.angular_velocity, d)),
                     .angular_velocity = rt * (in_a.angular_velocity - in_b.angular_velocity)};
}

math::dvec3 up(const World& world, Entity frame, const math::dvec3& position) {
    if (!frame || !is_frame(world, frame.id())) {
        return y_axis;
    }
    if (frame.get<ReferenceFrame>().up == FrameUp::radial) {
        const double length = math::length(position);
        return length > 0.0 ? position / length : y_axis;
    }
    return y_axis;
}

Result<math::dvec3> offset(const World& world, Entity from, Entity entity) {
    // Where an entity is: an ordinary one at its FrameTransform in its frame, a frame at its origin in its parent.
    const auto locate = [&](Entity e) -> Result<std::pair<Entity, math::dvec3>> {
        if (!e || !e.is_alive()) {
            return make_error(ErrorCode::ecs_unknown_entity, "offset: the entity is not alive");
        }
        if (const ReferenceFrame* frame = e.try_get<ReferenceFrame>()) {
            return std::pair{frame_of(world, e), frame->position};
        }
        const FrameTransform* ft = e.try_get<FrameTransform>();
        return std::pair{frame_of(world, e), ft ? ft->translation : math::dvec3(0.0)};
    };
    const auto a = locate(from);
    if (!a) {
        return std::unexpected(a.error());
    }
    const auto b = locate(entity);
    if (!b) {
        return std::unexpected(b.error());
    }
    const auto pose = frame_pose(world, b->first, a->first);
    if (!pose) {
        return std::unexpected(pose.error());
    }
    return pose->point(b->second) - a->second;
}

Result<> handoff(World& world, Entity entity, Entity to_frame, math::dvec3* velocity) {
    if (!entity || !entity.is_alive() || !entity.has<Transform>()) {
        return make_error(ErrorCode::ecs_unknown_entity, "handoff: the entity is not alive or has no Transform",
                          "Hand off an entity with a Transform (frames move by their ReferenceFrame).");
    }
    const ecs::entity_t target = to_frame ? to_frame.id() : 0;
    if (target != 0 && !is_frame(world, target)) {
        return not_a_frame(world, target);
    }
    const Entity from = frame_of(world, entity);
    auto pose = frame_pose(world, from, to_frame);
    if (!pose) {
        return std::unexpected(pose.error());
    }
    const char* name = ecs_get_name(world.c_ptr(), entity.id());
    if (name != nullptr) {
        const ecs::entity_t sibling =
            target != 0 ? ecs_lookup_child(world.c_ptr(), target, name) : ecs_lookup_child(world.c_ptr(), 0, name);
        if (sibling != 0 && sibling != entity.id()) {
            return make_error(ErrorCode::ecs_handoff_name_conflict,
                              std::format("handoff: the target frame already has a child named '{}'", name),
                              "Rename one of them first: names are unique among siblings.");
        }
    }

    // The pose in the old frame: the Transform itself when the entity sits directly in it (exact), else the
    // propagated FrameTransform, split into rotation and scale.
    const Transform& local = entity.get<Transform>();
    math::dvec3 position = local.translation;
    math::dquat rotation = math::dquat(local.rotation);
    math::vec3 scale = local.scale;
    const ecs::entity_t parent = parent_of(world, entity.id());
    if (parent != (from ? from.id() : 0)) {
        const FrameTransform& ft = entity.get<FrameTransform>();
        position = ft.translation;
        scale = math::vec3(math::length(ft.basis[0]), math::length(ft.basis[1]), math::length(ft.basis[2]));
        rotation = math::dquat(ft.rotation());
    }
    if (velocity != nullptr) {
        *velocity = pose->velocity_of(position, *velocity);
    }
    Transform moved;
    moved.translation = pose->point(position);
    moved.rotation = math::quat(math::normalize(math::quat_cast(pose->rotation) * rotation));
    moved.scale = scale;

    if (target != 0) {
        entity.child_of(to_frame);
    } else if (parent != 0) {
        entity.remove(flecs::ChildOf, flecs::Wildcard);
    }
    entity.set(moved);
    entity.set(FrameTransform{.translation = moved.translation, .basis = moved.basis(), .frame = target});
    return {};
}

}  // namespace veach
