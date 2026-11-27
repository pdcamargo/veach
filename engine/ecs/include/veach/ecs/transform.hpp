#pragma once

// Transforms and hierarchy (docs/specs/transforms/spec.md). An entity's
// Transform is its pose relative to its parent (ChildOf), or to the world at
// the root. TransformPropagation turns them into GlobalTransform, parents
// before children, once per fixed step and again just before extraction, so
// a pose written in any phase reaches the renderer in the same frame.

#include <veach/core/math.hpp>
#include <veach/ecs/flecs.hpp>

namespace veach {

/// Local pose, glTF-style: scale, then rotation, then translation (meters, radians; ADR 0013).
struct Transform {
    math::vec3 translation{0.0f};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  ///< identity; glm's constructor takes (w, x, y, z)
    math::vec3 scale{1.0f};

    /// Parent from local: translation * rotation * scale.
    [[nodiscard]] math::mat4 matrix() const {
        math::mat4 m = math::mat4_cast(math::normalize(rotation));
        m[0] *= scale.x;
        m[1] *= scale.y;
        m[2] *= scale.z;
        m[3] = math::vec4(translation, 1.0f);
        return m;
    }
};

/// World from local, computed by TransformPropagation. Added automatically with Transform; never write it.
struct GlobalTransform {
    math::mat4 matrix{1.0f};

    [[nodiscard]] math::vec3 translation() const { return math::vec3(matrix[3]); }
    /// The rotation, with scale removed from the basis.
    [[nodiscard]] math::quat rotation() const {
        return math::quat_cast(math::mat3(math::normalize(math::vec3(matrix[0])),
                                          math::normalize(math::vec3(matrix[1])),
                                          math::normalize(math::vec3(matrix[2]))));
    }
};

/// Runs the propagation now, outside the phases (tests, tools). The engine's systems do the same every frame.
void propagate_transforms(World& world);

}  // namespace veach
