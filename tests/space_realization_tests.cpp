// The realization layer (docs/specs/first-slice/spec.md, ADR 0049, 0088): stages never see the scale, geometry does.
// The hashes below were committed from macOS arm64; Windows x64 CI checks them (canonical code, ADR 0043).

#include <veach/space/clock.hpp>
#include <veach/space/flight.hpp>
#include <veach/space/gen/address.hpp>
#include <veach/space/realization.hpp>
#include <veach/space/sun.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <format>
#include <ostream>  // doctest prints string_view and string operands (MSVC needs it declared)
#include <string>

namespace vs = veach::space;
namespace sg = ::space::gen;

namespace {

/// The slice's system: seed 42, an F star with eight planets; b1 is an Earth-sized temperate world with a moon.
constexpr std::uint64_t slice_seed = 42;
const std::uint64_t slice_system = sg::parse_system_address("S:1000400100008000")->value;

vs::StarSystem realized(double scale) {
    auto s = vs::realize({.seed = slice_seed, .system = slice_system, .scale = scale});
    REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
    return std::move(*s);
}

std::string hex(std::uint64_t v) {
    return std::format("0x{:016x}", v);
}

// Regenerate only on purpose: a stage, dmath or the hash changed. INFO lines print the new values.
constexpr vs::StageHashes expected_stages{.universe = 0x14b9ee7757e680f6ULL,
                                          .system = 0xde0d92a50f020abfULL,
                                          .body_physics = 0x1e1f3aa3566bb95eULL,
                                          .atmosphere = 0x0614c8b54778c448ULL};

}  // namespace

TEST_CASE("[slice.AC1] the generation stages hash the same at s = 1, 0.1 and 0.01, only the realization changes") {
    const vs::StarSystem s1 = realized(1.0);
    const vs::StarSystem s01 = realized(0.1);
    const vs::StarSystem s001 = realized(0.01);
    INFO("universe " << hex(s1.hashes.universe) << " system " << hex(s1.hashes.system) << " body physics "
                     << hex(s1.hashes.body_physics) << " atmosphere " << hex(s1.hashes.atmosphere));
    CHECK(s1.hashes == expected_stages);
    CHECK(s01.hashes == s1.hashes);
    CHECK(s001.hashes == s1.hashes);
    // The geometry differs by scale, and by exactly the scale rules. Its hashes are canonical too.
    INFO("realized " << hex(s1.realized_hash()) << " " << hex(s01.realized_hash()) << " " << hex(s001.realized_hash()));
    CHECK(s1.realized_hash() == 0xb29395ad55c79549ULL);
    CHECK(s01.realized_hash() == 0x0ec9412eb98f3674ULL);
    CHECK(s001.realized_hash() == 0x2d9fc74583c7539bULL);
    for (const vs::StarSystem* s : {&s01, &s001}) {
        const double k = s->scale.s_p;
        INFO("s = " << k);
        REQUIRE(s->bodies.size() == s1.bodies.size());
        for (std::size_t i = 0; i < s->bodies.size(); ++i) {
            const vs::RealizedBody& a = s1.bodies[i];
            const vs::RealizedBody& b = s->bodies[i];
            CHECK(b.radius == doctest::Approx(a.radius * k).epsilon(1e-12));
            CHECK(b.surface_gravity == doctest::Approx(a.surface_gravity).epsilon(1e-12));  // physical
            CHECK(b.gm == doctest::Approx(a.gm * k * k).epsilon(1e-12));
            CHECK(b.relief == doctest::Approx(a.relief * std::sqrt(k)).epsilon(1e-12));
            CHECK(b.atmosphere_top == doctest::Approx(a.atmosphere_top * std::sqrt(k)).epsilon(1e-9));
            // Days don't scale; a tidally locked body's day is its orbit, which follows √s.
            const bool locked = b.index != 0 && s->record.bodies.at(b.index - 1).tidally_locked;
            CHECK(s->orbits.node(b.node).spin.period_q ==
                  (locked ? s->orbits.node(b.node).period_q : s1.orbits.node(a.node).spin.period_q));
        }
        // Orbital periods follow √s (to the period_q rounding).
        for (std::size_t n = 1; n < s->orbits.nodes().size(); ++n) {
            const double p = vs::seconds_from_period_q(s->orbits.nodes()[n].period_q);
            const double p1 = vs::seconds_from_period_q(s1.orbits.nodes()[n].period_q);
            CHECK(p == doctest::Approx(p1 * std::sqrt(k)).epsilon(1e-9));
        }
    }
}

TEST_CASE("[slice.AC2] the home planet: rotating, tilted, with an atmosphere from its composition") {
    const vs::StarSystem s = realized(0.01);
    const vs::RealizedBody* home = s.home();
    REQUIRE(home != nullptr);
    CHECK(home->name == "b1");
    CHECK(home->solid);
    CHECK(home->radius == doctest::Approx(63'710.0).epsilon(0.01));
    CHECK(home->surface_gravity == doctest::Approx(9.82).epsilon(0.01));
    const vs::OrbitNode& node = s.orbits.node(home->node);
    CHECK(node.spin.period_q > 0);
    CHECK(node.spin.obliquity * 180.0 / 3.141592653589793 == doctest::Approx(38.6).epsilon(0.01));
    CHECK(home->day == vs::DayCheck::ok);
    // N2/O2 air from the atmosphere stage; 100 km × √s of it at s = 0.01, Earth-like optical depth.
    const sg::AtmosphereComposition& air = s.atmospheres.at(home->index - 1);
    CHECK(air.kind == sg::atmosphere_temperate);
    CHECK(air.gases.at(0).gas == sg::gas_n2);
    REQUIRE(home->atmosphere.has_value());
    CHECK(home->atmosphere_top == doctest::Approx(9'915.0).epsilon(0.01));
    const veach::math::dvec3 tau = vs::vertical_optical_depth(*home->atmosphere);
    CHECK(tau.z > tau.x);  // bluer: Rayleigh scattering from the composition
    // Its moon has no air and a surface.
    bool moon = false;
    for (const vs::RealizedBody& b : s.bodies) {
        if (b.parent == home->index) {
            moon = true;
            CHECK(b.solid);
        }
    }
    CHECK(moon);
}

TEST_CASE("[slice.AC3] local solar time is noon under the sun and moves with the planet's spin") {
    const vs::StarSystem s = realized(0.01);
    const vs::RealizedBody& home = *s.home();
    const std::int32_t star = s.bodies.front().node;
    const std::int64_t tick = 123'456;
    const vs::SubsolarPoint sub = vs::subsolar_point(s.orbits, home.node, star, tick);
    const vs::SunSample noon = vs::sun_local(s.orbits, home.node, star, sub.latitude, sub.longitude, tick);
    CHECK(noon.local_solar_time == doctest::Approx(12.0).epsilon(1e-6));
    CHECK(noon.elevation * 180.0 / 3.141592653589793 == doctest::Approx(90.0).epsilon(1e-3));
    // A quarter of a sidereal day later the same place is a quarter of a sidereal day further on the 24-hour solar
    // clock: a little under six hours, since the solar day 1 / (1/P_sid - 1/P_orbit) is the longer one.
    const double sidereal = vs::seconds_from_period_q(s.orbits.node(home.node).spin.period_q);
    const double year = vs::seconds_from_period_q(s.orbits.node(home.node).period_q);
    const double solar = 1.0 / (1.0 / sidereal - 1.0 / year);
    const auto later = tick + static_cast<std::int64_t>(sidereal * 60.0 / 4.0);
    const vs::SunSample evening = vs::sun_local(s.orbits, home.node, star, sub.latitude, sub.longitude, later);
    // (to 0.5 %: the orbit is eccentric, so the sun's motion differs from the mean)
    CHECK(evening.local_solar_time == doctest::Approx(12.0 + 6.0 * sidereal / solar).epsilon(5e-3));
    CHECK(evening.local_solar_time < 18.0);
}

TEST_CASE("[slice.AC4] the governor and the handoff bands are the same in meters at every scale") {
    const vs::Governor g;
    CHECK(g.cap(10.0) == 100.0);
    CHECK(g.cap(10'000.0) == 2'000.0);
    for (const double k : {0.01, 1.0}) {
        const vs::StarSystem s = realized(k);
        const vs::HandoffBands b = vs::HandoffBands::of(*s.home(), s.scale.s_a);
        CHECK(b.body_fixed_below == s.home()->atmosphere_top);
        CHECK(b.inertial_above == doctest::Approx(1.1 * b.body_fixed_below));
        CHECK(b.surface_below == 500.0);
        CHECK(b.surface_above == 750.0);
        using vs::FlightFrame, vs::FlightScene, vs::Handoff;
        CHECK(vs::decide_handoff(b, FlightFrame::inertial, FlightScene::free_flight, b.body_fixed_below * 0.99, 1e5) ==
              Handoff::to_body_fixed);
        CHECK(vs::decide_handoff(b, FlightFrame::body_fixed, FlightScene::free_flight, b.body_fixed_below * 1.05,
                                 1e5) == Handoff::none);  // hysteresis
        CHECK(vs::decide_handoff(b, FlightFrame::body_fixed, FlightScene::free_flight, 900.0, 499.0) ==
              Handoff::to_surface);
        CHECK(vs::decide_handoff(b, FlightFrame::body_fixed, FlightScene::surface, 900.0, 600.0) == Handoff::none);
        CHECK(vs::decide_handoff(b, FlightFrame::body_fixed, FlightScene::surface, 1200.0, 751.0) ==
              Handoff::to_free_flight);
    }
    // The heading is transported, never derived from north: over the pole it stays continuous.
    const double a0 = 3.141592653589793 * 0.25;
    vs::Heading h{.up = {0.0, std::sin(a0), std::cos(a0)}, .forward = {0.0, std::cos(a0), -std::sin(a0)}};
    for (int i = 0; i <= 200; ++i) {
        const double a = 3.141592653589793 * (0.25 + 0.5 * i / 200.0);  // across +Y, the pole
        const veach::math::dvec3 up{0.0, std::sin(a), std::cos(a)};
        const vs::Heading next = vs::transport(h, up, 1.0, 0.0);
        CHECK(veach::math::dot(next.forward, h.forward) > 0.999);
        h = next;
    }
    // Settings out of range are refused with the fix.
    auto bad = vs::realize({.seed = 1, .system = slice_system, .scale = 2.0});
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == veach::ErrorCode::universe_settings_invalid);
    auto low = vs::realize({.seed = 1, .system = slice_system, .scale = 0.1, .orbit_scale = 0.05});
    REQUIRE_FALSE(low.has_value());
    CHECK(low.error().code == veach::ErrorCode::universe_settings_invalid);
}
