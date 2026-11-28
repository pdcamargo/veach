#pragma once

// Transforms and hierarchy (docs/specs/transforms/spec.md, docs/specs/frames/spec.md; ADR 0045). An entity's
// Transform is its pose relative to its parent (ChildOf), or to its reference frame at the top of a chain. The
// translation is double: positions are doubles relative to a frame. TransformPropagation turns Transforms into
// FrameTransforms, the pose in the entity's frame (its nearest ReferenceFrame ancestor, or the root world), parents
// before children, once per fixed step and again just before extraction, so a pose written in any phase reaches
// the renderer in the same frame. Propagation stops at frames: there is no world pose, only poses in frames.

#include <veach/core/math.hpp>
#include <veach/ecs/flecs.hpp>

namespace veach {

/// Local pose, glTF-style: scale, then rotation, then translation (meters, radians; ADR 0013, ADR 0045).
struct Transform {
    math::dvec3 translation{0.0};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  ///< identity; glm's constructor takes (w, x, y, z)
    math::vec3 scale{1.0f};

    /// Rotation x scale: the float32 3x3 part of parent from local.
    [[nodiscard]] math::mat3 basis() const {
        math::mat3 m = math::mat3_cast(math::normalize(rotation));
        m[0] *= scale.x;
        m[1] *= scale.y;
        m[2] *= scale.z;
        return m;
    }
};

/// The pose in the entity's reference frame: frame from local, computed by TransformPropagation. Added automatically
/// with Transform; never write it. It replaces a world pose (ADR 0045): to compare entities in different frames, go
/// through veach::frame_pose (veach/ecs/frames.hpp).
struct FrameTransform {
    math::dvec3 translation{0.0};  ///< meters, in the frame
    math::mat3 basis{1.0f};        ///< rotation x scale, float32
    ecs::entity_t frame = 0;       ///< the ReferenceFrame entity this pose is in; 0 is the root world

    /// The rotation, with scale removed from the basis.
    [[nodiscard]] math::quat rotation() const {
        return math::quat_cast(
            math::mat3(math::normalize(basis[0]), math::normalize(basis[1]), math::normalize(basis[2])));
    }
    /// A point given in the entity's local space, in the frame (double).
    [[nodiscard]] math::dvec3 point(const math::dvec3& local) const { return translation + math::dmat3(basis) * local; }
};

/// Runs the frame pass and the propagation now, outside the phases (tests, tools). The engine's systems do the same
/// every frame.
void propagate_transforms(World& world);

}  // namespace veach
