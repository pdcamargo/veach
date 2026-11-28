// The real-time renderer's large-scale sky occlusion (docs/specs/sky-occlusion/spec.md), on the platform's GPU (Metal
// in the pre-push gate, DX12/WARP in hosted CI): runtime boxes on a ground under a sky, lit by the sky alone unless a
// test adds a sun. Each check compares the same view with the occlusion (the default) and without it
// (AmbientOcclusion::sky_distance = 0), screen-space ambient occlusion on in both.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"
#include "test_shaders_materials.hpp"

#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>

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
    test::TempProject project{"sky-occlusion"};
    std::unique_ptr<rhi::Device> device;
    World world;
    AssetServer assets{world, project.path()};
    std::unique_ptr<render::Renderer> renderer;
    Entity camera;
    AssetId grey;

    explicit Scene(std::unique_ptr<rhi::Device> d) : device(std::move(d)) {
        world.import<CoreModule>();
        world.component<Camera>();
        world.component<RuntimeMeshInstance>();
        world.component<DirectionalLight>();
        world.component<Sky>();
        world.component<AmbientOcclusion>();
        world.component<AntiAliasing>();
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
        camera = world.entity("camera").set(Camera{.fov_y = math::radians(60.0f), .near_plane = 0.1f, .ev100 = 12.0f});
        camera.set(AntiAliasing{.temporal = false});  // each frame its own image
        // The ground: a slab whose top is y = 0.
        place("ground", {0.0, -0.5, 0.0}, {400.0f, 1.0f, 400.0f});
    }
    Entity place(const char* name, math::dvec3 at, math::vec3 scale) {
        return world.entity(name)
            .set(Transform{at, math::quat(1.0f, 0.0f, 0.0f, 0.0f), scale})
            .set(RuntimeMeshInstance{grey});
    }
    void look(math::dvec3 eye, math::dvec3 target) {
        const math::vec3 f = math::normalize(math::vec3(target - eye));
        const math::vec3 r = math::normalize(math::cross(f, math::vec3(0.0f, 1.0f, 0.0f)));
        const math::vec3 u = math::cross(r, f);
        camera.set(Transform{eye, math::quat_cast(math::mat3(r, u, -f)), math::vec3(1.0f)});
    }
    render::Image capture(float sky_distance) {
        camera.set(AmbientOcclusion{.sky_distance = sky_distance});
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
    /// The mean green (0..1, display-encoded) of the pixels within `radius` of where world point `p` lands.
    float mean_at(const render::Image& image, math::vec3 p, int radius = 3) {
        const math::mat4& vp = renderer->render_world().views.front().view_projection;
        const math::vec4 clip = vp * math::vec4(p - math::vec3(renderer->render_world().space.origin), 1.0f);
        const int cx = static_cast<int>((0.5f + 0.5f * clip.x / clip.w) * static_cast<float>(image.width));
        const int cy = static_cast<int>((0.5f - 0.5f * clip.y / clip.w) * static_cast<float>(image.height));
        float sum = 0.0f;
        int n = 0;
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                REQUIRE(x >= 0);
                REQUIRE(y >= 0);
                REQUIRE(x < static_cast<int>(image.width));
                REQUIRE(y < static_cast<int>(image.height));
                sum +=
                    static_cast<float>(
                        image.rgba[(static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4 + 1]) /
                    255.0f;
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

constexpr float on = 200.0f;  // AmbientOcclusion's default reach
constexpr float off = 0.0f;

}  // namespace

TEST_CASE("[skyocc.AC1] the walls of a street see the sky above the buildings across it, the open ones the whole sky") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(sky_only(12.0f));
    // A street 6 m wide between two blocks 16 m tall and 40 m long; a lone block 40 m away in the open.
    f.place("west", {-5.0, 8.0, -20.0}, {4.0f, 16.0f, 40.0f});
    f.place("east", {5.0, 8.0, -20.0}, {4.0f, 16.0f, 40.0f});
    f.place("lone", {45.0, 8.0, -20.0}, {4.0f, 16.0f, 4.0f});
    f.look({0.0, 1.7, 0.0}, {-3.0, 1.0, -12.0});
    const math::vec3 street(-2.95f, 1.0f, -12.0f);  // the west block's face at the foot of the street
    const render::Image with = f.capture(on);
    const render::Image without = f.capture(off);
    CHECK(f.mean_at(with, street) < 0.8f * f.mean_at(without, street));
    f.look({30.0, 1.7, -20.0}, {43.0, 1.0, -20.0});
    const math::vec3 open(42.95f, 2.0f, -20.0f);  // the lone block's west face, 37 m of open ground in front
    const render::Image open_with = f.capture(on);
    const render::Image open_without = f.capture(off);
    CHECK(f.mean_at(open_with, open) > 0.97f * f.mean_at(open_without, open));
}

TEST_CASE("[skyocc.AC2] a surface under a roof loses the sky the roof hides and keeps the sky under its edge") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(sky_only(12.0f));
    // A roof 20 x 20 m, 4 m up, over open ground (no walls): a ceiling with the sky around it.
    f.place("roof", {0.0, 4.1, -14.0}, {20.0f, 0.2f, 20.0f});
    f.place("box", {0.0, 0.5, -14.0}, math::vec3(1.0f));
    f.look({0.0, 2.0, -2.0}, {0.0, 0.5, -14.0});
    const math::vec3 top(0.0f, 1.0f, -13.8f);    // the box's top, facing the roof
    const math::vec3 side(0.0f, 0.6f, -13.49f);  // its side, facing the open sky under the roof's edge
    const render::Image with = f.capture(on);
    const render::Image without = f.capture(off);
    CHECK(f.mean_at(with, top) < 0.7f * f.mean_at(without, top));
    CHECK(f.mean_at(with, side) > f.mean_at(with, top));
}

TEST_CASE("[skyocc.AC3] a closed chamber is dark inside: its walls get neither the sky nor the ground's bounce") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(sky_only(12.0f));
    // A chamber 10 x 6 x 30 m inside, walls and roof 1 m thick, open at its south end (z = 0). The roof rests between
    // the walls: a height map cannot tell a wall from air under a roof that overlaps it (the spec's limits).
    f.place("roof", {0.0, 6.5, -15.0}, {10.0f, 1.0f, 30.0f});
    f.place("west", {-5.5, 3.5, -15.0}, {1.0f, 7.0f, 30.0f});
    f.place("east", {5.5, 3.5, -15.0}, {1.0f, 7.0f, 30.0f});
    f.place("back", {0.0, 3.5, -30.5}, {12.0f, 7.0f, 1.0f});
    f.look({0.0, 1.7, -12.0}, {0.0, 2.0, -30.0});
    const math::vec3 back(0.0f, 2.0f, -29.95f);  // the back wall, 30 m in
    const render::Image with = f.capture(on);
    const render::Image without = f.capture(off);
    CHECK(f.mean_at(with, back) < 0.5f * f.mean_at(without, back));
}

TEST_CASE("[skyocc.AC4] beyond its reach, or with sky_distance 0, the image is the one without it") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(sky_only(12.0f));
    // The street of AC1, 400 m from the camera (the reach is 200 m).
    f.place("west", {-5.0, 8.0, -420.0}, {4.0f, 16.0f, 40.0f});
    f.place("east", {5.0, 8.0, -420.0}, {4.0f, 16.0f, 40.0f});
    f.look({0.0, 30.0, 0.0}, {0.0, 1.0, -420.0});
    const render::Image with = f.capture(on);
    const render::Image without = f.capture(off);
    // Equal but for rounding: no channel of any pixel more than one display level apart.
    int apart = 0;
    for (std::size_t i = 0; i < with.rgba.size(); ++i) {
        apart += std::abs(int(with.rgba[i]) - int(without.rgba[i])) > 1 ? 1 : 0;
    }
    CHECK(apart == 0);
}

TEST_CASE("[skyocc.AC5] a still camera's image never changes, whether the height map is drawn again or kept") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.world.entity("sky").set(sky_only(17.0f));
    f.world.entity("sun")
        .set(Transform{{0.0, 0.0, 0.0}, math::angleAxis(-0.4f, math::vec3(1.0f, 0.3f, 0.0f)), math::vec3(1.0f)})
        .set(DirectionalLight{math::vec3(1.0f), 5.0f});
    f.place("west", {-5.0, 8.0, -20.0}, {4.0f, 16.0f, 40.0f});
    f.place("east", {5.0, 8.0, -20.0}, {4.0f, 16.0f, 40.0f});
    f.look({0.0, 1.7, 0.0}, {-3.0, 1.0, -12.0});
    const render::Image first = f.capture(on);
    // The map is drawn on the first frame and kept while the camera stays.
    for (int i = 0; i < 3; ++i) {
        CHECK(f.capture(on).rgba == first.rgba);
    }
    // Away and back: the map is drawn again around the camera each time, on the same texel grid.
    f.look({0.0, 1.7, 60.0}, {-3.0, 1.0, -12.0});
    (void)f.capture(on);
    f.look({0.0, 1.7, 0.0}, {-3.0, 1.0, -12.0});
    CHECK(f.capture(on).rgba == first.rgba);
}
