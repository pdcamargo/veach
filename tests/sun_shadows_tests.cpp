// Sun shadows in the real-time renderer (docs/specs/sun-shadows/spec.md, ADR 0095): the cascades' geometry on the CPU
// (splits, stable fits, which lights cast), then a box shadowing a floor on the platform's GPU (Metal in the pre-push
// gate, DX12/WARP in hosted CI).

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"
#include "test_shaders_materials.hpp"

#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>
#include <veach/render/shadow_cascades.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <vector>

using namespace veach;

namespace {

/// A unit cube centered on the origin, flat-shaded, one material slot.
render::MeshData cube() {
    render::MeshData data;
    const std::array<math::vec3, 6> normals{math::vec3(1, 0, 0),  math::vec3(-1, 0, 0), math::vec3(0, 1, 0),
                                            math::vec3(0, -1, 0), math::vec3(0, 0, 1),  math::vec3(0, 0, -1)};
    for (const math::vec3& n : normals) {
        const math::vec3 u = std::abs(n.y) > 0.5f ? math::vec3(1, 0, 0) : math::vec3(0, 1, 0);
        const math::vec3 v = math::cross(n, u);
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (const auto& [a, b] :
             {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f}, std::pair{-1.0f, 1.0f}}) {
            const math::vec3 p = 0.5f * (n + a * u + b * v);
            MeshVertex vertex;
            vertex.position = {p.x, p.y, p.z};
            vertex.normal = {n.x, n.y, n.z};
            data.vertices.push_back(vertex);
        }
        // Counter-clockwise seen from outside.
        const bool flip = math::dot(math::cross(u, v), n) < 0.0f;
        const std::array<std::uint32_t, 6> quad =
            flip ? std::array<std::uint32_t, 6>{0, 2, 1, 0, 3, 2} : std::array<std::uint32_t, 6>{0, 1, 2, 0, 2, 3};
        for (const std::uint32_t i : quad) {
            data.indices.push_back(base + i);
        }
    }
    return data;
}

/// A camera at `position` looking along `forward` (unit), 60 degrees vertically, 16:9, as extraction builds it.
render::RenderView view_at(math::vec3 position, math::vec3 forward) {
    render::RenderView view;
    const math::quat rotation =
        math::quatLookAtRH(forward, std::abs(forward.y) > 0.99f ? math::vec3(0, 0, -1) : math::vec3(0, 1, 0));
    math::mat4 world_from_camera = math::mat4_cast(rotation);
    world_from_camera[3] = math::vec4(position, 1.0f);
    view.view = math::inverse(world_from_camera);
    view.projection = math::perspective_reverse_z(math::radians(60.0f), 16.0f / 9.0f, 0.1f);
    view.view_projection = view.projection * view.view;
    view.inverse_view_projection = math::inverse(view.view_projection);
    view.position = position;
    view.near_plane = 0.1f;
    return view;
}

/// Where a render-space point lands in a cascade, in texels.
math::vec2 texel_of(const render::CascadeFit& fit, math::vec3 p, std::uint32_t resolution) {
    const math::vec4 clip = fit.clip_from_render * math::vec4(p, 1.0f);
    return math::vec2(clip.x * 0.5f + 0.5f, 0.5f - clip.y * 0.5f) * static_cast<float>(resolution);
}

float fraction_distance(float a, float b) {
    const float d = a - b;
    return std::abs(d - std::round(d));
}

}  // namespace

TEST_CASE("[shadows.AC1] cascades end at the shadow distance, each a quarter of the next") {
    const std::array<float, 4> four = render::cascade_splits(1000.0f, 4);
    CHECK(four[0] == doctest::Approx(15.625f));
    CHECK(four[1] == doctest::Approx(62.5f));
    CHECK(four[2] == doctest::Approx(250.0f));
    CHECK(four[3] == doctest::Approx(1000.0f));
    const std::array<float, 4> two = render::cascade_splits(80.0f, 2);
    CHECK(two[0] == doctest::Approx(20.0f));
    CHECK(two[1] == doctest::Approx(80.0f));
    CHECK(render::cascade_splits(10.0f, 9)[3] == doctest::Approx(10.0f));  // at most four
}

TEST_CASE("[shadows.AC2] a cascade keeps its size as the camera turns and its texel grid as it moves") {
    const math::vec3 l = math::normalize(math::vec3(0.3f, 0.8f, -0.4f));
    constexpr std::uint32_t resolution = 2048;
    const math::dvec3 origin(128.0, 0.0, -64.0);
    const render::CascadeFit a =
        render::fit_cascade(view_at({1.0f, 2.0f, 3.0f}, math::vec3(0, 0, -1)), origin, l, 0.1f, 15.625f, resolution);
    // Turning: the same sphere, so the same size and texel.
    const render::CascadeFit turned =
        render::fit_cascade(view_at({1.0f, 2.0f, 3.0f}, math::normalize(math::vec3(0.7f, -0.2f, -0.6f))), origin, l,
                            0.1f, 15.625f, resolution);
    CHECK(turned.radius == doctest::Approx(a.radius).epsilon(1e-6));
    CHECK(turned.texel == doctest::Approx(a.texel).epsilon(1e-6));

    // Moving by a fraction of a texel: a static point stays on the same spot within its texel.
    const math::vec3 q(2.5f, 0.0f, -6.0f);
    for (const float step : {0.13f, 0.5f, 3.7f, 11.0f}) {
        const render::CascadeFit b = render::fit_cascade(
            view_at(math::vec3(1.0f, 2.0f, 3.0f) + math::vec3(step * a.texel, 0.0f, 0.0f), math::vec3(0, 0, -1)),
            origin, l, 0.1f, 15.625f, resolution);
        const math::vec2 ta = texel_of(a, q, resolution);
        const math::vec2 tb = texel_of(b, q, resolution);
        CHECK(fraction_distance(ta.x, tb.x) < 0.01f);
        CHECK(fraction_distance(ta.y, tb.y) < 0.01f);
    }

    // Render space moving to the next 64 m cell (ADR 0045) with the camera where it was: the same texels.
    const math::vec3 cell(64.0f, 0.0f, 0.0f);
    const render::CascadeFit moved =
        render::fit_cascade(view_at(math::vec3(1.0f, 2.0f, 3.0f) - cell, math::vec3(0, 0, -1)),
                            origin + math::dvec3(cell), l, 0.1f, 15.625f, resolution);
    const math::vec2 before = texel_of(a, q, resolution);
    const math::vec2 after = texel_of(moved, q - cell, resolution);
    CHECK(after.x == doctest::Approx(before.x).epsilon(1e-4));
    CHECK(after.y == doctest::Approx(before.y).epsilon(1e-4));
}

TEST_CASE("[shadows.AC3] the two brightest directional lights cast, and points, spots and dark lights do not") {
    std::vector<render::RenderLight> lights(5);
    lights[0] = {.kind = LightKind::point, .radiance = math::vec3(1e6f)};
    lights[1] = {.kind = LightKind::directional, .radiance = math::vec3(10.0f)};
    lights[2] = {.kind = LightKind::directional, .radiance = math::vec3(0.0f)};
    lights[3] = {.kind = LightKind::directional, .radiance = math::vec3(1000.0f)};
    lights[4] = {.kind = LightKind::directional, .radiance = math::vec3(100.0f)};
    CHECK(render::shadowed_lights(lights, 2) == std::vector<std::uint32_t>{3, 4});
    CHECK(render::shadowed_lights(lights, 1) == std::vector<std::uint32_t>{3});
    CHECK(render::shadowed_lights(lights, 7) == std::vector<std::uint32_t>{3, 4});  // at most two
    CHECK(render::shadowed_lights(lights, 0).empty());
}

TEST_CASE("[shadows.AC4] a box shadows the floor under the sun, and casters outside the view's slices are not drawn") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> device = test::gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        test::TempProject project{"sun-shadows"};
        World world;
        AssetServer assets{world, project.path()};
        world.import<CoreModule>();
        world.component<Camera>();
        world.component<RuntimeMeshInstance>();
        world.component<DirectionalLight>();
        world.component<Sky>();
        world.component<SunShadows>();
        render::Renderer renderer(*device);
        auto owned = std::make_unique<render::SceneRenderer>(
            render::SceneShaders{test_shaders::path_tracer(), test_shaders::realtime_visibility(),
                                 test_shaders::realtime_shade_bins()},
            test_shaders::material_kinds, assets);
        render::SceneRenderer* scene = owned.get();
        renderer.add_feature(std::move(owned));
        render::MeshData data = cube();
        data.materials = {renderer.meshes().add_material(
            {.name = "grey", .base_color = {0.6f, 0.6f, 0.6f, 1.0f}, .metallic = 0.0f, .roughness = 0.8f})};
        const AssetId box = *renderer.meshes().add(data);
        const auto place = [&](const char* name, math::dvec3 at, math::vec3 scale) {
            world.entity(name)
                .set(Transform{at, math::quat(1.0f, 0.0f, 0.0f, 0.0f), scale})
                .set(RuntimeMeshInstance{box});
        };
        place("floor", {0.0, -0.1, 0.0}, {20.0f, 0.2f, 20.0f});
        place("caster", {0.0, 3.0, 0.0}, math::vec3(1.0f));
        place("far", {30.0, 1.0, 0.0}, math::vec3(1.0f));  // its shadow falls away from the view
        // The sun shines down and toward +X: light travels along (0.3, -1, 0).
        const math::vec3 travel = math::normalize(math::vec3(0.3f, -1.0f, 0.0f));
        world.entity("sun")
            .set(Transform{{0.0, 0.0, 0.0}, math::quatLookAtRH(travel, math::vec3(0, 0, 1)), math::vec3(1.0f)})
            .set(DirectionalLight{math::vec3(1.0f), 5.0f});
        world.entity("sky").set(Sky{.mode = SkyMode::constant, .color = math::vec3(0.2f, 0.3f, 0.5f)});
        const Entity camera =
            world.entity("camera")
                .set(Transform{{0.0, 10.0, 0.0},
                               math::angleAxis(-std::numbers::pi_v<float> / 2.0f, math::vec3(1, 0, 0)),
                               math::vec3(1.0f)})
                .set(Camera{math::radians(60.0f), 0.1f, true});
        const auto capture = [&] {
            propagate_transforms(world);
            renderer.extract(world);
            render::Image out;
            renderer.request_capture([&](Result<render::Image> image) {
                REQUIRE(image.has_value());
                out = std::move(*image);
            });
            renderer.render();
            return out;
        };
        const auto luminance_at = [&](const render::Image& image, math::vec3 p) {
            const math::mat4& vp = renderer.render_world().views.at(0).view_projection;
            const math::vec4 clip = vp * math::vec4(p, 1.0f);
            const auto x =
                static_cast<std::uint32_t>((0.5f + 0.5f * clip.x / clip.w) * static_cast<float>(image.width));
            const auto y =
                static_cast<std::uint32_t>((0.5f - 0.5f * clip.y / clip.w) * static_cast<float>(image.height));
            const std::size_t i = (std::size_t{y} * image.width + x) * 4;
            return 0.2126f * image.rgba[i] + 0.7152f * image.rgba[i + 1] + 0.0722f * image.rgba[i + 2];
        };
        // On the floor: in the box's shadow (x 0.25 to 1.55 m), and in the sun well away from it.
        const math::vec3 shaded(1.2f, 0.0f, 0.0f);
        const math::vec3 lit(-3.0f, 0.0f, 0.0f);
        const render::Image with = capture();
        // The sky's ambient light still reaches the shaded floor: the sun's share is gone.
        CHECK(luminance_at(with, shaded) < 0.7f * luminance_at(with, lit));
        renderer.readbacks().flush();
        const render::CullStats& stats = scene->cull_stats();
        REQUIRE(stats.shadow_meshlets_drawn.size() == 4);  // one sun, four cascades
        // The far box's shadow falls away from the nearest slice: culled there. The deeper slices lie under the
        // floor, which the sweep test cannot know, so they keep it (conservative).
        CHECK(stats.shadow_meshlets_drawn[0] == 2);
        for (const std::uint32_t drawn : stats.shadow_meshlets_drawn) {
            CHECK(drawn <= 3);
        }

        camera.set(SunShadows{.suns = 0});
        const render::Image without = capture();
        CHECK(luminance_at(without, shaded) == doctest::Approx(luminance_at(without, lit)).epsilon(0.05));
        device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[shadows.AC8] casters that shadow no visible pixel, or hide behind others from the sun, are not drawn") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> device = test::gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        test::TempProject project{"sun-shadow-culling"};
        World world;
        AssetServer assets{world, project.path()};
        world.import<CoreModule>();
        world.component<Camera>();
        world.component<RuntimeMeshInstance>();
        world.component<DirectionalLight>();
        world.component<Sky>();
        world.component<SunShadows>();
        render::Renderer renderer(*device);
        auto owned = std::make_unique<render::SceneRenderer>(
            render::SceneShaders{test_shaders::path_tracer(), test_shaders::realtime_visibility(),
                                 test_shaders::realtime_shade_bins()},
            test_shaders::material_kinds, assets);
        render::SceneRenderer* scene = owned.get();
        renderer.add_feature(std::move(owned));
        render::MeshData data = cube();
        data.materials = {renderer.meshes().add_material(
            {.name = "grey", .base_color = {0.6f, 0.6f, 0.6f, 1.0f}, .metallic = 0.0f, .roughness = 0.8f})};
        const AssetId box = *renderer.meshes().add(data);
        const auto place = [&](const char* name, math::dvec3 at, math::vec3 scale) {
            world.entity(name)
                .set(Transform{at, math::quat(1.0f, 0.0f, 0.0f, 0.0f), scale})
                .set(RuntimeMeshInstance{box});
        };
        // The sun travels along (1, -1, 0); the camera looks straight down from 30 m, so every receiver is in the
        // second cascade (view depths 14 to 62.5 m). A roof at 10 m hides the floor under it, x -21 to -6 m, from the
        // camera, and shadows the floor at x -4 to 6 m, which the camera sees.
        place("floor", {0.0, -0.1, 0.0}, {80.0f, 0.2f, 80.0f});
        place("roof", {-9.0, 10.0, 0.0}, {10.0f, 0.2f, 6.0f});
        // Under the roof as the sun sees it, its shadow on the visible floor at x = 4 m: hidden from the light.
        place("hidden", {3.0, 1.0, 0.0}, math::vec3(0.2f));
        // Clear of the roof toward the sun, but its shadow falls at x = -12 m, under the roof: no pixel sees it.
        place("unseen", {-13.0, 1.0, 0.0}, math::vec3(0.5f));
        const math::vec3 travel = math::normalize(math::vec3(1.0f, -1.0f, 0.0f));
        world.entity("sun")
            .set(Transform{{0.0, 0.0, 0.0}, math::quatLookAtRH(travel, math::vec3(0, 0, 1)), math::vec3(1.0f)})
            .set(DirectionalLight{math::vec3(1.0f), 5.0f});
        world.entity("sky").set(Sky{.mode = SkyMode::constant, .color = math::vec3(0.2f, 0.3f, 0.5f)});
        world.entity("camera")
            .set(Transform{{0.0, 30.0, 0.0},
                           math::angleAxis(-std::numbers::pi_v<float> / 2.0f, math::vec3(1, 0, 0)),
                           math::vec3(1.0f)})
            .set(Camera{math::radians(60.0f), 0.1f, true});
        const auto capture = [&] {
            propagate_transforms(world);
            renderer.extract(world);
            render::Image out;
            renderer.request_capture([&](Result<render::Image> image) {
                REQUIRE(image.has_value());
                out = std::move(*image);
            });
            renderer.render();
            return out;
        };
        const render::Image first = capture();
        render::Image last;
        for (int frame = 0; frame < 4; ++frame) {
            last = capture();
        }
        renderer.readbacks().flush();
        const render::CullStats& stats = scene->cull_stats();
        REQUIRE(stats.shadow_meshlets_drawn.size() == 4);
        // Only the floor and the roof cast into the second cascade once last frame's map shows the hidden box behind
        // the roof (the first frame has no such map and draws it); the first cascade has no receiver at all.
        CHECK(stats.shadow_meshlets_drawn[0] == 0);
        CHECK(stats.shadow_meshlets_drawn[1] == 2);
        // What the culling dropped changed no pixel.
        REQUIRE(first.rgba.size() == last.rgba.size());
        int worst = 0;
        for (std::size_t i = 0; i < first.rgba.size(); ++i) {
            worst = std::max(worst, std::abs(int{first.rgba[i]} - int{last.rgba[i]}));
        }
        CHECK(worst <= 1);
        device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
