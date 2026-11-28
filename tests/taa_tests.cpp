// Temporal anti-aliasing (docs/specs/temporal-aa/spec.md, ADR 0061): the jitter sequence and the jittered projection.
// The resolve itself is tested by images: tests/golden/taa_motion.png (taa.AC3) and the render-origin pair (taa.AC4).

#include <veach/core/math.hpp>
#include <veach/render/renderer.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

using namespace veach;

TEST_CASE("[taa.AC1] the jitter is Halton (2, 3) by frame, period 8, centered within half a pixel") {
    // Points 1 to 8 of Halton (2, 3), minus half a pixel.
    constexpr float expected[8][2] = {
        {0.0f, -1.0f / 6.0f},   {-0.25f, 1.0f / 6.0f},    {0.25f, -7.0f / 18.0f}, {-0.375f, -1.0f / 18.0f},
        {0.125f, 5.0f / 18.0f}, {-0.125f, -5.0f / 18.0f}, {0.375f, 1.0f / 18.0f}, {-0.4375f, 7.0f / 18.0f},
    };
    math::vec2 sum(0.0f);
    for (std::uint64_t frame = 0; frame < 8; ++frame) {
        const math::vec2 j = render::taa_jitter(frame);
        CHECK(j.x == doctest::Approx(expected[frame][0]));
        CHECK(j.y == doctest::Approx(expected[frame][1]));
        CHECK(std::abs(j.x) < 0.5f);
        CHECK(std::abs(j.y) < 0.5f);
        CHECK(render::taa_jitter(frame + 8) == j);  // indexed by the frame counter only: replays reproduce it
        sum += j;
    }
    CHECK(std::abs(sum.x / 8.0f) < 0.07f);  // the eight samples average near the pixel's center
    CHECK(std::abs(sum.y / 8.0f) < 0.07f);
}

TEST_CASE("[taa.AC1] the jittered projection moves every point by the jitter, in pixels, y down") {
    const math::mat4 view_projection = math::perspective_reverse_z(math::radians(60.0f), 16.0f / 9.0f, 0.1f);
    constexpr std::uint32_t width = 1920;
    constexpr std::uint32_t height = 1080;
    const math::vec2 jitter(0.25f, -0.375f);
    const math::mat4 jittered = render::jittered_view_projection(view_projection, jitter, width, height);
    const auto pixel = [&](const math::mat4& m, math::vec3 p) {
        const math::vec4 clip = m * math::vec4(p, 1.0f);
        return math::vec2((0.5f + 0.5f * clip.x / clip.w) * width, (0.5f - 0.5f * clip.y / clip.w) * height);
    };
    for (const math::vec3 p :
         {math::vec3(0.0f, 0.0f, -1.0f), math::vec3(3.0f, -2.0f, -40.0f), math::vec3(-50.0f, 20.0f, -900.0f)}) {
        const math::vec2 moved = pixel(jittered, p) - pixel(view_projection, p);
        CHECK(moved.x == doctest::Approx(jitter.x).epsilon(1e-3));
        CHECK(moved.y == doctest::Approx(jitter.y).epsilon(1e-3));
    }
    // Depth is untouched (reverse-Z stays exact).
    const math::vec4 a = view_projection * math::vec4(1.0f, 1.0f, -7.0f, 1.0f);
    const math::vec4 b = jittered * math::vec4(1.0f, 1.0f, -7.0f, 1.0f);
    CHECK(a.z / a.w == b.z / b.w);
    // No jitter, no change.
    CHECK(render::jittered_view_projection(view_projection, math::vec2(0.0f), width, height) == view_projection);
}
