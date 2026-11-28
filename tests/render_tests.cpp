#include "support/test_support.hpp"

#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>

#include <doctest/doctest.h>

#include <algorithm>

using namespace veach;
using rhi::ResourceState;

namespace {

/// Records every call as text so tests can assert what the graph executed.
class RecordingCommandList final : public rhi::CommandList {
public:
    void begin_render_pass(const rhi::RenderPassDesc& d) override {
        log("begin_render_pass " + std::string(d.debug_name));
    }
    void end_render_pass() override { log("end_render_pass"); }
    void begin_compute_pass(std::string_view name) override { log("begin_compute_pass " + std::string(name)); }
    void end_compute_pass() override { log("end_compute_pass"); }
    void set_pipeline(rhi::PipelineHandle) override { log("set_pipeline"); }
    void set_parameters(const void*, std::size_t) override { log("set_parameters"); }
    void draw(std::uint32_t, std::uint32_t, std::uint32_t) override { log("draw"); }
    void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override { log("dispatch"); }
    void dispatch_indirect(rhi::BufferHandle, std::uint64_t) override { log("dispatch_indirect"); }
    void draw_indirect(rhi::BufferHandle, std::uint64_t, std::uint32_t, std::uint32_t) override {
        log("draw_indirect");
    }
    void draw_indexed_indirect(rhi::BufferHandle, rhi::BufferHandle, std::uint64_t, std::uint32_t,
                               std::uint32_t) override {
        log("draw_indexed_indirect");
    }
    void build_blas(rhi::AccelHandle) override { log("build_blas"); }
    void refit_blas(rhi::AccelHandle) override { log("refit_blas"); }
    void build_tlas(rhi::AccelHandle, std::span<const rhi::TlasInstance>) override { log("build_tlas"); }
    void refit_tlas(rhi::AccelHandle, std::span<const rhi::TlasInstance>) override { log("refit_tlas"); }
    void copy_texture_to_buffer(rhi::TextureHandle, rhi::BufferHandle) override { log("copy"); }
    void copy_buffer(rhi::BufferHandle, rhi::BufferHandle, std::uint64_t) override { log("copy"); }
    void use(rhi::ResourceRef, ResourceState s) override { log(std::format("use {}", rhi::to_string(s))); }
    void barrier(std::span<const rhi::Barrier> barriers) override {
        for (const rhi::Barrier& b : barriers) {
            log(std::format("barrier {}->{}", rhi::to_string(b.before), rhi::to_string(b.after)));
        }
    }
    void push_debug_group(std::string_view name) override { log("pass " + std::string(name)); }
    void pop_debug_group() override {}
    void begin_timing(std::string_view name) override { log("begin_timing " + std::string(name)); }
    void end_timing() override { log("end_timing"); }

    std::vector<std::string> calls;

private:
    void log(std::string call) { calls.push_back(std::move(call)); }
};

struct GraphFixture {
    GraphFixture() : device(std::move(*rhi::create_device({}))), pool(*device) {}
    std::unique_ptr<rhi::Device> device;
    render::TransientPool pool;

    rhi::TextureHandle texture(std::string name) {
        return *device->create_texture({64, 64, rhi::Format::rgba8_unorm,
                                        rhi::TextureUsage::color_target | rhi::TextureUsage::storage, std::move(name)});
    }
};

render::RenderGraph::Execute noop() {
    return [](render::PassContext&) {
    };
}

}  // namespace

TEST_CASE("[render.AC1] the graph derives order from dependencies and culls unused passes") {
    GraphFixture f;
    render::RenderGraph graph(*f.device, &f.pool);
    const auto out = graph.import(f.texture("out"), "out", ResourceState::undefined);
    const auto buffer =
        graph.import(*f.device->create_buffer({.size = 256, .debug_name = "data"}), "data", ResourceState::undefined);
    const auto gbuffer = graph.create_texture(
        {64, 64, rhi::Format::rgba8_unorm, rhi::TextureUsage::color_target | rhi::TextureUsage::sampled, "gbuffer"});
    const auto unused = graph.create_texture({64, 64, rhi::Format::rgba8_unorm, rhi::TextureUsage::storage, "unused"});

    graph.add_graphics_pass(
        "geometry", [&](render::PassBuilder& b) { b.color_target(gbuffer, rhi::LoadOp::clear); }, noop());
    graph.add_compute_pass("simulate", [&](render::PassBuilder& b) { b.write(buffer); }, noop());
    graph.add_graphics_pass(
        "lighting",
        [&](render::PassBuilder& b) {
            b.read(gbuffer);
            b.color_target(out);
        },
        noop());
    graph.add_compute_pass("debug view", [&](render::PassBuilder& b) { b.write(unused); }, noop());
    REQUIRE(graph.compile().has_value());

    // "lighting" depends only on "geometry"; it is pulled ahead of the independent
    // compute pass to keep graphics work together. "debug view" writes nothing anyone reads.
    CHECK(graph.execution_order() == std::vector<std::string>{"geometry", "lighting", "simulate"});
    CHECK(graph.describe().find("culled: debug view") != std::string::npos);
}

TEST_CASE("[render.AC2] barriers follow declared accesses, including write-after-write and final states") {
    GraphFixture f;
    render::RenderGraph graph(*f.device, &f.pool);
    const auto image = graph.import(f.texture("image"), "image", ResourceState::undefined, ResourceState::present);
    graph.add_compute_pass("write 1", [&](render::PassBuilder& b) { b.write(image); }, noop());
    graph.add_compute_pass("write 2", [&](render::PassBuilder& b) { b.write(image); }, noop());
    graph.add_graphics_pass("draw", [&](render::PassBuilder& b) { b.color_target(image); }, noop());
    REQUIRE(graph.compile().has_value());

    RecordingCommandList cmd;
    graph.execute(cmd);
    const std::vector<std::string> barriers = [&] {
        std::vector<std::string> out;
        std::ranges::copy_if(cmd.calls, std::back_inserter(out),
                             [](const std::string& c) { return c.starts_with("barrier"); });
        return out;
    }();
    CHECK(barriers == std::vector<std::string>{
                          "barrier undefined->shader_write",
                          "barrier shader_write->shader_write",  // UAV barrier between the two writes
                          "barrier shader_write->color_target",
                          "barrier color_target->present",  // final state of the imported image
                      });
    CHECK(graph.final_state(image) == ResourceState::present);
}

TEST_CASE("[render.AC3] passes open the right encoder and declare residency") {
    GraphFixture f;
    render::RenderGraph graph(*f.device, &f.pool);
    const auto tlas = graph.import(rhi::AccelHandle{1, 1}, "tlas", ResourceState::accel_read);
    const auto color = graph.import(f.texture("color"), "color", ResourceState::shader_write);
    graph.add_compute_pass(
        "trace",
        [&](render::PassBuilder& b) {
            b.read(tlas, ResourceState::accel_read);
            b.write(color);
        },
        [](render::PassContext& ctx) { ctx.cmd.dispatch(1, 1); });
    REQUIRE(graph.compile().has_value());
    RecordingCommandList cmd;
    graph.execute(cmd);
    CHECK(cmd.calls == std::vector<std::string>{"pass trace", "barrier shader_write->shader_write",
                                                "begin_timing trace", "begin_compute_pass trace", "use accel_read",
                                                "use shader_write", "dispatch", "end_compute_pass", "end_timing"});
}

TEST_CASE("[render.AC4] impossible accesses are rejected with render.graph_invalid") {
    GraphFixture f;
    {
        render::RenderGraph graph(*f.device, &f.pool);
        const auto t =
            graph.create_texture({8, 8, rhi::Format::rgba8_unorm, rhi::TextureUsage::sampled, "never written"});
        const auto out = graph.import(f.texture("out"), "out", ResourceState::undefined);
        graph.add_graphics_pass(
            "reader",
            [&](render::PassBuilder& b) {
                b.read(t);
                b.color_target(out);
            },
            noop());
        auto result = graph.compile();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::render_graph_invalid);
        CHECK(result.error().message.find("never written") != std::string::npos);
    }
    {
        render::RenderGraph graph(*f.device, &f.pool);
        const auto out = graph.import(f.texture("out"), "out", ResourceState::undefined);
        graph.add_compute_pass("bad", [&](render::PassBuilder& b) { b.color_target(out); }, noop());
        CHECK(graph.compile().error().code == ErrorCode::render_graph_invalid);
    }
}

TEST_CASE("[render.AC5] the null RHI runs the whole renderer without a GPU") {
    test::TempProject project("render");
    project.write("main.luau", "local veach = require(\"@veach\")\n");
    test::LogCapture logs;
    Engine engine(test::headless_config(project.path(), "main.luau", 2));
    CHECK(engine.run() == 0);
    CHECK_FALSE(logs.has_code("render.graph_invalid"));
}

TEST_CASE("[rhi.AC3] indirect arguments and indices are graph accesses with their own states") {
    GraphFixture f;
    const auto buffer = [&](std::string name, rhi::BufferUsage usage) {
        return *f.device->create_buffer({.size = 64, .usage = usage, .debug_name = std::move(name), .stride = 4});
    };
    {
        render::RenderGraph graph(*f.device, &f.pool);
        const auto args = graph.import(buffer("args", rhi::BufferUsage::storage | rhi::BufferUsage::indirect), "args",
                                       ResourceState::undefined);
        const auto indices =
            graph.import(buffer("indices", rhi::BufferUsage::index), "indices", ResourceState::undefined);
        const auto out = graph.import(f.texture("out"), "out", ResourceState::undefined);
        graph.add_compute_pass("cull", [&](render::PassBuilder& b) { b.write(args); }, noop());
        graph.add_graphics_pass(
            "draw",
            [&](render::PassBuilder& b) {
                b.read(args, ResourceState::indirect_argument);
                b.read(indices, ResourceState::index_read);
                b.color_target(out);
            },
            noop());
        REQUIRE(graph.compile().has_value());
        RecordingCommandList cmd;
        graph.execute(cmd);
        CHECK(std::ranges::find(cmd.calls, "barrier shader_write->indirect_argument") != cmd.calls.end());
        CHECK(std::ranges::find(cmd.calls, "barrier undefined->index_read") != cmd.calls.end());
        CHECK(graph.final_state(args) == ResourceState::indirect_argument);
    }
    const auto rejected = [&](std::string_view expected, const auto& add) {
        render::RenderGraph graph(*f.device, &f.pool);
        add(graph);
        const auto result = graph.compile();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::render_graph_invalid);
        CHECK_MESSAGE(result.error().message.find(expected) != std::string::npos, result.error().message);
    };
    rejected("is not a buffer", [&](render::RenderGraph& graph) {
        const auto t = graph.import(f.texture("t"), "t", ResourceState::undefined);
        graph.add_compute_pass(
            "p", [&](render::PassBuilder& b) { b.read(t, ResourceState::indirect_argument); }, noop());
    });
    rejected("cannot draw or dispatch", [&](render::RenderGraph& graph) {
        const auto args = graph.import(buffer("args", rhi::BufferUsage::indirect), "args", ResourceState::undefined);
        graph.add_transfer_pass(
            "p", [&](render::PassBuilder& b) { b.read(args, ResourceState::indirect_argument); }, noop());
    });
    rejected("cannot draw", [&](render::RenderGraph& graph) {
        const auto indices = graph.import(buffer("indices", rhi::BufferUsage::index), "i", ResourceState::undefined);
        graph.add_compute_pass(
            "p", [&](render::PassBuilder& b) { b.read(indices, ResourceState::index_read); }, noop());
    });
}

TEST_CASE("[frames.AC5] a pooled transient's first barrier in the next frame syncs with its last use") {
    GraphFixture f;
    const rhi::TextureDesc desc{64, 64, rhi::Format::rgba8_unorm,
                                rhi::TextureUsage::color_target | rhi::TextureUsage::sampled, "gbuffer"};
    std::vector<std::string> first_barriers;
    for (int frame = 0; frame < 2; ++frame) {
        render::RenderGraph graph(*f.device, &f.pool);
        const auto out = graph.import(f.texture("out"), "out", ResourceState::undefined);
        const auto gbuffer = graph.create_texture(desc);
        graph.add_graphics_pass("geometry", [&](render::PassBuilder& b) { b.color_target(gbuffer); }, noop());
        graph.add_graphics_pass(
            "lighting",
            [&](render::PassBuilder& b) {
                b.read(gbuffer);
                b.color_target(out);
            },
            noop());
        REQUIRE(graph.compile().has_value());
        RecordingCommandList cmd;
        graph.execute(cmd);
        const auto it = std::ranges::find_if(cmd.calls, [](const std::string& c) { return c.starts_with("barrier"); });
        first_barriers.push_back(*it);
        CHECK(graph.describe().find("(discard)") != std::string::npos);
    }
    CHECK(first_barriers[0] == "barrier undefined->color_target");
    CHECK(first_barriers[1] == "barrier shader_read->color_target");  // last frame left it in shader_read
}

namespace {

/// A world with the camera components, and a renderer on the null device to extract into.
struct ViewFixture {
    std::unique_ptr<rhi::Device> device = std::move(*rhi::create_device({}));
    render::Renderer renderer{*device};
    World world;

    ViewFixture() {
        world.import<CoreModule>();
        world.component<Camera>();
    }
    const render::RenderWorld& extract() {
        propagate_transforms(world);  // what the engine's TransformPropagation systems do every frame
        renderer.extract(world);
        return renderer.render_world();
    }
};

/// Records whether features saw a primary view.
class ViewProbe final : public render::RenderFeature {
public:
    explicit ViewProbe(bool& saw_view) : saw_view_(saw_view) {}
    [[nodiscard]] std::string_view name() const override { return "test.view_probe"; }
    [[nodiscard]] Result<> prepare(rhi::Device& /*device*/) override { return {}; }
    void record(render::RenderGraph& /*graph*/, const render::FrameContext& frame) override {
        saw_view_ = frame.view != nullptr;
    }

private:
    bool& saw_view_;
};

math::vec3 to_ndc(const math::mat4& view_projection, math::vec3 world_point) {
    const math::vec4 clip = view_projection * math::vec4(world_point, 1.0f);
    return math::vec3(clip) / clip.w;
}

}  // namespace

TEST_CASE("[views.AC1] an active camera with a Transform becomes a reverse-Z view") {
    ViewFixture f;
    const math::quat yaw = math::angleAxis(math::radians(30.0f), math::world_up);
    f.world.entity("camera")
        .set(Transform{{1.0f, 2.0f, 3.0f}, yaw, {5.0f, 5.0f, 5.0f}})  // scale must not affect the view
        .set(Camera{math::radians(60.0f), 0.5f, true});
    const render::RenderWorld& rw = f.extract();
    REQUIRE(rw.views.size() == 1);
    const render::RenderView& v = rw.views.front();
    CHECK(v.width == 1280);
    CHECK(v.height == 720);
    CHECK(math::distance(v.position, math::vec3(1.0f, 2.0f, 3.0f)) < 1e-5f);

    const math::vec3 forward = yaw * math::world_forward;
    const math::vec3 right = yaw * math::world_right;
    const math::vec3 at_near = to_ndc(v.view_projection, v.position + forward * 0.5f);
    CHECK(at_near.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(at_near.y == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(at_near.z == doctest::Approx(1.0f).epsilon(1e-4));  // reverse-Z: the near plane is depth 1
    const math::vec3 farther = to_ndc(v.view_projection, v.position + forward * 10.0f);
    CHECK(farther.z > 0.0f);
    CHECK(farther.z < 1.0f);
    CHECK(to_ndc(v.view_projection, v.position + forward * 10.0f + right).x > 0.0f);  // +X is right

    const math::mat4 identity = v.inverse_view_projection * v.view_projection;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            CHECK(identity[c][r] == doctest::Approx(c == r ? 1.0f : 0.0f).epsilon(1e-4));
        }
    }
}

TEST_CASE("[views.AC2] inactive cameras are skipped and views come in entity-id order") {
    ViewFixture f;
    bool saw_view = true;
    f.renderer.add_feature(std::make_unique<ViewProbe>(saw_view));
    CHECK(f.extract().views.empty());
    f.renderer.render();
    CHECK_FALSE(saw_view);  // no camera: FrameContext::view is null

    const Entity narrow = f.world.entity("narrow").set(Camera{math::radians(40.0f), 0.1f, true});
    f.world.entity("off").set(Camera{math::radians(50.0f), 0.1f, false});
    const Entity wide = f.world.entity("wide").set(Camera{math::radians(70.0f), 0.1f, true});
    const render::RenderWorld& rw = f.extract();
    REQUIRE(rw.views.size() == 2);
    // Ordered by entity id, which is not creation order once Flecs recycles ids (with a newer generation).
    CHECK(rw.views[0].camera < rw.views[1].camera);
    const bool narrow_first = narrow.id() < wide.id();
    CHECK(rw.views[0].camera == (narrow_first ? narrow.id() : wide.id()));
    // A narrower field of view has a larger y scale.
    CHECK((rw.views[0].projection[1][1] > rw.views[1].projection[1][1]) == narrow_first);
    f.renderer.render();
    CHECK(saw_view);
}
