// The space backdrop (docs/specs/space-backdrop/spec.md, ADR 0091): canonical generation in the engine and per system
// in space/, and the renderer's passes on the null RHI. The Metal image is a golden (tests/CMakeLists.txt).

#include <veach/render/backdrop.hpp>
#include <veach/render/renderer.hpp>
#include <veach/space/backdrop.hpp>
#include <veach/space/gen/address.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>  // doctest prints string operands (MSVC needs it declared)
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "slice.hpp"
#include "support/test_support.hpp"

using namespace veach;

namespace {

bool unit(const math::vec3& v) {
    return std::abs(math::length(v) - 1.0f) < 1e-5f;
}

/// A system address `radius_ly` out from the galaxy's center along +X and `height_ly` above its plane (layer 0).
std::uint64_t address_at(double radius_ly, double height_ly) {
    const auto a = ::space::gen::SystemAddress::encode({.layer = 0,
                                                        .x = 8192 + static_cast<std::uint32_t>(radius_ly / 10.0),
                                                        .y = 1024 + static_cast<std::uint32_t>(height_ly / 10.0),
                                                        .z = 8192,
                                                        .system = 1});
    REQUIRE(a.has_value());
    return a->value;
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

TEST_CASE("[backdrop.AC4] a system's backdrop comes from the universe seed and its address, the same at every scale") {
    veach::space::UniverseConfig config;
    config.seed = 42;
    config.system = 0x1000400100008000ULL;  // the slice's system
    const render::Backdrop home = veach::space::system_backdrop(config);
    CHECK(home == veach::space::system_backdrop(config));  // a revisit sees the same sky
    // Committed (ADR 0043): the slice's sky, and a system 26,000 ly out (the Sun's distance from the center).
    CHECK(render::backdrop_hash(home) == 0xc54507b7cda897a0ULL);
    veach::space::UniverseConfig earthlike = config;
    earthlike.system = address_at(26'000.0, 50.0);
    CHECK(render::backdrop_hash(veach::space::system_backdrop(earthlike)) == 0x03b5ddb4270bad02ULL);

    // Not a function of the scale: the sky is the same at s = 0.01 and s = 1.
    veach::space::UniverseConfig small = config;
    small.scale = 0.01;
    veach::space::UniverseConfig full = config;
    full.scale = 1.0;
    CHECK(veach::space::system_backdrop(small) == veach::space::system_backdrop(full));
    // Another seed or another system: another sky.
    veach::space::UniverseConfig reseeded = config;
    reseeded.seed = 7;
    CHECK(render::backdrop_hash(veach::space::system_backdrop(reseeded)) != render::backdrop_hash(home));
    CHECK(render::backdrop_hash(veach::space::system_backdrop(earthlike)) != render::backdrop_hash(home));
}

TEST_CASE("[backdrop.AC5] where a system is in the galaxy shapes its band") {
    veach::space::UniverseConfig config;
    config.seed = 42;
    config.system = address_at(4'000.0, 50.0);
    const render::GalaxyView inner = veach::space::galaxy_view(config);
    config.system = address_at(26'000.0, 50.0);
    const render::GalaxyView middle = veach::space::galaxy_view(config);
    config.system = address_at(70'000.0, 50.0);
    const render::GalaxyView outer = veach::space::galaxy_view(config);
    config.system = address_at(26'000.0, 9'000.0);
    const render::GalaxyView above = veach::space::galaxy_view(config);

    // Nearer the center: a brighter band, a bigger bulge, an anticenter nearly as bright as the center.
    CHECK(inner.band_brightness > middle.band_brightness);
    CHECK(middle.band_brightness > outer.band_brightness);
    CHECK(inner.bulge > middle.bulge);
    CHECK(inner.anticenter > middle.anticenter);
    CHECK(middle.anticenter > outer.anticenter);
    // Far above the plane: fainter, wider, below the system's horizon of the galaxy, fewer nebulae.
    CHECK(above.band_brightness < middle.band_brightness);
    CHECK(above.band_width > middle.band_width);
    CHECK(above.band_latitude < middle.band_latitude);
    CHECK(above.nebulae < middle.nebulae);
    for (const render::GalaxyView& v : {inner, middle, outer, above}) {
        CHECK(std::abs(math::dot(v.north, v.center)) < 1e-12);
        CHECK(std::abs(math::length(v.north) - 1.0) < 1e-12);
        CHECK(std::abs(math::length(v.center) - 1.0) < 1e-12);
    }
}

TEST_CASE("[backdrop.AC6] the slice on the null RHI bakes its system's backdrop once and draws it every frame") {
    // The slice's own modules, project and script (as tests/slice_tests.cpp), a few frames in orbit.
    std::vector<std::string> args{"--project", std::string(VEACH_SOURCE_DIR) + "/apps/slice", "--headless", "--script",
                                  "scripts/slice_s0.01.luau"};
    const std::vector<std::string_view> views(args.begin(), args.end());
    auto config = resolve_config({.name = "t", .default_project = {}}, views);
    REQUIRE(config.has_value());
    config->log_to_console = false;
    test::LogCapture logs;
    Engine engine(std::move(*config));
    slice::import_modules(engine);
    REQUIRE(engine.init().has_value());
    REQUIRE(engine.start().has_value());
    auto& renderer = engine.services().get<render::Renderer>();
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(engine.frame());
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
    const auto baked = std::ranges::count_if(
        logs.messages("render"), [](const std::string& m) { return m.starts_with("space backdrop: baked"); });
    CHECK(baked == 1);
    const std::string& graph = renderer.last_graph();
    CHECK(graph.find("space backdrop") != std::string::npos);
    CHECK(graph.find("space backdrop bake") == std::string::npos);  // baked on the first frame only

    // The renderer gets the system's backdrop, turned into render space.
    const render::ResolvedSky& sky = renderer.render_world().sky;
    REQUIRE(sky.backdrop.has_value());
    veach::space::UniverseConfig universe;
    universe.seed = 42;
    universe.system = 0x1000400100008000ULL;
    CHECK(sky.backdrop->bakes_like(veach::space::system_backdrop(universe)));
    CHECK(std::abs(math::determinant(sky.backdrop->orientation) - 1.0f) < 1e-4f);
    CHECK(sky.backdrop_settings.exposure_compensation == SpaceBackdrop{}.exposure_compensation);
}
