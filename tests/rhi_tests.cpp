#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"

#include <veach/render/render_graph.hpp>
#include <veach/rhi/submission_tracker.hpp>

#include <doctest/doctest.h>

#include <array>
#include <span>
#include <vector>

using namespace veach;
using rhi::SubmissionId;

TEST_CASE("[frames.AC1] frame N waits only for frame N - frames_in_flight") {
    rhi::SubmissionTracker tracker(2);
    SubmissionId wait;

    CHECK(tracker.begin_frame(wait).slot == 1);  // frame 1
    CHECK(wait.value == 0);
    const SubmissionId f1 = tracker.next_submission();
    tracker.end_frame();

    CHECK(tracker.begin_frame(wait).slot == 0);  // frame 2: slot 0 never used, no wait
    CHECK(wait.value == 0);
    const SubmissionId f2 = tracker.next_submission();
    tracker.end_frame();

    static_cast<void>(tracker.begin_frame(wait));  // frame 3 reuses frame 1's slot: waits for frame 1, not 2
    CHECK(wait.value == f1.value);
    tracker.mark_completed(f1);
    static_cast<void>(tracker.next_submission());
    tracker.end_frame();

    static_cast<void>(tracker.begin_frame(wait));  // frame 4 reuses frame 2's slot
    CHECK(wait.value == f2.value);

    rhi::SubmissionTracker single(1);  // one frame in flight: every frame waits for the previous one
    static_cast<void>(single.begin_frame(wait));
    const SubmissionId s1 = single.next_submission();
    single.end_frame();
    static_cast<void>(single.begin_frame(wait));
    CHECK(wait.value == s1.value);
}

TEST_CASE("[frames.AC2] destroyed resources are released only after the GPU is done with them") {
    rhi::SubmissionTracker tracker(2);
    std::vector<int> released;

    tracker.retire([&] { released.push_back(0); });  // nothing in flight: released immediately
    CHECK(released == std::vector<int>{0});

    const SubmissionId a = tracker.next_submission();
    tracker.retire([&] { released.push_back(1); });  // may be in use by a
    const SubmissionId b = tracker.next_submission();
    tracker.retire([&] { released.push_back(2); });  // may be in use by a or b
    CHECK(tracker.pending_releases() == 2);

    tracker.collect();
    CHECK(released.size() == 1);

    tracker.mark_completed(a);
    tracker.collect();
    CHECK(released == std::vector<int>{0, 1});

    tracker.mark_completed(SubmissionId{1});  // stale reports never move the watermark back
    CHECK(tracker.completed().value == a.value);
    tracker.mark_completed(b);
    tracker.collect();
    CHECK(released == std::vector<int>{0, 1, 2});
}

TEST_CASE("[frames.AC3] the null device runs frames and kills destroyed handles at once") {
    auto device = std::move(*rhi::create_device({.frames_in_flight = 3}));
    CHECK(device->frames_in_flight() == 3);
    for (int i = 0; i < 5; ++i) {
        const rhi::FrameInfo frame = device->begin_frame();
        CHECK(frame.index == static_cast<std::uint64_t>(i + 1));
        CHECK(frame.slot == frame.index % 3);
        auto buffer = device->create_buffer({.size = 64, .memory = rhi::MemoryType::upload, .debug_name = "per-frame"});
        REQUIRE(buffer.has_value());
        CHECK(device->map(*buffer) != nullptr);
        device->submit(device->begin_commands("frame"), {});
        device->destroy(*buffer);
        CHECK(device->map(*buffer) == nullptr);  // the handle is dead immediately
        device->end_frame();
    }
}

TEST_CASE("[frames.AC4] frames_in_flight is validated and reaches the device") {
    const std::vector<std::string_view> args{"--headless"};
    auto config = resolve_config({.name = "t"}, args);
    REQUIRE(config.has_value());
    CHECK(config->project.frames_in_flight == 2);

    test::TempProject project("frames");
    project.write("veach.project.json", R"({"frames_in_flight": 9})");
    const std::string dir = project.path().string();
    const std::vector<std::string_view> bad{"--project", dir};
    auto rejected = resolve_config({.name = "t"}, bad);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message.find("between 1 and 4") != std::string::npos);
}

// ---- Raster state on the platform's GPU
// (docs/specs/realtime-renderer-v0/spec.md) --------------------------------

namespace {

/// Mirrors `Params` in tests/shaders/raster_states.slang.
struct RasterStateParams {
    float depth = 0.5f;
    std::uint32_t id = 0;
    std::uint32_t clockwise = 0;
    std::uint32_t unused = 0;
};
static_assert(rhi::parameters_match<RasterStateParams>(test_shaders::layout::raster_states,
                                                       {VEACH_PARAM(RasterStateParams, depth),
                                                        VEACH_PARAM(RasterStateParams, id),
                                                        VEACH_PARAM(RasterStateParams, clockwise),
                                                        VEACH_PARAM(RasterStateParams, unused)}));

constexpr std::uint32_t raster_size = 64;
constexpr std::uint32_t cleared = 0xFFFFFFFFu;

/// Where a pixel lies against the triangle the shader draws (the lower-left
/// half of the target): -1 outside, 1 inside, 0 on the diagonal (skipped).
int side(std::uint32_t x, std::uint32_t y) {
    const float half = raster_size / 2.0f;
    const float sum = ((static_cast<float>(x) + 0.5f) / half - 1.0f) + (1.0f - (static_cast<float>(y) + 0.5f) / half);
    return sum < -0.05f ? 1 : sum > 0.05f ? -1 : 0;
}

struct RasterResult {
    std::vector<std::uint32_t> pixels;  ///< two per pixel, rows top to bottom
    rhi::FrameTimings timings;
};

/// Draws `draws` into a 64x64 rg32_uint target cleared to 0xFFFFFFFF (and a
/// depth target cleared to 0, the far plane, when `depth`) through the render
/// graph, and reads the target back.
RasterResult rasterize(rhi::Device& device, rhi::GraphicsPipelineDesc desc, std::span<const RasterStateParams> draws,
                       bool depth) {
    desc.library = &test_shaders::raster_states();
    desc.vertex_entry = "vs_main";
    desc.fragment_entry = "fs_main";
    desc.color_formats = {rhi::Format::rg32_uint};
    desc.depth_format = depth ? rhi::Format::depth32_float : rhi::Format::undefined;
    auto pipeline = device.create_graphics_pipeline(desc);
    REQUIRE_MESSAGE(pipeline.has_value(), (pipeline ? std::string() : pipeline.error().message));
    auto target = device.create_texture({raster_size, raster_size, rhi::Format::rg32_uint,
                                         rhi::TextureUsage::color_target | rhi::TextureUsage::copy_src, "ids"});
    REQUIRE(target.has_value());
    const std::uint64_t bytes = rhi::row_bytes(rhi::Format::rg32_uint, raster_size) * raster_size;
    auto readback = device.create_buffer(
        {.size = bytes, .usage = rhi::BufferUsage::copy_dst, .memory = rhi::MemoryType::readback, .debug_name = "ids"});
    REQUIRE(readback.has_value());
    render::TransientPool pool(device);

    static_cast<void>(device.begin_frame());
    render::RenderGraph graph(device, &pool);
    const auto ids = graph.import(*target, "ids", rhi::ResourceState::undefined);
    const auto out = graph.import(*readback, "readback", rhi::ResourceState::undefined);
    const auto depth_buffer = depth ? graph.create_texture({raster_size, raster_size, rhi::Format::depth32_float,
                                                            rhi::TextureUsage::depth_target, "depth"})
                                    : render::GraphResource{};
    graph.add_graphics_pass(
        "draw",
        [&](render::PassBuilder& b) {
            b.color_target_uint(ids, rhi::LoadOp::clear, {cleared, cleared, cleared, cleared});
            if (depth) {
                b.depth_target(depth_buffer, rhi::LoadOp::clear, 0.0f);
            }
        },
        [&](render::PassContext& ctx) {
            ctx.cmd.set_pipeline(*pipeline);
            for (const RasterStateParams& draw : draws) {
                ctx.cmd.set_parameters(draw);
                ctx.cmd.draw(3);
            }
        });
    graph.add_transfer_pass(
        "read back",
        [&](render::PassBuilder& b) {
            b.read(ids, rhi::ResourceState::copy_src);
            b.write(out, rhi::ResourceState::copy_dst);
        },
        [&](render::PassContext& ctx) { ctx.cmd.copy_texture_to_buffer(ctx.texture(ids), ctx.buffer(out)); });
    REQUIRE(graph.compile().has_value());
    auto cmd = device.begin_commands("raster states");
    graph.execute(*cmd);
    static_cast<void>(device.submit(std::move(cmd)));
    device.end_frame();
    device.wait_idle();

    RasterResult result;
    const auto* words = static_cast<const std::uint32_t*>(device.map(*readback));
    REQUIRE(words != nullptr);
    result.pixels.assign(words, words + bytes / 4);
    result.timings = device.last_frame_timings();
    device.destroy(*pipeline);
    device.destroy(*target);
    device.destroy(*readback);
    return result;
}

/// Pixels on `which` side of the diagonal (1 inside, -1 outside) that are not
/// `expected`.
std::size_t count_wrong(const RasterResult& r, int which, std::array<std::uint32_t, 2> expected) {
    std::size_t wrong = 0;
    for (std::uint32_t y = 0; y < raster_size; ++y) {
        for (std::uint32_t x = 0; x < raster_size; ++x) {
            const std::size_t i = (std::size_t{y} * raster_size + x) * 2;
            if (side(x, y) == which && (r.pixels[i] != expected[0] || r.pixels[i + 1] != expected[1])) {
                ++wrong;
            }
        }
    }
    return wrong;
}

}  // namespace

TEST_CASE(
    "[rt.AC10] reverse-Z depth test: the nearer triangle wins in any "
    "draw order, and back faces are culled") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = test::gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        rhi::GraphicsPipelineDesc desc;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.depth_compare = rhi::CompareOp::greater;
        desc.cull = rhi::CullMode::back;
        desc.debug_name = "depth test";
        const std::array<RasterStateParams, 4> draws{{
            {.depth = 0.25f, .id = 2},                 // far
            {.depth = 0.75f, .id = 1},                 // near: replaces it
            {.depth = 0.25f, .id = 3},                 // far again: fails the test
            {.depth = 1.0f, .id = 4, .clockwise = 1},  // nearest, but a back face: culled
        }};
        const RasterResult r = rasterize(*device, desc, draws, true);
        CHECK_MESSAGE(count_wrong(r, 1, {1, ~1u}) == 0, "the near front-facing triangle must cover its half");
        CHECK_MESSAGE(count_wrong(r, -1, {cleared, cleared}) == 0, "the other half keeps the clear value");

        // Without depth state the last draw wins, so the test above is not passing
        // by accident.
        rhi::GraphicsPipelineDesc plain;
        plain.debug_name = "no depth";
        const std::array<RasterStateParams, 2> unordered{{{.depth = 0.75f, .id = 1}, {.depth = 0.25f, .id = 2}}};
        CHECK(count_wrong(rasterize(*device, plain, unordered, true), 1, {2, ~2u}) == 0);
        device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE(
    "[rt.AC11] an rg32_uint target clears to 0xFFFFFFFF and keeps what "
    "the fragment shader writes, exactly") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = test::gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        rhi::GraphicsPipelineDesc desc;
        desc.debug_name = "ids";
        const std::array<RasterStateParams, 1> draws{{{.id = 0x89ABCDEFu}}};  // ~id = 0x76543210: every bit used
        const RasterResult r = rasterize(*device, desc, draws, false);
        CHECK(count_wrong(r, 1, {0x89ABCDEFu, 0x76543210u}) == 0);
        CHECK(count_wrong(r, -1, {cleared, cleared}) == 0);

        // rt.AC7 at the RHI: the render graph timed both passes of that frame.
        CHECK(r.timings.unavailable.empty());
        REQUIRE(r.timings.passes.size() == 2);
        CHECK(r.timings.passes[0].name == "draw");
        CHECK(r.timings.passes[1].name == "read back");
        CHECK(r.timings.passes[0].gpu_ms > 0.0);
        CHECK(r.timings.passes[0].gpu_ms < 1000.0);
        device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rt.AC10] a pipeline that tests depth without a depth format is rejected on every backend") {
    auto device = std::move(*rhi::create_device({}));
    rhi::GraphicsPipelineDesc desc;
    desc.depth_test = true;
    desc.debug_name = "no depth format";
    auto rejected = device->create_graphics_pipeline(desc);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == ErrorCode::rhi_invalid_pipeline);
    desc.depth_format = rhi::Format::rg32_uint;
    CHECK_FALSE(device->create_graphics_pipeline(desc).has_value());
    desc.depth_format = rhi::Format::depth32_float;
    CHECK(device->create_graphics_pipeline(desc).has_value());
}
