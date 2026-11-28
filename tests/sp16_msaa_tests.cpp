// Spike SP-16 (VCH-092): multisampled attachments in tile memory, resolved at the end of the pass, and alpha to
// coverage (Metal only; docs/research/spikes/sp-16-visibility-vs-forward.md). Validation runs on every backend.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"

#include <veach/render/render_graph.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace veach;

namespace {

/// Mirrors `Params` in tests/shaders/msaa_triangle.slang.
struct MsaaParams {
    float coverage = 1.0f;
    std::uint32_t unused0 = 0;
    std::uint32_t unused1 = 0;
    std::uint32_t unused2 = 0;
};
static_assert(rhi::parameters_match<MsaaParams>(test_shaders::layout::msaa_triangle,
                                                {VEACH_PARAM(MsaaParams, coverage), VEACH_PARAM(MsaaParams, unused0),
                                                 VEACH_PARAM(MsaaParams, unused1), VEACH_PARAM(MsaaParams, unused2)}));

constexpr std::uint32_t size = 32;

/// Draws the triangle into a 4x MSAA target resolved into an rgba8 one; returns the resolved red channel.
std::vector<std::uint8_t> draw_resolved(rhi::Device& device, float coverage, bool alpha_to_coverage) {
    rhi::GraphicsPipelineDesc desc;
    desc.library = &test_shaders::msaa_triangle();
    desc.vertex_entry = "vs_main";
    desc.fragment_entry = "fs_main";
    desc.color_formats = {rhi::Format::rgba8_unorm};
    desc.sample_count = 4;
    desc.alpha_to_coverage = alpha_to_coverage;
    desc.debug_name = "msaa triangle";
    auto pipeline = device.create_graphics_pipeline(desc);
    REQUIRE_MESSAGE(pipeline.has_value(), (pipeline ? std::string() : pipeline.error().message));
    auto resolved = device.create_texture({size, size, rhi::Format::rgba8_unorm,
                                           rhi::TextureUsage::color_target | rhi::TextureUsage::copy_src, "resolved"});
    REQUIRE(resolved.has_value());
    const std::uint64_t bytes = rhi::row_bytes(rhi::Format::rgba8_unorm, size) * size;
    auto readback = device.create_buffer({.size = bytes,
                                          .usage = rhi::BufferUsage::copy_dst,
                                          .memory = rhi::MemoryType::readback,
                                          .debug_name = "resolved readback"});
    REQUIRE(readback.has_value());
    render::TransientPool pool(device);

    static_cast<void>(device.begin_frame());
    render::RenderGraph graph(device, &pool);
    rhi::TextureDesc ms{size, size, rhi::Format::rgba8_unorm, rhi::TextureUsage::color_target, "samples"};
    ms.sample_count = 4;
    const auto samples = graph.create_texture(ms);
    const auto target = graph.import(*resolved, "resolved", rhi::ResourceState::undefined);
    const auto out = graph.import(*readback, "readback", rhi::ResourceState::undefined);
    graph.add_graphics_pass(
        "draw",
        [&](render::PassBuilder& b) {
            b.color_target(samples, rhi::LoadOp::clear, {0.0f, 0.0f, 0.0f, 0.0f});
            b.resolve(samples, target);
        },
        [&](render::PassContext& ctx) {
            ctx.cmd.set_pipeline(*pipeline);
            ctx.cmd.set_parameters(MsaaParams{coverage});
            ctx.cmd.draw(3);
        });
    graph.add_transfer_pass(
        "read back",
        [&](render::PassBuilder& b) {
            b.read(target, rhi::ResourceState::copy_src);
            b.write(out, rhi::ResourceState::copy_dst);
        },
        [&](render::PassContext& ctx) { ctx.cmd.copy_texture_to_buffer(ctx.texture(target), ctx.buffer(out)); });
    const auto compiled = graph.compile();
    REQUIRE_MESSAGE(compiled.has_value(), (compiled ? std::string() : compiled.error().message));
    auto cmd = device.begin_commands("msaa");
    graph.execute(*cmd);
    static_cast<void>(device.submit(std::move(cmd)));
    device.end_frame();
    device.wait_idle();

    const auto* texels = static_cast<const std::uint8_t*>(device.map(*readback));
    REQUIRE(texels != nullptr);
    std::vector<std::uint8_t> red(std::size_t{size} * size);
    for (std::size_t i = 0; i < red.size(); ++i) {
        red[i] = texels[i * 4];
    }
    device.destroy(*pipeline);
    device.destroy(*resolved);
    device.destroy(*readback);
    return red;
}

}  // namespace

TEST_CASE("[sp16.msaa] multisampled textures and pipelines are validated on every backend") {
    auto device = rhi::create_device({.backend = rhi::Backend::null});
    REQUIRE(device.has_value());
    rhi::TextureDesc three{8, 8, rhi::Format::rgba8_unorm, rhi::TextureUsage::color_target, "three samples"};
    three.sample_count = 3;
    CHECK_FALSE((*device)->create_texture(three).has_value());
    rhi::TextureDesc sampled{8, 8, rhi::Format::rgba8_unorm,
                             rhi::TextureUsage::color_target | rhi::TextureUsage::sampled, "sampled samples"};
    sampled.sample_count = 4;
    const auto rejected = (*device)->create_texture(sampled);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == ErrorCode::rhi_invalid_texture);
    rhi::GraphicsPipelineDesc a2c;
    a2c.library = &test_shaders::msaa_triangle();
    a2c.vertex_entry = "vs_main";
    a2c.fragment_entry = "fs_main";
    a2c.color_formats = {rhi::Format::rgba8_unorm};
    a2c.alpha_to_coverage = true;  // with one sample
    const auto no_samples = (*device)->create_graphics_pipeline(a2c);
    REQUIRE_FALSE(no_samples.has_value());
    CHECK(no_samples.error().code == ErrorCode::rhi_invalid_pipeline);
}

TEST_CASE("[sp16.msaa] 4x MSAA resolves on tile, and alpha to coverage drops samples") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = test::gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        if (device->backend() != rhi::Backend::metal) {
            MESSAGE("skipped: multisampling is Metal-only (spike SP-16)");
            return;
        }
        const std::vector<std::uint8_t> edge = draw_resolved(*device, 1.0f, false);
        std::size_t inside = 0;
        std::size_t outside = 0;
        std::size_t between = 0;
        for (const std::uint8_t v : edge) {
            inside += v == 255 ? 1 : 0;
            outside += v == 0 ? 1 : 0;
            between += v > 0 && v < 255 ? 1 : 0;
        }
        // The diagonal crosses 32 pixels: they resolve to a fraction of their 4 samples; the halves stay pure.
        CHECK(between >= size / 2);
        CHECK(between <= 2 * size);
        CHECK(inside > 400);
        CHECK(outside > 400);

        const std::vector<std::uint8_t> half = draw_resolved(*device, 0.5f, true);
        const std::uint8_t deep_inside = half[std::size_t{size - 4} * size + 4];  // lower left
        CHECK(deep_inside > 64);
        CHECK(deep_inside < 192);                            // about half its samples kept
        CHECK(half[std::size_t{4} * size + size - 4] == 0);  // upper right: never covered
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
