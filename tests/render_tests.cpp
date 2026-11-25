#include "support/test_support.hpp"

#include <veach/render/render_graph.hpp>

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
    void build_blas(rhi::AccelHandle) override { log("build_blas"); }
    void refit_blas(rhi::AccelHandle) override { log("refit_blas"); }
    void build_tlas(rhi::AccelHandle, std::span<const rhi::TlasInstance>) override { log("build_tlas"); }
    void refit_tlas(rhi::AccelHandle, std::span<const rhi::TlasInstance>) override { log("refit_tlas"); }
    void copy_texture_to_buffer(rhi::TextureHandle, rhi::BufferHandle) override { log("copy"); }
    void use(rhi::ResourceRef, ResourceState s) override { log(std::format("use {}", rhi::to_string(s))); }
    void barrier(std::span<const rhi::Barrier> barriers) override {
        for (const rhi::Barrier& b : barriers) {
            log(std::format("barrier {}->{}", rhi::to_string(b.before), rhi::to_string(b.after)));
        }
    }
    void push_debug_group(std::string_view name) override { log("pass " + std::string(name)); }
    void pop_debug_group() override {}

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
                                                "begin_compute_pass trace", "use accel_read", "use shader_write",
                                                "dispatch", "end_compute_pass"});
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
