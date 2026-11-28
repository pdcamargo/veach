// The GPU-driven real-time renderer (docs/specs/gpu-driven-renderer/spec.md, ADR 0093): the GPU scene's uploads on
// the null RHI, then culling, two-phase occlusion and motion vectors on the platform's GPU (Metal in the pre-push
// gate, DX12/WARP in hosted CI), on a small scene of runtime boxes: a wall, a box in front of it, one hidden behind it
// and one behind the camera.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"
#include "test_shaders_materials.hpp"

#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
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

/// The scene: a wall at z = -5, a box in front of it, one hidden behind it and one behind the camera.
struct Scene {
    test::TempProject project{"gpu-driven"};
    std::unique_ptr<rhi::Device> device;
    World world;
    AssetServer assets{world, project.path()};
    std::unique_ptr<render::Renderer> renderer;
    render::SceneRenderer* scene = nullptr;
    Entity camera;
    Entity front;
    Entity hidden;

    explicit Scene(std::unique_ptr<rhi::Device> d) : device(std::move(d)) {
        world.import<CoreModule>();
        world.component<Camera>();
        world.component<RuntimeMeshInstance>();
        world.component<DirectionalLight>();
        world.component<Sky>();
        renderer = std::make_unique<render::Renderer>(*device);
        auto owned = std::make_unique<render::SceneRenderer>(
            render::SceneShaders{test_shaders::path_tracer(), test_shaders::realtime_visibility(),
                                 test_shaders::realtime_shade_bins()},
            test_shaders::material_kinds, assets);
        scene = owned.get();
        renderer->add_feature(std::move(owned));
        render::MeshStore& meshes = renderer->meshes();
        render::MeshData data = cube();
        data.materials = {meshes.add_material(
            {.name = "grey", .base_color = {0.6f, 0.6f, 0.6f, 1.0f}, .metallic = 0.0f, .roughness = 0.8f})};
        const AssetId box = *meshes.add(data);
        const auto place = [&](const char* name, math::dvec3 at, math::vec3 scale) {
            return world.entity(name)
                .set(Transform{at, math::quat(1.0f, 0.0f, 0.0f, 0.0f), scale})
                .set(RuntimeMeshInstance{box});
        };
        place("wall", {0.0, 0.0, -5.0}, {14.0f, 10.0f, 0.2f});
        front = place("front", {0.0, -0.5, -3.0}, math::vec3(1.0f));
        hidden = place("hidden", {2.0, 0.0, -10.0}, math::vec3(1.0f));
        place("behind", {0.0, 0.0, 5.0}, math::vec3(1.0f));
        world.entity("sun")
            .set(Transform{{0.0, 0.0, 0.0}, math::angleAxis(-1.0f, math::vec3(1.0f, 0.2f, 0.0f)), math::vec3(1.0f)})
            .set(DirectionalLight{math::vec3(1.0f), 5.0f});
        world.entity("sky").set(Sky{.mode = SkyMode::constant, .color = math::vec3(0.2f, 0.3f, 0.5f)});
        camera = world.entity("camera")
                     .set(Transform{{0.0, 0.0, 0.0}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)})
                     .set(Camera{math::radians(60.0f), 0.1f, true});
    }
    void look_from(math::dvec3 position, float yaw) {
        camera.set(Transform{position, math::angleAxis(yaw, math::vec3(0.0f, 1.0f, 0.0f)), math::vec3(1.0f)});
    }
    void frame() {
        propagate_transforms(world);
        renderer->extract(world);
        renderer->render();
    }
    /// This frame's image.
    render::Image capture() {
        render::Image out;
        renderer->request_capture([&](Result<render::Image> image) {
            REQUIRE(image.has_value());
            out = std::move(*image);
        });
        frame();
        return out;
    }
    /// The culling statistics of the frame just rendered.
    const render::CullStats& stats() {
        renderer->readbacks().flush();
        return scene->cull_stats();
    }
};

/// Copies the "motion" target of each frame into a readback buffer (ADR 0060's frame blackboard).
class MotionProbe final : public render::RenderFeature {
public:
    [[nodiscard]] std::string_view name() const override { return "test.motion_probe"; }
    [[nodiscard]] Result<> prepare(rhi::Device& device) override {
        device_ = &device;
        return {};
    }
    void record(render::RenderGraph& graph, const render::FrameContext& frame) override {
        const std::optional<render::GraphResource> motion = frame.blackboard->get("motion");
        REQUIRE(motion.has_value());
        width_ = frame.width;
        height_ = frame.height;
        if (!buffer_.is_valid()) {
            buffer_ = *device_->create_buffer({std::uint64_t{width_} * height_ * 4, rhi::BufferUsage::copy_dst,
                                               rhi::MemoryType::readback, "motion probe"});
        }
        const render::GraphResource target = graph.import(buffer_, "motion probe", rhi::ResourceState::undefined);
        graph.add_transfer_pass(
            "motion probe",
            [&](render::PassBuilder& b) {
                b.read(*motion, rhi::ResourceState::copy_src);
                b.write(target, rhi::ResourceState::copy_dst);
            },
            [source = *motion, target](render::PassContext& ctx) {
                ctx.cmd.copy_texture_to_buffer(ctx.texture(source), ctx.buffer(target));
            });
    }
    ~MotionProbe() override {
        if (buffer_.is_valid()) {
            device_->wait_idle();
            device_->destroy(buffer_);
        }
    }
    MotionProbe() = default;
    MotionProbe(const MotionProbe&) = delete;
    MotionProbe& operator=(const MotionProbe&) = delete;
    MotionProbe(MotionProbe&&) = delete;
    MotionProbe& operator=(MotionProbe&&) = delete;

    /// The last frame's motion at a pixel (after the GPU finished it), UV units.
    math::vec2 at(std::uint32_t x, std::uint32_t y) {
        device_->wait_idle();
        const auto* halves = static_cast<const std::uint16_t*>(device_->map(buffer_));
        REQUIRE(halves != nullptr);
        const std::size_t i = (std::size_t{y} * width_ + x) * 2;
        return {half_to_float(halves[i]), half_to_float(halves[i + 1])};
    }
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }

private:
    static float half_to_float(std::uint16_t h) {
        const std::uint32_t sign = (h >> 15) & 1u;
        const std::uint32_t exponent = (h >> 10) & 31u;
        const std::uint32_t mantissa = h & 1023u;
        float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                                    : std::ldexp(static_cast<float>(mantissa | 1024u), static_cast<int>(exponent) - 25);
        return sign != 0 ? -value : value;
    }
    rhi::Device* device_ = nullptr;
    rhi::BufferHandle buffer_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

/// Where a point lands on screen, in UV (y down).
math::vec2 screen_uv(const math::mat4& view_projection, math::vec3 p) {
    const math::vec4 clip = view_projection * math::vec4(p, 1.0f);
    return {0.5f + 0.5f * clip.x / clip.w, 0.5f - 0.5f * clip.y / clip.w};
}

}  // namespace

TEST_CASE("[gpudriven.AC2] the GPU scene uploads the records that changed, and nothing for a still scene") {
    Scene f(std::move(*rhi::create_device({})));
    constexpr std::uint64_t record = 240;
    f.frame();
    CHECK(f.scene->instance_bytes_uploaded() == 4 * record);  // every record, the first time
    f.frame();
    CHECK(f.scene->instance_bytes_uploaded() == 0);  // nothing moved; neither did the camera's 64 m cell
    f.look_from({1.0, 0.5, 2.0}, 0.3f);
    f.frame();
    CHECK(f.scene->instance_bytes_uploaded() == 0);  // the camera moved inside its render cell
    f.front.set(Transform{{0.5, -0.5, -3.0}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)});
    f.frame();
    CHECK(f.scene->instance_bytes_uploaded() == record);  // the box that moved (its transform and last frame's)
    f.frame();
    CHECK(f.scene->instance_bytes_uploaded() == record);  // it stopped: last frame's transform caught up
    f.frame();
    CHECK(f.scene->instance_bytes_uploaded() == 0);
    f.look_from({100.0, 0.0, 0.0}, 0.0f);  // into another 64 m cell: render space moved, every record with it
    f.frame();
    CHECK(f.scene->instance_bytes_uploaded() == 4 * record);
}

TEST_CASE("[gpudriven.AC3] culling drops what is outside the frustum or hidden, and the image does not change") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    const render::Image first = f.capture();  // no pyramid yet: only the frustum culls
    const render::CullStats& early = f.stats();
    CHECK(early.instances == 4);
    CHECK(early.instances_visible == 3);  // the box behind the camera is outside the frustum
    CHECK(early.instances_rechecked == 0);
    for (int i = 0; i < 3; ++i) {
        f.frame();
    }
    const render::Image later = f.capture();
    const render::CullStats& stats = f.stats();
    CHECK(stats.instances_visible == 2);      // the wall and the box in front of it
    CHECK(stats.instances_rechecked == 1);    // the box behind the wall, against last frame's pyramid...
    CHECK(stats.instances_disoccluded == 0);  // ...and still hidden against this frame's
    CHECK(stats.meshlets_drawn[0] == 2);
    CHECK(stats.meshlets_drawn[1] == 0);
    REQUIRE(first.rgba.size() == later.rgba.size());
    CHECK(first.rgba == later.rgba);
}

TEST_CASE("[gpudriven.AC4] a camera that jumps to see what was hidden sees it the same frame") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    for (int i = 0; i < 3; ++i) {
        f.frame();
    }
    // Around the end of the wall, looking back at the hidden box: last frame's pyramid says it is occluded.
    f.look_from({9.5, 0.0, -9.0}, std::numbers::pi_v<float> / 2.0f);
    const render::Image jumped = f.capture();
    const render::CullStats& stats = f.stats();
    CHECK(stats.instances_disoccluded >= 1);
    CHECK(stats.meshlets_drawn[1] >= 1);

    auto other = test::gpu_device();
    REQUIRE(other != nullptr);
    Scene fresh(std::move(other));
    fresh.look_from({9.5, 0.0, -9.0}, std::numbers::pi_v<float> / 2.0f);
    const render::Image reference = fresh.capture();  // its first frame: no pyramid, nothing occluded
    REQUIRE(jumped.rgba.size() == reference.rgba.size());
    CHECK(jumped.rgba == reference.rgba);
}

TEST_CASE("[gpudriven.AC5] the visibility pass writes motion vectors: where each pixel was last frame") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Scene f(std::move(device));
    auto owned = std::make_unique<MotionProbe>();
    MotionProbe& probe = *owned;
    f.renderer->add_feature(std::move(owned));
    f.frame();
    f.front.set(Transform{{0.2, -0.5, -3.0}, math::quat(1.0f, 0.0f, 0.0f, 0.0f), math::vec3(1.0f)});
    f.frame();
    const math::mat4& view_projection = f.renderer->render_world().views.front().view_projection;
    // The front box's center face moved 0.2 m right: its pixels came from 0.2 m to the left.
    const math::vec3 face(0.2f, -0.5f, -2.5f);
    const math::vec2 now = screen_uv(view_projection, face);
    const math::vec2 before = screen_uv(view_projection, face - math::vec3(0.2f, 0.0f, 0.0f));
    const auto pixel = [&](math::vec2 uv) {
        return std::pair{static_cast<std::uint32_t>(uv.x * static_cast<float>(probe.width())),
                         static_cast<std::uint32_t>(uv.y * static_cast<float>(probe.height()))};
    };
    const auto [x, y] = pixel(now);
    const math::vec2 moved = probe.at(x, y);
    CHECK(moved.x == doctest::Approx(before.x - now.x).epsilon(0.02));
    CHECK(std::abs(moved.y) < 1e-3f);
    // The wall and the sky did not move.
    const auto [wx, wy] = pixel(screen_uv(view_projection, math::vec3(-3.0f, 2.0f, -4.9f)));
    CHECK(math::length(probe.at(wx, wy)) < 1e-4f);
    CHECK(math::length(probe.at(2, 2)) < 1e-4f);  // a corner: the wall fills it, or the sky
}
