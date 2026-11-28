// Reference frames and double positions (docs/specs/frames/spec.md, ADR 0045, ADR 0046).

#include "support/test_support.hpp"

#include <veach/core/dmath.hpp>
#include <veach/core/time.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/ecs/json.hpp>
#include <veach/render/renderer.hpp>
#include <veach/runtime/mcp_server.hpp>
#include <veach/space/clock.hpp>
#include <veach/space/orbit.hpp>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>

using namespace veach;
using json = nlohmann::json;

namespace {

constexpr double au = 1.495978707e11;
constexpr double light_year = 9.4607304725808e15;
constexpr double pi = 3.141592653589793238462643383279502884;

World make_world() {
    World world;
    world.import<CoreModule>();
    return world;
}

double ulp(double x) {
    return std::nextafter(x, std::numeric_limits<double>::infinity()) - x;
}

Entity frame(World& world, const char* name, const ReferenceFrame& f, Entity parent = {}) {
    Entity e = world.entity(name);
    if (parent) {
        e.child_of(parent);
    }
    e.set(f);
    return e;
}

math::dquat about_y(double angle) {
    return {dmath::cos(0.5 * angle), 0.0, dmath::sin(0.5 * angle), 0.0};
}

double max_abs(const math::dmat3& a, const math::dmat3& b) {
    double m = 0.0;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            m = std::max(m, std::abs(a[c][r] - b[c][r]));
        }
    }
    return m;
}

/// A planet-like pair: an inertial body frame 1 AU out, moving at 29.78 km/s, and its body-fixed frame spinning once
/// a sidereal day about a tilted axis.
struct Planet {
    World world = make_world();
    double omega = 2.0 * pi / 86164.0905;
    math::dquat tilt = math::normalize(math::dquat(dmath::cos(0.2), dmath::sin(0.2), 0.0, 0.0));
    Entity inertial;
    Entity fixed;
    explicit Planet(double spin_angle = 1.234) {
        inertial = frame(world, "earth", {.position = {au, 0.0, 0.0}, .linear_velocity = {0.0, 0.0, -29780.0}});
        fixed = frame(world, "earth_fixed",
                      {.kind = FrameKind::rotating,
                       .up = FrameUp::radial,
                       .orientation = tilt * about_y(spin_angle),
                       .angular_velocity = tilt * math::dvec3(0.0, omega, 0.0)},
                      inertial);
        propagate_transforms(world);
    }
};

}  // namespace

TEST_CASE("[frames.AC1] positions keep their precision in a frame at 1 AU and at 0.1 ly") {
    // SP-03's budget for one continuous frame: the double spacing at the distance.
    CHECK(ulp(au) == doctest::Approx(3.05e-5).epsilon(0.01));
    CHECK(ulp(0.1 * light_year) == 0.125);
    for (const double distance : {au, 0.1 * light_year}) {
        CAPTURE(distance);
        World world = make_world();
        const math::dvec3 where = math::dvec3(0.6, 0.48, 0.64) * distance;
        const Entity ship = frame(world, "ship", {.position = where});
        const Entity crew = world.entity("crew").child_of(ship).set(Transform{{1.0, 0.5, 0.25}});
        const Entity gauge = world.entity("gauge").child_of(crew).set(Transform{{1e-6, 0.0, 0.0}});
        propagate_transforms(world);
        // Relative to its frame, a micrometer is a micrometer however far the frame is.
        CHECK(crew.get<FrameTransform>().translation == math::dvec3(1.0, 0.5, 0.25));
        CHECK(crew.get<FrameTransform>().frame == ship.id());
        CHECK(std::abs(gauge.get<FrameTransform>().translation.x - (1.0 + 1e-6)) < 1e-15);
        // In the root world it is rounded to the spacing there, and no worse.
        const auto pose = frame_pose(world, ship, Entity{});
        REQUIRE(pose.has_value());
        const math::dvec3 in_root = pose->point(crew.get<FrameTransform>().translation);
        CHECK(math::length(in_root - (where + math::dvec3(1.0, 0.5, 0.25))) <= 2.0 * ulp(distance));
        // ...while two points of the same frame keep their micrometer through the frame.
        CHECK(std::abs(math::length(pose->point(gauge.get<FrameTransform>().translation) - in_root) - 1e-6) <=
              2.0 * ulp(distance));
    }
}

TEST_CASE("[frames.AC2] propagation stops at frames and matches float32 propagation in a flat world") {
    World world = make_world();
    // A flat chain, as every scene before frames: root -> child -> grandchild.
    const Transform root_local{
        {10.0, -2.0, 3.5}, math::angleAxis(0.7f, math::normalize(math::vec3(1, 2, 3))), {1.0f, 2.0f, 0.5f}};
    const Transform child_local{{0.25, 1.5, -4.0}, math::angleAxis(-1.1f, math::vec3(0, 1, 0)), {3.0f, 3.0f, 3.0f}};
    const Entity root = world.entity("root").set(root_local);
    const Entity child = world.entity("child").child_of(root).set(child_local);
    propagate_transforms(world);
    CHECK(root.get<FrameTransform>().translation == root_local.translation);
    CHECK(root.get<FrameTransform>().basis == root_local.basis());
    // The float32 world matrix of before: parent * local as mat4.
    const auto mat4_of = [](const Transform& t) {
        math::mat4 m(t.basis());
        m[3] = math::vec4(math::vec3(t.translation), 1.0f);
        return m;
    };
    const math::mat4 old = mat4_of(root_local) * mat4_of(child_local);
    const FrameTransform& c = child.get<FrameTransform>();
    for (int col = 0; col < 3; ++col) {
        CHECK(c.basis[col] == math::vec3(old[col]));  // the basis is the same float32 product, bit for bit
    }
    CHECK(math::length(math::vec3(c.translation) - math::vec3(old[3])) < 1e-5f);  // the translation is better

    // Frames: a frame's children start from the identity; frames are found through entities without Transform.
    const Entity ship = frame(world, "ship", {.position = {au, 0.0, 0.0}});
    const Entity hull = world.entity("hull").child_of(ship).set(Transform{{2.0, 0.0, 0.0}});
    const Entity seat = world.entity("seat").child_of(hull).set(Transform{{0.0, 1.0, 0.0}});
    const Entity group = world.entity("group").child_of(ship);  // no Transform
    const Entity lamp = world.entity("lamp").child_of(group).set(Transform{{0.0, 3.0, 0.0}});
    const Entity cabin = frame(world, "cabin", {.position = {5.0, 0.0, 0.0}}, hull);  // a frame inside an entity
    const Entity pilot = world.entity("pilot").child_of(cabin).set(Transform{{0.0, 0.0, 1.0}});
    propagate_transforms(world);
    CHECK(hull.get<FrameTransform>().translation == math::dvec3(2.0, 0.0, 0.0));
    CHECK(hull.get<FrameTransform>().frame == ship.id());
    CHECK(seat.get<FrameTransform>().translation == math::dvec3(2.0, 1.0, 0.0));
    CHECK(seat.get<FrameTransform>().frame == ship.id());
    CHECK(lamp.get<FrameTransform>().frame == ship.id());
    CHECK(pilot.get<FrameTransform>().translation == math::dvec3(0.0, 0.0, 1.0));
    CHECK(pilot.get<FrameTransform>().frame == cabin.id());
    CHECK(frame_of(world, pilot) == cabin);
    CHECK(frame_of(world, cabin) == ship);  // a frame's parent frame is the nearest frame above it, skipping hull
    CHECK(cabin.get<FrameLink>().parent == ship.id());
    CHECK(cabin.get<FrameLink>().depth == 2);
    CHECK_FALSE(frame_of(world, root));

    // A Transform on a frame is reported, and its children still start from the identity.
    test::LogCapture logs;
    ship.set(Transform{{100.0, 0.0, 0.0}});
    propagate_transforms(world);
    CHECK(logs.contains("'ship' has both ReferenceFrame and Transform"));
    CHECK(hull.get<FrameTransform>().translation == math::dvec3(2.0, 0.0, 0.0));
}

TEST_CASE("[frames.AC3] frame-to-frame poses compose in double through the lowest common ancestor") {
    World world = make_world();
    // Two stations 1 AU out, 1 km apart, one turned.
    const Entity a = frame(world, "a", {.position = {au, 1.0e9, -2.0e10}});
    const Entity b = frame(world, "b", {.position = {au + 1000.25, 1.0e9 + 0.5, -2.0e10}, .orientation = about_y(0.7)});
    propagate_transforms(world);
    const auto ab = frame_pose(world, a, b);
    REQUIRE(ab.has_value());
    // By hand: the 1 km between them, turned into b's axes.
    const math::dvec3 d(-1000.25, -0.5, 0.0);
    const math::dvec3 expected = math::transpose(math::mat3_cast(about_y(0.7))) * d;
    CHECK(math::length(ab->position - expected) < 1e-9);
    // Through the root instead (what composing absolute poses does), the 1 AU rounding shows.
    const FramePose naive = compose(local_pose(b.get<ReferenceFrame>()).inverse(), local_pose(a.get<ReferenceFrame>()));
    MESSAGE("a in b: through the ancestor ", math::length(ab->position - expected), " m, through absolute poses ",
            math::length(naive.position - expected), " m");
    CHECK(math::length(naive.position - expected) > 1e-7);

    // A deeper tree: sun -> planet (moving) -> body-fixed (spinning) and planet -> ship (moving in orbit). The
    // ancestor is the planet's frame; the result agrees with composing by hand there.
    Planet p;
    const Entity ship = frame(p.world, "ship",
                              {.position = {7.0e6, 1000.0, 0.0},
                               .orientation = math::normalize(math::dquat(0.9, 0.1, 0.3, -0.2)),
                               .linear_velocity = {0.0, 0.0, 7546.0}},
                              p.inertial);
    propagate_transforms(p.world);
    const auto fixed_in_ship = frame_pose(p.world, p.fixed, ship);
    REQUIRE(fixed_in_ship.has_value());
    const FramePose by_hand =
        compose(local_pose(ship.get<ReferenceFrame>()).inverse(), local_pose(p.fixed.get<ReferenceFrame>()));
    CHECK(math::length(fixed_in_ship->position - by_hand.position) < 1e-9);
    CHECK(max_abs(fixed_in_ship->rotation, by_hand.rotation) < 1e-15);
    CHECK(math::length(fixed_in_ship->velocity - by_hand.velocity) < 1e-9);
    CHECK(math::length(fixed_in_ship->angular_velocity - by_hand.angular_velocity) < 1e-18);
    // The inverse undoes it.
    const auto ship_in_fixed = frame_pose(p.world, ship, p.fixed);
    REQUIRE(ship_in_fixed.has_value());
    const FramePose round = compose(*fixed_in_ship, *ship_in_fixed);
    CHECK(math::length(round.position) < 1e-8);
    CHECK(max_abs(round.rotation, math::dmat3(1.0)) < 1e-15);
    CHECK(math::length(round.velocity) < 1e-9);
    // A point fixed on the spinning body moves at ω × r in the planet's frame.
    const auto fixed_in_planet = frame_pose(p.world, p.fixed, p.inertial);
    REQUIRE(fixed_in_planet.has_value());
    const math::dvec3 r(6.371e6, 0.0, 0.0);
    const math::dvec3 v = fixed_in_planet->velocity_of(r, math::dvec3(0.0));
    CHECK(math::length(v - math::cross(fixed_in_planet->angular_velocity, fixed_in_planet->rotation * r)) < 1e-12);
    CHECK(math::length(v) == doctest::Approx(p.omega * 6.371e6 * std::cos(0.0)).epsilon(0.3));  // ~465 m/s

    // Not a frame: an error with a code.
    const Entity rock = p.world.entity("rock").set(Transform{});
    const auto bad = frame_pose(p.world, rock, Entity{});
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::ecs_not_a_frame);
}

TEST_CASE("[frames.AC4] a handoff converts pose and velocity in double, so nothing moves") {
    Planet p;
    // A craft 1 km above the surface, moving at 95 m/s relative to the ground, in the planet's inertial frame.
    const auto fixed_in_inertial = frame_pose(p.world, p.fixed, p.inertial);
    REQUIRE(fixed_in_inertial.has_value());
    const math::dvec3 r_fixed = math::normalize(math::dvec3(0.3, 0.5, 0.8)) * (6.371e6 + 1000.0);
    const math::dvec3 r = fixed_in_inertial->point(r_fixed);
    const math::dvec3 v = fixed_in_inertial->velocity_of(r_fixed, math::dvec3(95.0, 0.0, 0.0));
    const Entity craft =
        p.world.entity("craft").child_of(p.inertial).set(Transform{r, math::quat(0.9f, 0.1f, 0.3f, 0.3f)});
    propagate_transforms(p.world);

    math::dvec3 velocity = v;
    REQUIRE(handoff(p.world, craft, p.fixed, &velocity).has_value());
    CHECK(craft.parent() == p.fixed);
    CHECK(craft.get<FrameTransform>().frame == p.fixed.id());
    // In the rotating frame: v_rot = v_inertial − ω × r, in its axes.
    const math::dmat3 rt = math::transpose(fixed_in_inertial->rotation);
    const math::dvec3 expected_v = rt * (v - math::cross(fixed_in_inertial->angular_velocity, r));
    CHECK(math::length(velocity - expected_v) < 1e-9);
    CHECK(math::length(velocity - math::dvec3(95.0, 0.0, 0.0)) < 1e-6);
    const math::dvec3 back = fixed_in_inertial->point(craft.get<Transform>().translation);
    const double dx = math::length(back - r);
    // And back to the inertial frame: the same place and velocity.
    propagate_transforms(p.world);
    REQUIRE(handoff(p.world, craft, p.inertial, &velocity).has_value());
    CHECK(craft.parent() == p.inertial);
    const double dx2 = math::length(craft.get<Transform>().translation - r);
    const double dv = math::length(velocity - v);
    MESSAGE("handoff inertial -> body-fixed: |dx| ", dx, " m; and back: |dx| ", dx2, " m, |dv| ", dv, " m/s");
    CHECK(dx < 1e-6);
    CHECK(dx2 < 1e-6);
    CHECK(dv < 1e-6);
    const float turned =
        math::length(craft.get<Transform>().rotation - math::normalize(math::quat(0.9f, 0.1f, 0.3f, 0.3f)));
    CHECK(turned < 1e-6f);

    // A nested entity goes straight under the new frame, its pose taken from its FrameTransform.
    const Entity hull = p.world.entity("hull").child_of(p.inertial).set(Transform{r, {}, {2.0f, 2.0f, 2.0f}});
    const Entity light = p.world.entity("light").child_of(hull).set(Transform{{0.0, 1.0, 0.0}});
    propagate_transforms(p.world);
    REQUIRE(handoff(p.world, light, p.fixed).has_value());
    CHECK(light.parent() == p.fixed);
    CHECK(light.get<Transform>().scale == math::vec3(2.0f));
    CHECK(math::length(fixed_in_inertial->point(light.get<Transform>().translation) - (r + math::dvec3(0, 2, 0))) <
          1e-6);

    // Errors: not a frame, a name taken in the target frame.
    const auto bad = handoff(p.world, craft, hull);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::ecs_not_a_frame);
    p.world.entity("craft2").child_of(p.fixed).set(Transform{});
    const Entity twin = p.world.entity("craft2").child_of(p.inertial);  // same name, other parent: allowed
    twin.set(Transform{});
    const auto taken = handoff(p.world, twin, p.fixed);
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error().code == ErrorCode::ecs_handoff_name_conflict);
}

TEST_CASE("[frames.AC5] a rotating frame's orientation comes from exact tick time") {
    World world = make_world();
    PhaseRunner runner;
    runner.register_phases(world);
    world.set<Time>({});
    const FrameSpin s{.tilt = math::normalize(math::dquat(0.95, 0.2, 0.0, 0.1)), .rate = 7.2921150e-5, .phase = 0.3};
    const Entity body = world.entity("body").set(s);
    CHECK(body.get<ReferenceFrame>().kind == FrameKind::rotating);
    CHECK(body.get<ReferenceFrame>().orientation == math::normalize(s.tilt) * about_y(0.3));  // set at tick 0

    // Step 999,999 ends at tick 10^6: PostSimulation writes the pose for the step's end.
    world.get_mut<Time>().fixed_step = 999'999;
    runner.run(world, PhaseId::post_simulation, static_cast<float>(fixed_delta_seconds));
    const double t = 1.0e6 * fixed_delta_seconds;
    const math::dquat closed_form = math::normalize(s.tilt) * about_y(s.phase + s.rate * t);
    CHECK(body.get<ReferenceFrame>().orientation == closed_form);  // bit for bit: nothing was integrated
    CHECK(body.get<ReferenceFrame>().angular_velocity == math::normalize(s.tilt) * math::dvec3(0.0, s.rate, 0.0));

    // A pose source written with the toolbox's clock (space::orientation, exact residue arithmetic): a base fixed on
    // the body stays where the clock says, in the inertial view, at any tick.
    const space::Spin spin{.period_q = space::period_q_per_tick * 5'169'846, .phase0 = 0.4, .obliquity = 0.41};
    const Entity earth = world.entity("earth").set(ReferenceFrame{.kind = FrameKind::rotating, .up = FrameUp::radial});
    const auto orientation_quat = [](const space::Orientation& o) {
        return math::normalize(math::quat_cast(math::dmat3(o.x, o.y, o.z)));
    };
    world.system<ReferenceFrame>("EarthSpin").kind<phase::PostSimulation>().each([&](Entity e, ReferenceFrame& f) {
        if (e == earth) {
            const std::int64_t tick = static_cast<std::int64_t>(e.world().get<Time>().fixed_step) + 1;
            f.orientation = orientation_quat(space::orientation(spin, tick));
        }
    });
    const math::dvec3 base(0.0, 6.371e6 * std::sin(0.5), 6.371e6 * std::cos(0.5));
    const Entity site = world.entity("site").child_of(earth).set(Transform{base});
    for (const std::int64_t step : {std::int64_t{0}, std::int64_t{12'345}, std::int64_t{5'184'000 * 30}}) {
        world.get_mut<Time>().fixed_step = static_cast<std::uint64_t>(step);
        runner.run(world, PhaseId::post_simulation, static_cast<float>(fixed_delta_seconds));
        propagate_transforms(world);
        const auto pose = frame_pose(world, earth, Entity{});
        REQUIRE(pose.has_value());
        const math::dvec3 seen = pose->point(site.get<FrameTransform>().translation);
        const math::dvec3 clock = space::orientation(spin, step + 1).to_system(base);
        CAPTURE(step);
        CHECK(math::length(seen - clock) < 1e-8);
        CHECK(site.get<Transform>().translation == base);  // the base itself never moves
    }
}

TEST_CASE("[frames.AC6] up is +Y in flat frames and radial in body frames") {
    World world = make_world();
    const Entity station = frame(world, "station", {.position = {5.0, 0.0, 0.0}});
    const Entity body = frame(world, "body", {.up = FrameUp::radial});
    CHECK(up(world, Entity{}, {3.0, 4.0, 0.0}) == math::dvec3(0.0, 1.0, 0.0));
    CHECK(up(world, station, {3.0, 4.0, 0.0}) == math::dvec3(0.0, 1.0, 0.0));
    CHECK(math::length(up(world, body, {3.0, 4.0, 0.0}) - math::dvec3(0.6, 0.8, 0.0)) < 1e-15);
    CHECK(up(world, body, {0.0, 0.0, 0.0}) == math::dvec3(0.0, 1.0, 0.0));
}

namespace {

/// A renderer on the null device, a camera and two lights; the scene is placed by `place`.
struct ExtractScene {
    std::unique_ptr<rhi::Device> device = std::move(*rhi::create_device({}));
    World world = make_world();
    render::Renderer renderer{*device};
    ExtractScene() {
        world.component<Camera>();
        world.component<PointLight>();
        world.component<SpotLight>();
    }
    /// The camera-relative quantities a renderer can see: camera-from-object for every light and draw.
    const render::RenderWorld& extract() {
        propagate_transforms(world);
        renderer.extract(world);
        return renderer.render_world();
    }
};

void cornell_like(World& world, Entity parent, math::dvec3 at, math::quat turn) {
    const auto put = [&](const char* name, math::dvec3 p, math::quat q) {
        Entity e = world.entity(name);
        if (parent) {
            e.child_of(parent);
        }
        e.set(Transform{at + math::dvec3(turn * math::vec3(p)), turn * q});
        return e;
    };
    put("camera", {0.0, 1.0, 3.9}, math::quat(1, 0, 0, 0)).set(Camera{});
    put("bulb", {0.0, 1.5, 0.0}, math::quat(1, 0, 0, 0)).set(PointLight{.intensity = 10.0f});
    put("spot", {0.5, 1.9, -0.5}, math::angleAxis(-1.2f, math::vec3(1, 0, 0))).set(SpotLight{});
}

/// Camera-space positions and directions of the lights (what shading sees).
std::vector<math::vec3> camera_space(const render::RenderWorld& rw) {
    std::vector<math::vec3> out;
    const math::mat4& view = rw.views.front().view;
    for (const render::RenderLight& l : rw.lights) {
        out.push_back(math::vec3(view * math::vec4(l.position, 1.0f)));
        out.push_back(math::vec3(view * math::vec4(l.direction, 0.0f)));
    }
    return out;
}

}  // namespace

TEST_CASE("[frames.AC7] extraction is camera-relative, with one render origin") {
    ExtractScene origin;
    cornell_like(origin.world, Entity{}, math::dvec3(0.0), math::quat(1, 0, 0, 0));
    const render::RenderWorld& base = origin.extract();
    REQUIRE(base.views.size() == 1);
    CHECK(base.space.frame == 0);
    CHECK(base.space.origin == math::dvec3(0.0));  // within 32 m of the origin: render space is world space
    CHECK(base.views[0].position == math::vec3(0.0f, 1.0f, 3.9f));
    CHECK(base.views[0].height_in_frame == doctest::Approx(1.0));
    const std::vector<math::vec3> expected = camera_space(base);

    // The same scene 1 AU out in the root world: the render origin follows the camera, on the 64 m grid.
    const math::dvec3 far = math::dvec3(0.6, 0.48, 0.64) * au;
    ExtractScene root;
    cornell_like(root.world, Entity{}, far, math::quat(1, 0, 0, 0));
    const render::RenderWorld& rw = root.extract();
    CHECK(rw.space.origin == render::render_origin(far + math::dvec3(0.0, 1.0, 3.9)));
    CHECK(math::length(rw.views[0].position) < 56.0f);  // no large numbers reach the GPU
    std::vector<math::vec3> got = camera_space(rw);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(math::length(got[i] - expected[i]) < 1e-4f);  // 1 AU rounds positions to 3e-5 m
    }

    // Inside a frame 0.1 ly out: everything is in the camera's frame, so render space is the frame and the render
    // world is bit for bit the one at the origin.
    ExtractScene in_frame;
    const Entity ship = frame(in_frame.world, "ship", {.position = math::dvec3(0.6, 0.48, 0.64) * (0.1 * light_year)});
    cornell_like(in_frame.world, ship, math::dvec3(0.0), math::quat(1, 0, 0, 0));
    const render::RenderWorld& framed = in_frame.extract();
    CHECK(framed.space.frame == ship.id());
    CHECK(framed.views[0].view == base.views[0].view);
    CHECK(framed.views[0].view_projection == base.views[0].view_projection);
    REQUIRE(framed.lights.size() == base.lights.size());
    for (std::size_t i = 0; i < base.lights.size(); ++i) {
        CHECK(framed.lights[i].position == base.lights[i].position);
        CHECK(framed.lights[i].direction == base.lights[i].direction);
    }

    // Seen from the root world into a turned frame 1 AU out: composed through the ancestor, the same view.
    ExtractScene across;
    const math::quat turn = math::angleAxis(1.0f, math::normalize(math::vec3(0.2f, 1.0f, 0.1f)));
    const Entity station = frame(across.world, "station", {.position = far, .orientation = math::dquat(turn)});
    cornell_like(across.world, station, math::dvec3(0.0), math::quat(1, 0, 0, 0));
    const Entity camera = across.world.lookup("station::camera");
    REQUIRE(handoff(across.world, camera, Entity{}).has_value());  // the camera moves to the root world, in place
    const render::RenderWorld& crossed = across.extract();
    CHECK(crossed.space.frame == 0);
    got = camera_space(crossed);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(math::length(got[i] - expected[i]) < 1e-4f);
    }

    // The body context: height along up from the frame's origin, and up in render space.
    ExtractScene planet;
    const Entity body = frame(planet.world, "body", {.up = FrameUp::radial});
    const math::dvec3 eye = math::normalize(math::dvec3(1.0, 1.0, 0.0)) * (6.371e6 + 100.0);
    planet.world.entity("eye").child_of(body).set(Transform{eye}).set(Camera{});
    const render::RenderWorld& sky = planet.extract();
    CHECK(sky.views[0].height_in_frame == doctest::Approx(6.371e6 + 100.0));
    CHECK(math::length(sky.views[0].up - math::normalize(math::vec3(1.0f, 1.0f, 0.0f))) < 1e-6f);
    CHECK(math::length(sky.views[0].position) < 56.0f);
}

TEST_CASE("[frames.AC9] Luau sees positions as exact doubles, with Position arithmetic and the frame API") {
    test::TempProject project("frames-luau");
    project.write("main.luau", R"(--!strict
local veach = require("@veach")
local c = veach.components
local ship = veach.spawn("ship")
ship:set(c.ReferenceFrame, {
	kind = "inertial", up = "axis_y",
	position = veach.position(149597870700, 0, 0), orientation = veach.quat.identity(),
	linear_velocity = veach.position(0, 0, 0), angular_velocity = veach.position(0, 0, 0),
})
local probe = veach.spawn("probe")
probe:set(c.Transform, { translation = veach.position(149597870700.125, 2, 3), rotation = veach.quat.identity(), scale = vector.one })
local pilot = veach.spawn({ name = "pilot", parent = ship })
pilot:set(c.Transform, { translation = veach.position(0, 1.75, 0), rotation = veach.quat.identity(), scale = vector.one })
local Marker = veach.component("Marker", { at = veach.position(1e15, 0.5, -0.25), label = "far" })
veach.spawn("marker"):set(Marker, Marker.defaults)

veach.system("Check", "Simulation", veach.query(c.Transform), function(e, t)
	if e ~= probe or veach.time.frame() ~= 2 then
		return
	end
	local p = t.translation
	veach.log.info(`exact {p.x == 149597870700.125} {tostring(p)}`)
	local q = p + vector.create(0.5, 0, 0)
	local d: vector = q - p
	local back = q - vector.create(0.5, 0, 0)
	veach.log.info(`ops {d.x} {back == p} {typeof(q)}`)
	local in_root = veach.frames.convert(veach.position(0, 1.75, 0), ship, nil)
	veach.log.info(`convert {tostring(in_root)}`)
	local in_ship = veach.frames.convert(p, nil, ship)
	veach.log.info(`into ship {tostring(in_ship)}`)
	veach.log.info(`of {veach.frames.of(pilot) == ship} {veach.frames.of(e) == nil}`)
	local off = veach.frames.offset(pilot, e)
	veach.log.info(string.format("offset %.3f %.3f %.3f", off.x, off.y, off.z))
	local u = veach.frames.up(nil, p)
	veach.log.info(`up {u.y}`)
end)

veach.system("Marker", "Simulation", veach.query(Marker), function(_e, m)
	if veach.time.frame() == 2 then
		veach.log.info(`marker {tostring(m.at)} {m.label}`)
	end
end)
)");
    test::LogCapture logs;
    {
        Engine engine(test::headless_config(project.path(), "main.luau", 2));
        CHECK(engine.run() == 0);
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
    CHECK(logs.contains("exact true Position(149597870700.125, 2, 3)"));
    CHECK(logs.contains("ops 0.5 true Position"));
    CHECK(logs.contains("convert Position(149597870700, 1.75, 0)"));
    CHECK(logs.contains("into ship Position(0.125, 2, 3)"));
    CHECK(logs.contains("of true true"));
    CHECK(logs.contains("offset 0.125 0.250 3.000"));
    CHECK(logs.contains("up 1"));
    CHECK(logs.contains("marker Position(1e+15, 0.5, -0.25) far"));

    // A vector where a Position belongs is an error that names the fix; so is adding two positions.
    for (const char* source :
         {"local veach = require(\"@veach\")\nveach.spawn(\"x\"):set(veach.components.Transform, { translation = "
          "vector.create(1, 2, 3), rotation = veach.quat.identity(), scale = vector.one } :: any)\n",
          "local veach = require(\"@veach\")\nlocal p = veach.position(1, 2, 3)\nlocal _ = (p :: any) + p\n",
          "local veach = require(\"@veach\")\nlocal _ = veach.frames.convert(veach.position(0, 0, 0), "
          "veach.spawn(\"not a frame\"), nil)\n"}) {
        test::TempProject bad("frames-luau-bad");
        bad.write("main.luau", source);
        test::LogCapture bad_logs;
        Engine engine(test::headless_config(bad.path(), "main.luau", 1));
        CHECK(engine.run() == 1);
        const bool named = bad_logs.contains("veach.position(x, y, z)") ||
                           bad_logs.contains("Two positions don't add") || bad_logs.contains("[ecs.not_a_frame]");
        CHECK_MESSAGE(named, source);
    }
}

TEST_CASE("[frames.AC10] JSON and MCP carry double positions exactly, and the frames tool lists the tree") {
    // JSON: shortest round-trip digits.
    World world = make_world();
    Transform t{{149597870700.125, -0.1, 1e-300}};
    const json j = to_json(world, world.id<Transform>(), &t);
    CHECK(j["translation"].dump() == "[149597870700.125,-0.1,1e-300]");
    Transform back;
    REQUIRE(from_json(world, world.id<Transform>(), &back, j, "test").has_value());
    CHECK(back.translation == t.translation);

    test::TempProject project("frames-mcp");
    project.write("main.luau", R"(--!strict
local veach = require("@veach")
local ship = veach.spawn("ship")
ship:set(veach.components.FrameSpin, { tilt = veach.quat.identity(), rate = 0.001, phase = 0 })
veach.spawn({ name = "crate", parent = ship }):set(veach.components.Transform, {
	translation = veach.position(1, 2, 3), rotation = veach.quat.identity(), scale = vector.one })
)");
    Engine engine(test::headless_config(project.path(), "main.luau", 100));
    REQUIRE(engine.init().has_value());
    REQUIRE(engine.start().has_value());
    McpServer server(engine);
    const auto call = [&](const std::string& tool, json arguments) {
        const json r = server.handle({{"jsonrpc", "2.0"},
                                      {"id", 1},
                                      {"method", "tools/call"},
                                      {"params", {{"name", tool}, {"arguments", arguments}}}});
        REQUIRE_FALSE(r["result"].value("isError", false));
        return json::parse(r["result"]["content"][0]["text"].get<std::string>());
    };
    call("set_component", {{"entity", "ship::crate"},
                           {"component", "Transform"},
                           {"value", {{"translation", {149597870700.125, 0.1, -2.5e-7}}}}});
    const json got = call("get_component", {{"entity", "ship::crate"}, {"component", "Transform"}});
    CHECK(got["translation"][0].get<double>() == 149597870700.125);
    CHECK(got["translation"][1].get<double>() == 0.1);
    CHECK(got["translation"][2].get<double>() == -2.5e-7);
    const json shown = call("eval", {{"source",
                                      "local veach = require(\"@veach\")\n"
                                      "return tostring(veach.position(149597870700.125, 0.1, -2.5e-7))"}});
    CHECK(shown["returns"][0] == "Position(149597870700.125, 0.1, -2.5e-07)");
    call("step_frames", {{"count", 1}});
    const json tree = call("frames", json::object());
    REQUIRE(tree["frames"].size() == 1);
    CHECK(tree["frames"][0]["path"] == "ship");
    CHECK(tree["frames"][0]["kind"] == "rotating");
    CHECK(tree["frames"][0]["parent"].is_null());
    CHECK(tree["frames"][0]["entities"] == 1);
    CHECK(tree["frames"][0]["position"] == json::array({0.0, 0.0, 0.0}));
    CHECK(tree["frames"][0].contains("spin"));
    engine.shutdown();
}
