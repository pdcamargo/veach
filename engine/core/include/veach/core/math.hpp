#pragma once

// veach::math — glm, plus the engine conventions (ADR 0013). Match glTF:
//   * right-handed, +Y up, cameras look down -Z, +X right
//   * meters, seconds, radians
//   * column-major matrices, column vectors (v' = M * v)
//   * reverse-Z depth: near plane -> 1, far/infinity -> 0, clip depth in [0, 1]
//
// GLM_FORCE_DEPTH_ZERO_TO_ONE is defined for every target (see engine/core/CMakeLists.txt);
// still, use the helpers below rather than glm::perspective, which is not reverse-Z.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace veach::math {

using namespace glm;  // NOLINT(google-build-using-namespace): veach::math *is* glm plus conventions.

inline constexpr vec3 world_up{0.0f, 1.0f, 0.0f};
inline constexpr vec3 world_right{1.0f, 0.0f, 0.0f};
/// The direction an unrotated camera (or any "forward"-facing object) looks.
inline constexpr vec3 world_forward{0.0f, 0.0f, -1.0f};

/// Right-handed, reverse-Z, infinite-far perspective projection.
/// A point at view-space z = -near maps to depth 1; z -> -infinity maps to depth 0.
[[nodiscard]] inline mat4 perspective_reverse_z(float fov_y_radians, float aspect, float near_plane) {
    const float f = 1.0f / glm::tan(fov_y_radians * 0.5f);
    mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;       // clip.w = -view.z
    m[3][2] = near_plane;  // clip.z = near
    return m;
}

}  // namespace veach::math
