// The space backdrop (docs/specs/space-backdrop/spec.md, ADR 0090): canonical generation in the engine. The Metal
// image is a golden (tests/CMakeLists.txt).

#include <veach/render/backdrop.hpp>
#include <veach/render/renderer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>  // doctest prints string operands (MSVC needs it declared)
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "support/test_support.hpp"

using namespace veach;

namespace {

bool unit(const math::vec3& v) {
    return std::abs(math::length(v) - 1.0f) < 1e-5f;
}

}  // namespace

TEST_CASE("[backdrop.AC1] a backdrop from a seed is canonical: the same bits for the same seed, committed") {
    const render::Backdrop a = render::generate_backdrop(7, render::galaxy_view_from_seed(7));
    const render::Backdrop b = render::generate_backdrop(7, render::galaxy_view_from_seed(7));
    CHECK(a == b);
    CHECK(render::backdrop_hash(a) == render::backdrop_hash(b));
    // Committed: macOS and Windows CI must both produce these (ADR 0043). A change is a look change on purpose.
    CHECK(render::backdrop_hash(a) == 0x2863123a7f42298bULL);
    CHECK(render::backdrop_hash(render::generate_backdrop(42, render::galaxy_view_from_seed(42))) ==
          0xbfff4a17f902b9f0ULL);

    std::set<std::uint64_t> hashes;
    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        const render::Backdrop d = render::generate_backdrop(seed, render::galaxy_view_from_seed(seed));
        hashes.insert(render::backdrop_hash(d));
        CHECK(d.nebula_count <= render::max_backdrop_nebulae);
        for (std::uint32_t i = 0; i < d.nebula_count; ++i) {
            CHECK(unit(d.nebulae.at(i).direction));
            CHECK(d.nebulae.at(i).radius > 0.0f);
        }
        // The galactic frame is a rotation.
        for (int c = 0; c < 3; ++c) {
            CHECK(unit(d.orientation[c]));
        }
        CHECK(std::abs(math::determinant(d.orientation) - 1.0f) < 1e-5f);
    }
    CHECK(hashes.size() == 32);  // every seed its own sky
}

TEST_CASE("[backdrop.AC2] star colors are black bodies: white at 6500 K, orange when cool, blue when hot") {
    const math::vec3 white = render::blackbody_color(6500.0f);
    for (int c = 0; c < 3; ++c) {
        CHECK(white[c] == doctest::Approx(1.0f).epsilon(0.08));
    }
    const math::vec3 cool = render::blackbody_color(3000.0f);
    CHECK(cool.x > cool.y);  // red > green > blue
    CHECK(cool.y > cool.z);
    const math::vec3 hot = render::blackbody_color(15000.0f);
    CHECK(hot.z > hot.y);
    CHECK(hot.y > hot.x);
    for (const float k : {1000.0f, 2600.0f, 5800.0f, 10000.0f, 40000.0f}) {
        const math::vec3 c = render::blackbody_color(k);
        CHECK(0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(std::min({c.x, c.y, c.z}) >= 0.0f);
    }
}

TEST_CASE("[backdrop.AC3] turning a backdrop or changing its stars does not rebake it, its content does") {
    const render::Backdrop a = render::generate_backdrop(3, render::galaxy_view_from_seed(3));
    render::Backdrop turned = a;
    turned.orientation = math::mat3(math::angleAxis(1.0f, math::vec3(0.0f, 1.0f, 0.0f))) * a.orientation;
    turned.star_density = 2.0f;
    turned.star_saturation = 1.0f;
    CHECK(a.bakes_like(turned));
    render::Backdrop dustier = a;
    dustier.dust = a.dust * 2.0f;
    CHECK_FALSE(a.bakes_like(dustier));
    render::Backdrop other_seed = a;
    other_seed.seed ^= 1u;
    CHECK_FALSE(a.bakes_like(other_seed));
}
