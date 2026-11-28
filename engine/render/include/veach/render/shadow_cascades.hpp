#pragma once

// The geometry of the real-time renderer's sun shadows (docs/specs/sun-shadows/spec.md, ADR 0095): which lights cast,
// where the cascades end, and each cascade's fit around a slice of the camera's frustum, snapped to its texels in the
// render frame so that static shadows stay put while the camera and render space move. The renderer
// (engine/render/src/shadows.cpp) culls and draws with them; they are public for tests and tools.

#include <veach/core/math.hpp>
#include <veach/render/renderer.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace veach::render {

inline constexpr std::uint32_t max_shadowed_lights = 2;  ///< ADR 0062: the two brightest suns
inline constexpr std::uint32_t max_shadow_cascades = 4;

/// Where each cascade ends, meters from the camera along its view: the last at `distance`, each one before at a
/// quarter of the next (1 to 4 cascades).
[[nodiscard]] std::array<float, max_shadow_cascades> cascade_splits(float distance, std::uint32_t count);

/// One cascade fitted around a slice of a view's frustum.
struct CascadeFit {
    math::mat4 clip_from_render{1.0f};
    std::array<math::vec4, 5> planes{};  ///< culling: its four sides and its far side (away from the light)
    math::vec3 center{0.0f};             ///< render space, snapped
    float radius = 0.0f;                 ///< meters: half the cascade's width
    float texel = 0.0f;                  ///< meters per texel
    float depth_range = 0.0f;            ///< meters per unit of depth
};

/// The cascade covering view depths [near, far] of `view` for a light toward `l` (unit, toward the light): the
/// slice's bounding sphere, whose size does not change as the camera turns, centered on a whole texel of a grid fixed
/// in the render frame (`origin` is render space's origin there), so static shadows do not shimmer as the camera
/// moves or as render space moves to another cell. Its depth reaches twice the radius toward the light, or
/// `toward_light` meters if that is farther (casters beyond are flattened onto its near plane).
[[nodiscard]] CascadeFit fit_cascade(const RenderView& view, const math::dvec3& origin, math::vec3 l, float near,
                                     float far, std::uint32_t resolution, float toward_light = 0.0f);

/// The planes (inside where dot(xyz, p) + w >= 0) of the slice of `view`'s frustum between view depths `near` and
/// `far`, each moved out by `margin` meters: what a cascade's shadows must fall in to be seen.
[[nodiscard]] std::array<math::vec4, 6> receiver_planes(const RenderView& view, float near, float far, float margin);

/// Indices into `lights` of the directional lights that cast shadow maps: the brightest by luminance, at most `max`.
[[nodiscard]] std::vector<std::uint32_t> shadowed_lights(std::span<const RenderLight> lights, std::uint32_t max);

}  // namespace veach::render
