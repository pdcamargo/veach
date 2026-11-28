// Spike SP-14 (VCH-090), throwaway: float32 reverse-Z depth with an infinite far plane, from a cockpit to 1 AU
// (docs/research/spikes/sp-14-exposure-depth.md). The projection is the engine's (math::perspective_reverse_z) and the
// arithmetic is float32, as on the GPU.
#include <veach/core/math.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>

using namespace veach;

namespace {

/// The depth a point straight ahead at `distance` stores: clip = P * (0, 0, -distance, 1), depth = clip.z / clip.w.
float depth_of(const math::mat4& projection, float distance) {
    const math::vec4 clip = projection * math::vec4(0.0f, 0.0f, -distance, 1.0f);
    return clip.z / clip.w;
}

}  // namespace

TEST_CASE("[sp14.depth] reverse-Z float32 separates surfaces 3e-7 of their distance apart, at any distance") {
    const math::mat4 projection = math::perspective_reverse_z(1.0471976f, 16.0f / 9.0f, 0.05f);
    // From a cockpit sensor 0.1 m away to 1 AU; the moon and gas giant at s = 1 and s = 0.01.
    for (const float distance :
         {0.1f, 0.5f, 2.0f, 100.0f, 1e5f, 3.84e6f, 7e6f, 1e8f, 3.84e8f, 7e8f, 9.5e8f, 1.496e11f}) {
        CAPTURE(distance);
        const float near_depth = depth_of(projection, distance);
        const float far_depth = depth_of(projection, distance * (1.0f + 3e-7f));
        CHECK(std::isfinite(near_depth));
        CHECK(near_depth > 0.0f);
        CHECK(near_depth > far_depth);  // reverse-Z: nearer is greater, and the two are distinguishable
        // The depth is near / distance to within float32 rounding: relative precision the same at every distance.
        CHECK(std::abs(near_depth * distance / 0.05f - 1.0f) < 4e-7f);
    }
    // Conventional float depth (1 - near / distance) cannot tell 3.84e8 m from 7e8 m: both round to 1.
    CHECK(1.0f - 0.05f / 3.84e8f == 1.0f - 0.05f / 7e8f);
}
