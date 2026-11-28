// Bloom (docs/specs/bloom/spec.md): the chain's screen-relative plan on the CPU, and the glow on the platform's GPU
// (Metal in the pre-push gate, DX12/WARP in hosted CI): a sunlit white cube under a constant grey sky.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"
#include "test_shaders_materials.hpp"

#include <veach/platform/window.hpp>
#include <veach/render/bloom.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>

using namespace veach;

namespace {

/// The weighted mean of log2(texel size / image height) over the chain: where its glow sits on the screen.
float mean_octave(const render::BloomChain& chain) {
    float sum = 0.0f;
    for (std::uint32_t k = 0; k < chain.levels; ++k) {
        sum += chain.weights.at(k) * std::log2(chain.scale.at(k));
    }
    return sum;
}

float weight_sum(const render::BloomChain& chain) {
    float sum = 0.0f;
    for (std::uint32_t k = 0; k < chain.levels; ++k) {
        sum += chain.weights.at(k);
    }
    return sum;
}

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

/// A white cube in direct sunlight (about 8 stops above the sky) in the middle of a constant sky exposed to mid grey.
struct Scene {
    test::TempProject project{"bloom"};
    std::unique_ptr<rhi::Device> device;
    World world;
    AssetServer assets{world, project.path()};
    std::unique_ptr<render::Renderer> renderer;
    Entity camera;
    Entity window;

    explicit Scene(std::unique_ptr<rhi::Device> d) : device(std::move(d)) {
        world.import<CoreModule>();
        world.component<Camera>();
        world.component<RuntimeMeshInstance>();
        world.component<DirectionalLight>();
        world.component<Sky>();
        world.component<Bloom>();
        world.component<AntiAliasing>();
        world.component<Window>();
        world.component<PrimaryWindow>();
        renderer = std::make_unique<render::Renderer>(*device);
        renderer->add_feature(std::make_unique<render::SceneRenderer>(
            render::SceneShaders{test_shaders::path_tracer(), test_shaders::realtime_visibility(),
                                 test_shaders::realtime_shade_bins()},
            test_shaders::material_kinds, assets));
        render::MeshStore& meshes = renderer->meshes();
        render::MeshData data = cube();
        data.materials = {meshes.add_material(
            {.name = "white", .base_color = {0.8f, 0.8f, 0.8f, 1.0f}, .metallic = 0.0f, .roughness = 0.9f})};
        const AssetId mesh = *meshes.add(data);
        world.entity("cube")
            .set(Transform{{0.0, 0.0, -6.0}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(0.5f)})
            .set(RuntimeMeshInstance{mesh});
        world.entity("sky").set(Sky{.mode = SkyMode::constant, .color = math::vec3(1.0f)});
        // Straight from the camera's side: the cube's front face is lit head-on.
        world.entity("sun")
            .set(Transform{{0.0, 0.0, 0.0}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)})
            .set(DirectionalLight{.color = math::vec3(1.0f), .illuminance = 2000.0f});
        // 1 nit of sky at EV100 2.2 shows near mid grey; the cube's front, about 500 nits, far above AgX's white.
        camera = world.entity("camera")
                     .set(Camera{.fov_y = math::radians(60.0f), .near_plane = 0.1f, .ev100 = 2.2f})
                     .set(Transform{})
                     .set(AntiAliasing{.temporal = false});
        window = world.entity("window").add<PrimaryWindow>();
    }
    render::Image capture(std::uint32_t width, std::uint32_t height) {
        window.set(Window{.width = width, .height = height});
        render::Image out;
        renderer->request_capture([&](Result<render::Image> image) {
            REQUIRE(image.has_value());
            out = std::move(*image);
        });
        propagate_transforms(world);
        renderer->extract(world);
        renderer->render();
        REQUIRE(out.width == width);
        return out;
    }
    /// The mean green (0..255) of the 5 x 5 pixels at (u, v), fractions of the image's width and height.
    static float at(const render::Image& image, float u, float v) {
        const int cx = static_cast<int>(u * static_cast<float>(image.width));
        const int cy = static_cast<int>(v * static_cast<float>(image.height));
        float sum = 0.0f;
        for (int y = cy - 2; y <= cy + 2; ++y) {
            for (int x = cx - 2; x <= cx + 2; ++x) {
                sum += image.rgba[((static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4) + 1];
            }
        }
        return sum / 25.0f;
    }
};

}  // namespace

TEST_CASE("[bloom.AC1] the chain is screen-relative: the same glow at 1080p, 1440p and 2160p") {
    for (const float radius : {0.05f, 0.12f, 0.25f}) {
        const render::BloomChain hd = render::bloom_chain(1920, 1080, radius);
        const render::BloomChain qhd = render::bloom_chain(2560, 1440, radius);
        const render::BloomChain uhd = render::bloom_chain(3840, 2160, radius);
        for (const render::BloomChain* chain : {&hd, &qhd, &uhd}) {
            REQUIRE(chain->levels > 2);
            CHECK(weight_sum(*chain) == doctest::Approx(1.0f).epsilon(1e-5));
            CHECK(chain->blend.at(chain->levels - 1) == 1.0f);              // the coarsest level is its own result
            CHECK(chain->coverage == doctest::Approx(1.0f).epsilon(0.03));  // every size of the glow is there
            // No level wider than twice the radius: past it a level would weigh nothing.
            CHECK(chain->scale.at(chain->levels - 1) < 2.0f * radius);
        }
        CHECK(std::abs(mean_octave(qhd) - mean_octave(hd)) < 0.1f);
        CHECK(std::abs(mean_octave(uhd) - mean_octave(hd)) < 0.1f);
    }
    // The upsample's blends rebuild the normalized weights: level k's share is its blend times what the finer levels
    // left.
    const render::BloomChain chain = render::bloom_chain(1920, 1080, 0.12f);
    float left = 1.0f;
    for (std::uint32_t k = 0; k < chain.levels; ++k) {
        CHECK(chain.blend.at(k) * left == doctest::Approx(chain.weights.at(k)).epsilon(1e-4));
        left *= 1.0f - chain.blend.at(k);
    }
    // Under 1080 rows the finest sizes are missing: the glow keeps the share the coarser levels have at 1080p.
    const render::BloomChain small = render::bloom_chain(640, 360, 0.12f);
    CHECK(small.coverage < 0.9f);
    CHECK(small.coverage * small.weights.at(2) == doctest::Approx(chain.weights.at(3) * chain.coverage).epsilon(0.1));
    CHECK(render::bloom_chain(3, 3, 0.12f).levels == 0);  // too small to bloom
    CHECK(render::bloom_chain(1920, 1080, 0.0f).levels == 0);
}

TEST_CASE("[bloom.AC2] bloom spreads a bright cube's light around it and leaves a uniform sky as it was") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.camera.set(Bloom{.intensity = 0.0f});
    const render::Image off = f.capture(640, 360);
    f.camera.set(Bloom{.intensity = 0.04f, .radius = 0.05f});
    const render::Image on = f.capture(640, 360);
    // The cube covers about 0.08 of the height around the center. Beside it, the sky glows.
    const float near_off = Scene::at(off, 0.5f + 0.07f * 360.0f / 640.0f, 0.5f);
    const float near_on = Scene::at(on, 0.5f + 0.07f * 360.0f / 640.0f, 0.5f);
    CHECK(near_on > near_off + 8.0f);
    // Far from it (0.4 of the height away, eight times the radius), the uniform sky is unchanged.
    for (const auto& [u, v] : {std::pair{0.06f, 0.1f}, std::pair{0.94f, 0.9f}, std::pair{0.06f, 0.9f}}) {
        CHECK(std::abs(Scene::at(on, u, v) - Scene::at(off, u, v)) <= 1.0f);
    }
}

TEST_CASE("[bloom.AC3] the glow is the same at two resolutions, at the same distance on the screen") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    f.camera.set(Bloom{.intensity = 0.04f, .radius = 0.12f});
    const render::Image small = f.capture(640, 360);
    const render::Image large = f.capture(960, 540);
    float glow = 0.0f;
    for (const float d : {0.07f, 0.1f, 0.15f, 0.25f}) {  // fractions of the height from the cube's center
        const float u = 0.5f + d * 9.0f / 16.0f;
        CHECK(std::abs(Scene::at(small, u, 0.5f) - Scene::at(large, u, 0.5f)) <= 2.0f);
        glow = std::max(glow, Scene::at(small, u, 0.5f));
    }
    CHECK(glow > Scene::at(small, 0.05f, 0.1f) + 8.0f);  // there is a glow to compare
}
