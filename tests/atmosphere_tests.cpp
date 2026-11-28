// The planet atmosphere's CPU parts and helpers (docs/specs/planet-atmosphere/spec.md): the ray-sphere helpers at
// 1e4 km on the CPU and the GPU, the aerial-perspective volume's slice mapping, the per-medium LUT cache, sky
// visibility from physics ray casts and its way to the renderer.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"

#include <veach/physics/physics.hpp>
#include <veach/render/atmosphere.hpp>
#include <veach/render/renderer.hpp>
#include <veach/veach.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

using namespace veach;

namespace {

constexpr double deg = 3.14159265358979323846 / 180.0;

/// The near root of |o + t d| = R in double: the reference.
double near_root(math::dvec3 o, math::dvec3 d, double c) {
    const double b = math::dot(o, d);
    return -b - std::sqrt(b * b - c);
}

struct SphereQuery {
    bool span = true;  ///< a span query; false: a shadow test only
    math::vec3 origin;
    math::vec3 direction;
    float c = 0.0f;       ///< for the span
    float radius = 0.0f;  ///< for the shadow test
    double reference = 0.0;
};

/// A camera 1e4 km from the center of a 6371 km sphere, rays from its center to just inside its limb (39.6 deg).
std::vector<SphereQuery> far_queries() {
    constexpr double radius = 6371.0;
    const math::dvec3 o = math::normalize(math::dvec3(1.0, 2.0, 3.0)) * 1e4;
    const math::dvec3 toward = -math::normalize(o);
    const math::dvec3 side = math::normalize(math::cross(toward, math::dvec3(0.0, 1.0, 0.0)));
    const double h = 1e4 - radius;
    std::vector<SphereQuery> out;
    for (const double angle : {0.0, 10.0, 20.0, 30.0, 35.0, 39.0, 39.5}) {
        const math::dvec3 d = math::normalize(toward * std::cos(angle * deg) + side * std::sin(angle * deg));
        const double c = h * (2.0 * radius + h);
        out.push_back({true, math::vec3(o), math::vec3(d), static_cast<float>(c), static_cast<float>(radius),
                       near_root(o, d, c)});
    }
    return out;
}

/// A camera 1.3 m above a 1e4 km sphere, looking 1, 5 and 30 degrees below its horizon: the near root is meters to
/// km away, and |o|^2 - R^2 in float (steps of 8 km^2 against c = 26 km^2) has no digits left (c comes from the
/// altitude).
std::vector<SphereQuery> near_queries() {
    constexpr double radius = 1e4;
    constexpr double h = 0.0013;
    const math::dvec3 up = math::normalize(math::dvec3(3.0, -1.0, 2.0));
    const math::dvec3 horizontal = math::normalize(math::cross(up, math::dvec3(0.0, 0.0, 1.0)));
    const math::dvec3 o = up * (radius + h);
    std::vector<SphereQuery> out;
    for (const double dip : {1.0, 5.0, 30.0}) {
        const math::dvec3 d = math::normalize(horizontal * std::cos(dip * deg) - up * std::sin(dip * deg));
        const double c = h * (2.0 * radius + h);
        out.push_back({true, math::vec3(o), math::vec3(d), static_cast<float>(c), static_cast<float>(radius),
                       near_root(o, d, c)});
    }
    return out;
}

/// Sample points where the far rays meet the ground, computed in float as a shader would (so some are a few meters
/// under it), each with a sun `elevation` degrees above its local horizon.
std::vector<SphereQuery> shadow_queries(double elevation) {
    std::vector<SphereQuery> out;
    for (const SphereQuery& q : far_queries()) {
        const auto span = render::sphere_span(q.origin, q.direction, q.c);
        REQUIRE(span.has_value());
        const math::vec3 p = q.origin + q.direction * span->t0;
        const math::dvec3 n = math::normalize(math::dvec3(p));
        const math::dvec3 t = math::normalize(math::cross(n, math::dvec3(0.0, 1.0, 0.0)));
        const math::dvec3 l = n * std::sin(elevation * deg) + t * std::cos(elevation * deg);
        out.push_back({false, p, math::vec3(l), 0.0f, q.radius, 0.0});
    }
    return out;
}

/// Mirrors SphereProbeParams in tests/shaders/sphere_probe.slang.
struct SphereProbeParams {
    rhi::DescriptorHandle queries;
    rhi::DescriptorHandle results;
    std::uint32_t count = 0;
};
static_assert(rhi::parameters_match<SphereProbeParams>(test_shaders::layout::sphere_probe,
                                                       {VEACH_PARAM(SphereProbeParams, queries),
                                                        VEACH_PARAM(SphereProbeParams, results),
                                                        VEACH_PARAM(SphereProbeParams, count)}));

/// Runs `entry` of sphere_probe.slang over `count` queries packed in `packed` on the GPU: one vec4 per query.
std::vector<math::vec4> run_probe(rhi::Device& device, const char* entry, std::span<const math::vec4> packed,
                                  std::size_t count) {
    const std::uint64_t result_bytes = count * sizeof(math::vec4);
    auto input = device.create_buffer(
        {.size = packed.size() * sizeof(math::vec4), .debug_name = "sphere queries", .stride = sizeof(math::vec4)},
        std::as_bytes(std::span(packed)));
    auto results = device.create_buffer({.size = result_bytes,
                                         .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src,
                                         .debug_name = "sphere results",
                                         .stride = sizeof(math::vec4)});
    auto readback = device.create_buffer({.size = result_bytes,
                                          .usage = rhi::BufferUsage::copy_dst,
                                          .memory = rhi::MemoryType::readback,
                                          .debug_name = "sphere readback"});
    auto pipeline = device.create_compute_pipeline({&test_shaders::sphere_probe(), entry, {64, 1, 1}, "sphere probe"});
    REQUIRE(input.has_value());
    REQUIRE(results.has_value());
    REQUIRE(readback.has_value());
    REQUIRE_MESSAGE(pipeline.has_value(), (pipeline ? std::string() : pipeline.error().message));
    SphereProbeParams params;
    params.queries = device.descriptor(*input);
    params.results = device.descriptor(*results, rhi::View::storage);
    params.count = static_cast<std::uint32_t>(count);
    static_cast<void>(device.begin_frame());
    auto cmd = device.begin_commands("sphere probe");
    const std::array<rhi::Barrier, 1> to_write{
        {{*results, rhi::ResourceState::undefined, rhi::ResourceState::shader_write}}};
    cmd->barrier(to_write);
    cmd->begin_compute_pass("sphere probe");
    cmd->use(*input, rhi::ResourceState::shader_read);
    cmd->use(*results, rhi::ResourceState::shader_write);
    cmd->set_pipeline(*pipeline);
    cmd->set_parameters(params);
    cmd->dispatch(static_cast<std::uint32_t>((count + 63) / 64), 1);
    cmd->end_compute_pass();
    const std::array<rhi::Barrier, 2> to_copy{
        {{*results, rhi::ResourceState::shader_write, rhi::ResourceState::copy_src},
         {*readback, rhi::ResourceState::undefined, rhi::ResourceState::copy_dst}}};
    cmd->barrier(to_copy);
    cmd->copy_buffer(*results, *readback, result_bytes);
    const rhi::SubmissionId submission = device.submit(std::move(cmd));
    device.end_frame();
    device.wait(submission);
    const auto* mapped = static_cast<const math::vec4*>(device.map(*readback));
    REQUIRE(mapped != nullptr);
    std::vector<math::vec4> out(mapped, mapped + count);
    for (const rhi::BufferHandle b : {*input, *results, *readback}) {
        device.destroy(b);
    }
    device.destroy(*pipeline);
    device.wait_idle();
    return out;
}

/// Runs sphere_probe over `queries` on the GPU: (t0, t1, hit, blocks) per query.
std::vector<math::vec4> probe_on_gpu(rhi::Device& device, std::span<const SphereQuery> queries) {
    std::vector<math::vec4> packed;
    for (const SphereQuery& q : queries) {
        packed.emplace_back(q.origin, q.c);
        packed.emplace_back(q.direction, q.radius);
    }
    return run_probe(device, "sphere_probe", packed, queries.size());
}

/// A ray in twilight and where it is in the planet's shadow, from a double-precision reference.
struct ShadowQuery {
    math::vec3 origin;
    math::vec3 direction;
    float c = 0.0f;
    math::vec3 l;
    bool in_shadow = false;
    double enter = 0.0;
    double leave = 0.0;
};

/// The shadow's span along origin + t * direction in double: the cylinder of `radius` behind the center along -l.
ShadowQuery shadow_reference(math::dvec3 o, math::dvec3 d, math::dvec3 l, double radius, double altitude) {
    ShadowQuery q{math::vec3(o), math::vec3(d), static_cast<float>(altitude * (2.0 * radius + altitude)),
                  math::vec3(l)};
    const math::dvec3 op = o - l * math::dot(o, l);
    const math::dvec3 dp = d - l * math::dot(d, l);
    const double a = math::dot(dp, dp);
    const double b = math::dot(op, dp);
    const double c = math::dot(op, op) - radius * radius;
    const double discriminant = b * b - a * c;
    if (a < 1e-12 || discriminant <= 0.0) {
        return q;
    }
    double enter = (-b - std::sqrt(discriminant)) / a;
    double leave = (-b + std::sqrt(discriminant)) / a;
    // Behind the plane through the center perpendicular to l.
    const double dl = math::dot(d, l);
    if (dl > 0.0) {
        leave = std::min(leave, -math::dot(o, l) / dl);
    } else if (dl < 0.0) {
        enter = std::max(enter, -math::dot(o, l) / dl);
    } else if (math::dot(o, l) >= 0.0) {
        return q;
    }
    q.in_shadow = enter < leave;
    q.enter = enter;
    q.leave = leave;
    return q;
}

/// Twilight rays at real scale: a camera 300 m up on a 6360 km planet with the sun 0.5 to 18 degrees below its
/// horizon, looking 0 to 80 degrees up, toward the sun, across and away; and from 1e4 km.
std::vector<ShadowQuery> shadow_span_queries() {
    constexpr double radius = 6360.0;
    std::vector<ShadowQuery> out;
    for (const double altitude : {0.3, 1e4 - radius}) {
        const math::dvec3 up = math::normalize(math::dvec3(1.0, 2.0, 3.0));
        const math::dvec3 east = math::normalize(math::cross(up, math::dvec3(0.0, 0.0, 1.0)));
        const math::dvec3 north = math::cross(east, up);
        const math::dvec3 o = up * (radius + altitude);
        for (const double sun : {-0.5, -3.0, -10.0, -18.0}) {
            const math::dvec3 l = east * std::cos(sun * deg) + up * std::sin(sun * deg);
            for (const double azimuth : {0.0, 60.0, 120.0, 180.0}) {
                for (const double elevation : {0.0, 5.0, 30.0, 80.0, -30.0}) {
                    const math::dvec3 horizontal = east * std::cos(azimuth * deg) + north * std::sin(azimuth * deg);
                    const math::dvec3 d =
                        math::normalize(horizontal * std::cos(elevation * deg) + up * std::sin(elevation * deg));
                    out.push_back(shadow_reference(o, d, l, radius, altitude));
                }
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("[atmosphere.AC1] the stable sphere span keeps both roots at 1e4 km in float kilometers") {
    for (const SphereQuery& q : far_queries()) {
        const auto span = render::sphere_span(q.origin, q.direction, q.c);
        REQUIRE(span.has_value());
        // A few float ulps of the 1e4 km scale (1e-3 km = 1 m): what SP-10's orbit views need.
        CHECK_MESSAGE(std::abs(span->t0 - q.reference) < 2e-3, "t0 ", span->t0, " reference ", q.reference);
    }
    for (const SphereQuery& q : near_queries()) {
        const auto span = render::sphere_span(q.origin, q.direction, q.c);
        REQUIRE(span.has_value());
        const double error = std::abs(span->t0 - q.reference) / q.reference;
        CHECK_MESSAGE(error < 1e-4, "near root ", span->t0, " reference ", q.reference);
        // The textbook c = |o|^2 - R^2 in float has lost the 1.3 m altitude: the reason c comes from the altitude.
        const float naive_c = math::dot(q.origin, q.origin) - q.radius * q.radius;
        const auto naive = render::sphere_span(q.origin, q.direction, naive_c);
        const double naive_error = naive ? std::abs(naive->t0 - q.reference) / q.reference : 1.0;
        CHECK(naive_error > 10.0 * std::max(error, 1e-7));
    }
}

TEST_CASE("[atmosphere.AC1] a ground point rounded under the surface does not shadow itself") {
    for (const double elevation : {0.5, 5.0, 45.0, 89.0}) {
        for (const SphereQuery& q : shadow_queries(elevation)) {
            CHECK_MESSAGE(!render::sphere_blocks(q.origin, q.direction, q.radius), "sun ", elevation, " deg up");
        }
    }
    for (const double elevation : {-0.5, -5.0, -60.0}) {
        for (const SphereQuery& q : shadow_queries(elevation)) {
            CHECK_MESSAGE(render::sphere_blocks(q.origin, q.direction, q.radius), "sun ", elevation, " deg down");
        }
    }
}

TEST_CASE("[atmosphere.AC1] the shader's sphere helpers match their CPU twins at 1e4 km (GPU)") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    std::vector<SphereQuery> queries = far_queries();
    for (const SphereQuery& q : near_queries()) {
        queries.push_back(q);
    }
    for (const double elevation : {0.5, -0.5}) {
        for (const SphereQuery& q : shadow_queries(elevation)) {
            queries.push_back(q);
        }
    }
    const std::vector<math::vec4> gpu = probe_on_gpu(*device, queries);
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const SphereQuery& q = queries[i];
        const auto cpu = render::sphere_span(q.origin, q.direction, q.c);
        if (q.span) {
            REQUIRE(cpu.has_value());
            CHECK(gpu[i].z == 1.0f);
            CHECK_MESSAGE(std::abs(gpu[i].x - cpu->t0) <= 1e-6f * std::max(1.0f, std::abs(cpu->t0)) + 1e-6f, "query ",
                          i, ": GPU t0 ", gpu[i].x, ", CPU ", cpu->t0);
            CHECK_MESSAGE(std::abs(gpu[i].y - cpu->t1) <= 1e-6f * std::max(1.0f, std::abs(cpu->t1)) + 1e-6f, "query ",
                          i, ": GPU t1 ", gpu[i].y, ", CPU ", cpu->t1);
        }
        CHECK(gpu[i].w == (render::sphere_blocks(q.origin, q.direction, q.radius) ? 1.0f : 0.0f));
    }
}

TEST_CASE("[atmosphere.AC1] the planet's shadow along a twilight ray matches a double reference, on the GPU too") {
    const std::vector<ShadowQuery> queries = shadow_span_queries();
    // Within a few float ulps of the ray's length where it crosses the air (1e-3 km = 1 m at 1e4 km). Ends farther than
    // 2e4 km (rays nearly along the shadow's axis, ill-conditioned) need only be that far, on the same side.
    const auto close = [](double got, double reference) {
        if (std::abs(reference) > 2e4) {
            return got * reference > 0.0 && std::abs(got) > 1e4;
        }
        return std::abs(got - reference) <= 2e-3 + 1e-6 * std::abs(reference);
    };
    int shadowed = 0;
    for (const ShadowQuery& q : queries) {
        const auto span = render::sphere_shadow_span(q.origin, q.direction, q.c, q.l);
        REQUIRE(span.has_value() == q.in_shadow);
        if (!span) {
            continue;
        }
        ++shadowed;
        CHECK_MESSAGE(close(span->t0, q.enter), "enter ", span->t0, " reference ", q.enter);
        CHECK_MESSAGE(close(span->t1, q.leave), "leave ", span->t1, " reference ", q.leave);
        for (const double f : {0.25, 0.5, 0.75}) {  // inside, by the point test
            const double t = q.enter + f * (q.leave - q.enter);
            if (t > 0.0 && t < 1e4) {
                CHECK(render::sphere_blocks(q.origin + q.direction * static_cast<float>(t), q.l, 6360.0f));
            }
        }
    }
    CHECK(shadowed > 20);  // most twilight rays cross the shadow somewhere

    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    std::vector<math::vec4> packed;
    for (const ShadowQuery& q : queries) {
        packed.emplace_back(q.origin, q.c);
        packed.emplace_back(q.direction, 0.0f);
        packed.emplace_back(q.l, 0.0f);
    }
    const std::vector<math::vec4> gpu = run_probe(*device, "shadow_probe", packed, queries.size());
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto cpu =
            render::sphere_shadow_span(queries[i].origin, queries[i].direction, queries[i].c, queries[i].l);
        CHECK_MESSAGE((gpu[i].x < gpu[i].y) == cpu.has_value(), "query ", i);
        if (cpu) {
            CHECK_MESSAGE(close(gpu[i].x, cpu->t0), "query ", i, ": GPU enter ", gpu[i].x, ", CPU ", cpu->t0);
            CHECK_MESSAGE(close(gpu[i].y, cpu->t1), "query ", i, ": GPU leave ", gpu[i].y, ", CPU ", cpu->t1);
        }
    }
}

TEST_CASE("[atmosphere.AC2] the volume's slices reach centimeters near the camera and crowd at the ground") {
    for (const float x : {1e-7f, 1e-5f, 1e-3f, 0.1f, 0.5f, 0.9f, 0.999f, 1.0f}) {
        CHECK(render::aerial_slice_x(render::aerial_slice_w(x)) == doctest::Approx(x).epsilon(1e-3));
    }
    float previous = -1.0f;
    for (int k = 0; k <= 64; ++k) {
        const float x = render::aerial_slice_x(static_cast<float>(k) / 64.0f);
        CHECK(x > previous);
        previous = x;
    }
    // 32 slices over a 1,130 km span (a ground camera's horizontal ray at real scale): the first slice center is
    // under a meter and the second within tens of meters (a cave's walls); the last one is within half a percent of
    // the span's end (the ground under the veil seen from orbit).
    constexpr float span_km = 1130.0f;
    CHECK(render::aerial_slice_x(0.5f / 32.0f) * span_km < 0.001f);
    CHECK(render::aerial_slice_x(1.5f / 32.0f) * span_km < 0.05f);
    CHECK(1.0f - render::aerial_slice_x(31.5f / 32.0f) < 0.005f);
}

TEST_CASE("[atmosphere.AC3] a medium's tables are built once and kept while it is in use") {
    render::AtmosphereLutCache cache;
    render::AtmosphereParams a;
    a.planet = math::vec4(6371.0f, 6471.0f, 0.3f, 15.0f);
    render::AtmosphereParams b = a;
    b.planet.x = 1737.0f;  // a moon
    const auto first = cache.acquire(a);
    CHECK(first.build);
    for (int frame = 0; frame < 10; ++frame) {
        CHECK_FALSE(cache.acquire(a).build);
    }
    const auto moon = cache.acquire(b);
    CHECK(moon.build);
    CHECK(moon.index != first.index);
    // Back to the planet and its moon again: nothing is rebuilt.
    CHECK_FALSE(cache.acquire(a).build);
    CHECK_FALSE(cache.acquire(b).build);
    CHECK(cache.builds() == 2);
    // More media than slots: the least recently used goes first.
    std::vector<render::AtmosphereParams> others;
    for (int i = 0; i < 3; ++i) {
        render::AtmosphereParams m = a;
        m.planet.x = 100.0f * static_cast<float>(i + 1);
        others.push_back(m);
        CHECK(cache.acquire(m).build);
    }
    CHECK(cache.builds() == 5);
    CHECK(cache.acquire(b).build == false);  // b was used after a
    CHECK(cache.acquire(a).build);           // a was the oldest: evicted by the third
}

TEST_CASE("[atmosphere.AC4] sky visibility from static geometry: a roof closes the sky, a craft does not") {
    using namespace veach::physics;
    PhysicsWorld world({});
    const SceneId scene = *world.create_scene({.name = "shelter"});
    const math::dvec3 point{0.0, 1.0, 0.0};
    const math::vec3 up{0.0f, 1.0f, 0.0f};
    // Open ground: everything open.
    const SkyOpenness open = sky_visibility(world, scene, point, up);
    CHECK(open.visibility == doctest::Approx(1.0f));
    CHECK(open.reach == 0.0f);
    // A dynamic box overhead (a craft) is not the static world: still open.
    REQUIRE(world.add_body(scene, {.shape = Shape::box(math::vec3(10.0f, 0.5f, 10.0f)),
                                   .motion = MotionType::dynamic,
                                   .position = math::dvec3(0.0, 4.5, 0.0)}));
    CHECK(sky_visibility(world, scene, point, up).visibility == doctest::Approx(1.0f));
    // A static roof 20 m across, 3 m over the point: about cos^2(73 deg) = 9 % of the cosine-weighted sky is open.
    REQUIRE(world.add_body(scene, {.shape = Shape::box(math::vec3(10.0f, 0.5f, 10.0f)),
                                   .motion = MotionType::static_body,
                                   .position = math::dvec3(0.0, 4.5, 0.0)}));
    const SkyOpenness roofed = sky_visibility(world, scene, point, up);
    CHECK(roofed.visibility < 0.2f);
    CHECK(roofed.visibility > 0.0f);
    CHECK(roofed.reach > 3.0f);
    CHECK(roofed.reach < 15.0f);
}

TEST_CASE("[atmosphere.AC4] a camera's SkyVisibility reaches its view, and without one the camera is in the open") {
    std::unique_ptr<rhi::Device> device = std::move(*rhi::create_device({}));
    render::Renderer renderer{*device};
    World world;
    world.import<CoreModule>();
    world.import<render::RenderModule>();
    world.entity("open").set(Transform{}).set(Camera{});
    world.entity("cave").set(Transform{}).set(Camera{}).set(SkyVisibility{.visibility = 0.01f, .reach = 25.0f});
    propagate_transforms(world);
    renderer.extract(world);
    const render::RenderWorld& rw = renderer.render_world();
    REQUIRE(rw.views.size() == 2);
    const auto& open = rw.views[0].camera < rw.views[1].camera ? rw.views[0] : rw.views[1];
    const auto& cave = rw.views[0].camera < rw.views[1].camera ? rw.views[1] : rw.views[0];
    CHECK(open.sky_visibility.visibility == 1.0f);
    CHECK(open.sky_visibility.reach == 0.0f);
    CHECK(cave.sky_visibility.visibility == 0.01f);
    CHECK(cave.sky_visibility.reach == 25.0f);
}
