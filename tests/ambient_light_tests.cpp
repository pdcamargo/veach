// The real-time renderer's ambient light and occlusion (docs/specs/ambient-light/spec.md, ADR 0096), on the platform's
// GPU (Metal in the pre-push gate, DX12/WARP in hosted CI): runtime boxes under a sky, lit by the sky alone unless a
// test adds a sun.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"
#include "test_shaders_materials.hpp"

#include <veach/render/atmosphere.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <numbers>

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
        const bool flip = math::dot(math::cross(u, v), n) < 0.0f;
        const std::array<std::uint32_t, 6> quad =
            flip ? std::array<std::uint32_t, 6>{0, 2, 1, 0, 3, 2} : std::array<std::uint32_t, 6>{0, 1, 2, 0, 2, 3};
        for (const std::uint32_t i : quad) {
            data.indices.push_back(base + i);
        }
    }
    return data;
}

struct Scene {
    test::TempProject project{"ambient-light"};
    std::unique_ptr<rhi::Device> device;
    World world;
    AssetServer assets{world, project.path()};
    std::unique_ptr<render::Renderer> renderer;
    Entity camera;
    AssetId grey;
    AssetId metal;

    explicit Scene(std::unique_ptr<rhi::Device> d) : device(std::move(d)) {
        world.import<CoreModule>();
        world.component<Camera>();
        world.component<RuntimeMeshInstance>();
        world.component<DirectionalLight>();
        world.component<Sky>();
        world.component<AmbientOcclusion>();
        renderer = std::make_unique<render::Renderer>(*device);
        renderer->add_feature(std::make_unique<render::SceneRenderer>(
            render::SceneShaders{test_shaders::path_tracer(), test_shaders::realtime_visibility(),
                                 test_shaders::realtime_shade_bins()},
            test_shaders::material_kinds, assets));
        render::MeshStore& meshes = renderer->meshes();
        render::MeshData data = cube();
        data.materials = {meshes.add_material(
            {.name = "grey", .base_color = {0.6f, 0.6f, 0.6f, 1.0f}, .metallic = 0.0f, .roughness = 0.8f})};
        grey = *meshes.add(data);
        data.materials = {meshes.add_material(
            {.name = "metal", .base_color = {0.9f, 0.9f, 0.9f, 1.0f}, .metallic = 1.0f, .roughness = 0.3f})};
        metal = *meshes.add(data);
        camera = world.entity("camera").set(Camera{.fov_y = math::radians(60.0f), .near_plane = 0.1f, .ev100 = 0.0f});
    }
    Entity place(const char* name, math::dvec3 at, math::vec3 scale, AssetId mesh) {
        return world.entity(name)
            .set(Transform{at, math::quat(1.0f, 0.0f, 0.0f, 0.0f), scale})
            .set(RuntimeMeshInstance{mesh});
    }
    void look(math::dvec3 eye, math::dvec3 target) {
        const math::vec3 f = math::normalize(math::vec3(target - eye));
        const math::vec3 r = math::normalize(math::cross(f, math::vec3(0.0f, 1.0f, 0.0f)));
        const math::vec3 u = math::cross(r, f);
        camera.set(Transform{eye, math::quat_cast(math::mat3(r, u, -f)), math::vec3(1.0f)});
    }
    render::Image capture() {
        render::Image out;
        renderer->request_capture([&](Result<render::Image> image) {
            REQUIRE(image.has_value());
            out = std::move(*image);
        });
        propagate_transforms(world);
        renderer->extract(world);
        renderer->render();
        return out;
    }
    /// The mean color (0..1, display-encoded) of the pixels within `radius` of where world point `p` lands.
    math::vec3 mean_at(const render::Image& image, math::vec3 p, int radius = 3) {
        const math::mat4& vp = renderer->render_world().views.front().view_projection;
        const math::vec4 clip = vp * math::vec4(p - math::vec3(renderer->render_world().space.origin), 1.0f);
        const int cx = static_cast<int>((0.5f + 0.5f * clip.x / clip.w) * static_cast<float>(image.width));
        const int cy = static_cast<int>((0.5f - 0.5f * clip.y / clip.w) * static_cast<float>(image.height));
        math::vec3 sum(0.0f);
        int n = 0;
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                REQUIRE(x >= 0);
                REQUIRE(y >= 0);
                REQUIRE(x < static_cast<int>(image.width));
                REQUIRE(y < static_cast<int>(image.height));
                const std::size_t i = (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4;
                sum += math::vec3(image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]) / 255.0f;
                ++n;
            }
        }
        return sum / static_cast<float>(n);
    }
};

/// A physical sky that lights the scene only through its ambient light: its sun lights the air but is no light.
Sky sky_only(float time_of_day) {
    return Sky{.mode = SkyMode::physical, .drives_sun = false, .time_of_day = time_of_day, .aerial_perspective = false};
}

}  // namespace

TEST_CASE("[ambient.AC1] the sky's ambient light depends on the normal: a wall sees the ground, a roof the blue sky") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(sky_only(12.0f));
    f.place("box", {0.0, 0.0, -4.0}, math::vec3(2.0f), f.grey);
    f.camera.set(Camera{.fov_y = math::radians(60.0f), .near_plane = 0.1f, .ev100 = 12.0f});
    f.camera.set(AmbientOcclusion{.enabled = false});
    f.look({0.0, 2.5, 0.0}, {0.0, 0.0, -4.0});
    const render::Image image = f.capture();
    const math::vec3 roof = f.mean_at(image, {0.0f, 1.0f, -4.0f});
    const math::vec3 wall = f.mean_at(image, {0.0f, -0.3f, -3.0f});
    // The same grey under one sky: the roof faces the blue zenith, the wall half the sky and half the grey ground.
    CHECK(roof.z / roof.x > 1.05f * (wall.z / wall.x));
    CHECK(wall.x > 0.02f);  // the side faces are lit, not black
}

TEST_CASE("[ambient.AC2] a surface where the sun has set gets the night's ambient light under the camera's day sky") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    // The sun 5 degrees up at the camera (latitude 0, the equinox, 17:40). Two boxes 1,500 km away, 13.5 degrees
    // around the planet's curve (the Sky's world is flat, its air a sphere of 6,360 km under the camera): toward the
    // sun, it is 18 degrees up where the box stands; opposite, 8 degrees down. Each box is lowered by the curve's drop
    // so its face sits on the ground. No haze in between, no direct light.
    Scene f(std::move(device));
    Sky sky = sky_only(17.67f);
    sky.latitude = 0.0f;
    sky.day_of_year = 80.0f;
    f.world.entity("sky").set(sky);
    const math::vec3 sun = render::sun_direction(sky);
    REQUIRE(sun.y > 0.05f);
    REQUIRE(sun.y < 0.12f);
    const math::vec3 toward = math::normalize(math::vec3(sun.x, 0.0f, sun.z));
    constexpr double distance = 1.5e6;
    const double drop = distance * distance / (2.0 * 6.36e6);
    const math::dvec3 day = math::dvec3(toward) * distance - math::dvec3(0.0, drop, 0.0);
    const math::dvec3 night = -math::dvec3(toward) * distance - math::dvec3(0.0, drop, 0.0);
    f.place("day", day, math::vec3(2e5f), f.grey);
    f.place("night", night, math::vec3(2e5f), f.grey);
    f.camera.set(Camera{.fov_y = math::radians(30.0f), .near_plane = 1.0f, .ev100 = 12.0f});
    f.look({0.0, 10.0, 0.0}, night);
    const render::Image night_image = f.capture();
    const math::vec3 dark = f.mean_at(night_image, math::vec3(night - math::dvec3(toward) * -1e5), 4);
    f.look({0.0, 10.0, 0.0}, day);
    const render::Image day_image = f.capture();
    const math::vec3 lit = f.mean_at(day_image, math::vec3(day - math::dvec3(toward) * 1e5), 4);
    CHECK(lit.y > 0.1f);
    CHECK(dark.y < 0.1f * lit.y);
}

TEST_CASE("[ambient.AC3] metals reflect the sky: a smooth metal box lit by the sky alone is not black") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(sky_only(12.0f));
    f.place("box", {0.0, 0.0, -4.0}, math::vec3(2.0f), f.metal);
    f.camera.set(Camera{.fov_y = math::radians(60.0f), .near_plane = 0.1f, .ev100 = 12.0f});
    f.look({0.0, 2.5, 0.0}, {0.0, 0.0, -4.0});
    const render::Image image = f.capture();
    CHECK(f.mean_at(image, {0.0f, 1.0f, -4.0f}).y > 0.05f);   // the roof reflects the sky
    CHECK(f.mean_at(image, {0.0f, -0.3f, -3.0f}).y > 0.02f);  // the wall reflects the horizon and the ground
}

TEST_CASE("[ambient.AC4] ambient occlusion darkens the corner where a box meets a wall, and nothing in the open") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(Sky{.mode = SkyMode::constant, .color = math::vec3(1.0f)});
    f.place("wall", {0.0, 0.0, -5.0}, {14.0f, 10.0f, 0.2f}, f.grey);
    f.place("box", {0.0, 0.0, -4.4f}, math::vec3(1.0f), f.grey);  // touching the wall's face at z = -4.9
    f.look({1.5, 1.0, 0.0}, {0.0, 0.0, -4.9});
    const math::vec3 side(0.55f, 0.0f, -4.88f);   // on the wall, next to the box's side
    const math::vec3 above(0.0f, 0.55f, -4.88f);  // on the wall, just above the box's top
    const math::vec3 open(-2.5f, 2.0f, -4.88f);   // on the wall, 3 m from the box
    const render::Image with = f.capture();
    f.camera.set(AmbientOcclusion{.enabled = false});
    const render::Image without = f.capture();
    // Display values (AgX): 0.95 is about 0.88 in light.
    CHECK(f.mean_at(with, side).y < 0.95f * f.mean_at(without, side).y);
    CHECK(f.mean_at(with, above).y < 0.95f * f.mean_at(without, above).y);
    CHECK(f.mean_at(with, open).y > 0.97f * f.mean_at(without, open).y);
}

TEST_CASE("[ambient.AC5] ambient occlusion never touches direct light, and a still camera's image never changes") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(Sky{.mode = SkyMode::none});
    f.world.entity("sun")
        .set(Transform{{0.0, 0.0, 0.0}, math::angleAxis(-1.0f, math::vec3(1.0f, 0.2f, 0.0f)), math::vec3(1.0f)})
        .set(DirectionalLight{math::vec3(1.0f), 5.0f});
    f.place("wall", {0.0, 0.0, -5.0}, {14.0f, 10.0f, 0.2f}, f.grey);
    f.place("box", {0.0, 0.0, -4.4f}, math::vec3(1.0f), f.grey);
    f.look({1.5, 1.0, 0.0}, {0.0, 0.0, -4.9});
    const render::Image first = f.capture();
    const render::Image second = f.capture();
    CHECK(first.rgba == second.rgba);  // a fixed pattern: nothing changes from frame to frame
    f.camera.set(AmbientOcclusion{.enabled = false});
    const render::Image off = f.capture();
    CHECK(first.rgba == off.rgba);  // no sky, so no ambient light: occlusion has nothing to darken
}
