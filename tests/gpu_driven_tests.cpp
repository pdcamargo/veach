// Indirect work and non-blocking readback (VCH-071, VCH-073; docs/specs/gpu-driven-rhi/spec.md). The GPU tests run
// on Metal in the pre-push gate and on DX12/WARP in hosted CI, so they stay small: 64 items, 64x64 targets.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"

#include <veach/render/readback.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

using namespace veach;
using rhi::ResourceState;

namespace {

/// Mirrors `Params` in tests/shaders/indirect_cull.slang.
struct IndirectCullParams {
    rhi::DescriptorHandle items;
    rhi::DescriptorHandle visible;
    rhi::DescriptorHandle survivors;
    rhi::DescriptorHandle args;
    rhi::DescriptorHandle counter;
    std::uint32_t item_count = 0;
    std::uint32_t threshold = 0;
};
static_assert(rhi::parameters_match<IndirectCullParams>(
    test_shaders::layout::indirect_cull,
    {VEACH_PARAM(IndirectCullParams, items), VEACH_PARAM(IndirectCullParams, visible),
     VEACH_PARAM(IndirectCullParams, survivors), VEACH_PARAM(IndirectCullParams, args),
     VEACH_PARAM(IndirectCullParams, counter), VEACH_PARAM(IndirectCullParams, item_count),
     VEACH_PARAM(IndirectCullParams, threshold)}));

/// Mirrors `Params` in tests/shaders/indirect_draw.slang.
struct IndirectDrawParams {
    rhi::DescriptorHandle survivors;
    std::uint32_t indexed = 0;
    std::uint32_t unused = 0;
};
static_assert(rhi::parameters_match<IndirectDrawParams>(test_shaders::layout::indirect_draw,
                                                        {VEACH_PARAM(IndirectDrawParams, survivors),
                                                         VEACH_PARAM(IndirectDrawParams, indexed),
                                                         VEACH_PARAM(IndirectDrawParams, unused)}));

constexpr std::uint32_t item_count = 64;  // an 8x8 grid of 8x8-pixel cells in a 64x64 target
constexpr std::uint32_t target_size = 64;
constexpr std::uint32_t threshold = 23;
// Byte offsets of the records write_args fills (indirect_cull.slang).
constexpr std::uint64_t dispatch_offset = 0;
constexpr std::uint64_t draw_offset = 16;  // two DrawIndirectArgs: an empty one, then the real one
constexpr std::uint64_t indexed_offset = 48;
constexpr std::uint64_t args_size = 80;

/// A permutation of 0..63, so exactly `threshold` items survive, scattered over the grid.
std::uint32_t item_value(std::uint32_t item) {
    return (item * 37 + 11) % item_count;
}

bool survives(std::uint32_t item) {
    return item_value(item) < threshold;
}

/// The pipelines and buffers of the culling scene, on any device.
struct CullScene {
    explicit CullScene(rhi::Device& d) : device(d) {
        const auto compute = [&](std::string_view entry, std::array<std::uint32_t, 3> group) {
            auto p = device.create_compute_pipeline({&test_shaders::indirect_cull(), entry, group, std::string(entry)});
            REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().message));
            return *p;
        };
        cull = compute("cull", {64, 1, 1});
        write_args = compute("write_args", {1, 1, 1});
        count_threads = compute("count_threads", {8, 1, 1});
        rhi::GraphicsPipelineDesc desc;
        desc.library = &test_shaders::indirect_draw();
        desc.vertex_entry = "vs_main";
        desc.fragment_entry = "fs_main";
        desc.color_formats = {rhi::Format::r32_uint};
        desc.debug_name = "indirect draw";
        auto p = device.create_graphics_pipeline(desc);
        REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().message));
        draw = *p;

        std::array<std::uint32_t, item_count> values{};
        for (std::uint32_t i = 0; i < item_count; ++i) {
            values[i] = item_value(i);
        }
        const std::array<std::uint32_t, item_count + 1> zeros{};
        const auto storage = [&](std::string name, std::uint64_t size, rhi::BufferUsage extra,
                                 std::span<const std::byte> data) {
            auto b = device.create_buffer({.size = size,
                                           .usage = rhi::BufferUsage::storage | extra,
                                           .debug_name = std::move(name),
                                           .stride = sizeof(std::uint32_t)},
                                          data);
            REQUIRE(b.has_value());
            return *b;
        };
        items = storage("items", sizeof(values), rhi::BufferUsage::none, std::as_bytes(std::span(values)));
        visible = storage("visible", sizeof(zeros), rhi::BufferUsage::copy_src, std::as_bytes(std::span(zeros)));
        args = storage("indirect args", args_size, rhi::BufferUsage::indirect,
                       std::as_bytes(std::span(zeros).first(args_size / 4)));
        counter = storage("counter", 16, rhi::BufferUsage::copy_src, std::as_bytes(std::span(zeros).first(4)));
        const std::array<std::uint32_t, 6> quad{0, 1, 2, 2, 1, 3};
        auto index_buffer =
            device.create_buffer({.size = sizeof(quad), .usage = rhi::BufferUsage::index, .debug_name = "quad indices"},
                                 std::as_bytes(std::span(quad)));
        REQUIRE(index_buffer.has_value());
        indices = *index_buffer;
    }
    ~CullScene() {
        device.wait_idle();
        for (const rhi::PipelineHandle p : {cull, write_args, count_threads, draw}) {
            device.destroy(p);
        }
        for (const rhi::BufferHandle b : {items, visible, args, counter, indices}) {
            device.destroy(b);
        }
    }
    CullScene(const CullScene&) = delete;
    CullScene& operator=(const CullScene&) = delete;
    CullScene(CullScene&&) = delete;
    CullScene& operator=(CullScene&&) = delete;

    [[nodiscard]] IndirectCullParams params() const {
        return {device.descriptor(items),
                device.descriptor(visible, rhi::View::storage),
                device.descriptor(visible),
                device.descriptor(args, rhi::View::storage),
                device.descriptor(counter, rhi::View::storage),
                item_count,
                threshold};
    }

    /// Adds the culling pass; returns the survivor list ([0] the count, then item indices).
    render::GraphResource add_cull(render::RenderGraph& graph) {
        const auto in = graph.import(items, "items", ResourceState::undefined);
        const auto out = graph.import(visible, "visible", ResourceState::undefined);
        graph.add_compute_pass(
            "cull",
            [&](render::PassBuilder& b) {
                b.read(in);
                b.write(out);
            },
            [this](render::PassContext& ctx) {
                ctx.cmd.set_pipeline(cull);
                ctx.cmd.set_parameters(params());
                ctx.cmd.dispatch(1, 1);
            });
        return out;
    }

    rhi::Device& device;
    rhi::PipelineHandle cull, write_args, count_threads, draw;
    rhi::BufferHandle items, visible, args, counter, indices;
};

struct IndirectResult {
    std::uint32_t survivors = 0;               ///< what cull counted
    std::uint32_t threads = 0;                 ///< what count_threads, dispatched indirectly, counted
    std::vector<std::uint32_t> drawn;          ///< draw_indirect's target, rows top to bottom
    std::vector<std::uint32_t> drawn_indexed;  ///< draw_indexed_indirect's target
};

/// One frame: cull, write the arguments, dispatch and draw with them, read everything back.
IndirectResult run_indirect(rhi::Device& device) {
    CullScene scene(device);
    const auto target = [&](std::string name) {
        auto t =
            device.create_texture({target_size, target_size, rhi::Format::r32_uint,
                                   rhi::TextureUsage::color_target | rhi::TextureUsage::copy_src, std::move(name)});
        REQUIRE(t.has_value());
        return *t;
    };
    const auto readback = [&](std::string name, std::uint64_t size) {
        auto b = device.create_buffer({.size = size,
                                       .usage = rhi::BufferUsage::copy_dst,
                                       .memory = rhi::MemoryType::readback,
                                       .debug_name = std::move(name)});
        REQUIRE(b.has_value());
        return *b;
    };
    const std::uint64_t image_bytes = rhi::row_bytes(rhi::Format::r32_uint, target_size) * target_size;
    const std::array<rhi::TextureHandle, 2> targets{target("drawn"), target("drawn indexed")};
    const std::array<rhi::BufferHandle, 4> readbacks{readback("visible readback", 4), readback("counter readback", 4),
                                                     readback("drawn readback", image_bytes),
                                                     readback("drawn indexed readback", image_bytes)};

    static_cast<void>(device.begin_frame());
    render::RenderGraph graph(device);
    const auto visible = scene.add_cull(graph);
    const auto args = graph.import(scene.args, "args", ResourceState::undefined);
    const auto counter = graph.import(scene.counter, "counter", ResourceState::undefined);
    const auto indices = graph.import(scene.indices, "indices", ResourceState::undefined);
    const auto drawn = graph.import(targets[0], "drawn", ResourceState::undefined);
    const auto drawn_indexed = graph.import(targets[1], "drawn indexed", ResourceState::undefined);
    std::array<render::GraphResource, 4> outs{};
    for (std::size_t i = 0; i < outs.size(); ++i) {
        outs.at(i) = graph.import(readbacks.at(i), "readback", ResourceState::undefined);
    }
    graph.add_compute_pass(
        "write args",
        [&](render::PassBuilder& b) {
            b.read(visible);
            b.write(args);
        },
        [&](render::PassContext& ctx) {
            ctx.cmd.set_pipeline(scene.write_args);
            ctx.cmd.set_parameters(scene.params());
            ctx.cmd.dispatch(1, 1);
        });
    graph.add_compute_pass(
        "count threads",
        [&](render::PassBuilder& b) {
            b.read(args, ResourceState::indirect_argument);
            b.write(counter);
        },
        [&](render::PassContext& ctx) {
            ctx.cmd.set_pipeline(scene.count_threads);
            ctx.cmd.set_parameters(scene.params());
            ctx.cmd.dispatch_indirect(ctx.buffer(args), dispatch_offset);
        });
    const IndirectDrawParams draw_params{device.descriptor(scene.visible), 0, 0};
    graph.add_graphics_pass(
        "draw",
        [&](render::PassBuilder& b) {
            b.read(args, ResourceState::indirect_argument);
            b.read(visible);
            b.color_target_uint(drawn, rhi::LoadOp::clear, {});
        },
        [&](render::PassContext& ctx) {
            ctx.cmd.set_pipeline(scene.draw);
            ctx.cmd.set_parameters(draw_params);
            ctx.cmd.draw_indirect(ctx.buffer(args), draw_offset, 2, sizeof(rhi::DrawIndirectArgs));
        });
    graph.add_graphics_pass(
        "draw indexed",
        [&](render::PassBuilder& b) {
            b.read(args, ResourceState::indirect_argument);
            b.read(visible);
            b.read(indices, ResourceState::index_read);
            b.color_target_uint(drawn_indexed, rhi::LoadOp::clear, {});
        },
        [&](render::PassContext& ctx) {
            ctx.cmd.set_pipeline(scene.draw);
            ctx.cmd.set_parameters(IndirectDrawParams{draw_params.survivors, 1, 0});
            ctx.cmd.draw_indexed_indirect(ctx.buffer(indices), ctx.buffer(args), indexed_offset);
        });
    graph.add_transfer_pass(
        "read back",
        [&](render::PassBuilder& b) {
            b.read(visible, ResourceState::copy_src);
            b.read(counter, ResourceState::copy_src);
            b.read(drawn, ResourceState::copy_src);
            b.read(drawn_indexed, ResourceState::copy_src);
            for (const render::GraphResource out : outs) {
                b.write(out, ResourceState::copy_dst);
            }
        },
        [&](render::PassContext& ctx) {
            ctx.cmd.copy_buffer(ctx.buffer(visible), ctx.buffer(outs[0]), 4);
            ctx.cmd.copy_buffer(ctx.buffer(counter), ctx.buffer(outs[1]), 4);
            ctx.cmd.copy_texture_to_buffer(ctx.texture(drawn), ctx.buffer(outs[2]));
            ctx.cmd.copy_texture_to_buffer(ctx.texture(drawn_indexed), ctx.buffer(outs[3]));
        });
    const auto compiled = graph.compile();
    REQUIRE_MESSAGE(compiled.has_value(), (compiled ? std::string() : compiled.error().message));
    auto cmd = device.begin_commands("indirect");
    graph.execute(*cmd);
    static_cast<void>(device.submit(std::move(cmd)));
    device.end_frame();
    device.wait_idle();

    IndirectResult result;
    const auto words = [&](rhi::BufferHandle b, std::uint64_t bytes) {
        const auto* data = static_cast<const std::uint32_t*>(device.map(b));
        REQUIRE(data != nullptr);
        return std::vector<std::uint32_t>(data, data + bytes / 4);
    };
    result.survivors = words(readbacks[0], 4).front();
    result.threads = words(readbacks[1], 4).front();
    result.drawn = words(readbacks[2], image_bytes);
    result.drawn_indexed = words(readbacks[3], image_bytes);
    for (const rhi::TextureHandle t : targets) {
        device.destroy(t);
    }
    for (const rhi::BufferHandle b : readbacks) {
        device.destroy(b);
    }
    return result;
}

/// Pixels that differ from the expected picture: item + 1 inside each survivor's inset quad, 0 elsewhere.
std::size_t wrong_pixels(const std::vector<std::uint32_t>& pixels) {
    std::size_t wrong = 0;
    for (std::uint32_t y = 0; y < target_size; ++y) {
        for (std::uint32_t x = 0; x < target_size; ++x) {
            const std::uint32_t item = (y / 8) * 8 + x / 8;
            const bool inside = x % 8 >= 1 && x % 8 < 7 && y % 8 >= 1 && y % 8 < 7;
            const std::uint32_t expected = inside && survives(item) ? item + 1 : 0;
            wrong += pixels.at(std::size_t{y} * target_size + x) != expected ? 1 : 0;
        }
    }
    return wrong;
}

}  // namespace

TEST_CASE("[rhi.AC1] dispatch_indirect launches the group counts a compute pass wrote") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = test::gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        const IndirectResult r = run_indirect(*device);
        CHECK(r.survivors == threshold);
        CHECK(r.threads == (threshold + 7) / 8 * 8);  // whole groups of 8 threads
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC2] a compute pass culls, and indirect draws draw exactly the survivors") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = test::gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        const IndirectResult r = run_indirect(*device);
        REQUIRE(r.survivors == threshold);
        // A two-record multi-draw whose first record is empty, and an indexed draw: one instance per survivor.
        CHECK(wrong_pixels(r.drawn) == 0);
        CHECK(wrong_pixels(r.drawn_indexed) == 0);
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC3] the null RHI checks indirect calls and runs the same frame") {
    auto device = std::move(*rhi::create_device({}));
    const IndirectResult r = run_indirect(*device);
    CHECK(r.survivors == 0);  // nothing ran
    CHECK(r.drawn.size() == std::size_t{target_size} * target_size);
}

TEST_CASE("[rhi.AC4] is_complete never waits") {
    SUBCASE("null: submissions complete at once") {
        auto device = std::move(*rhi::create_device({}));
        CHECK(device->is_complete({0}));
        static_cast<void>(device->begin_frame());
        const rhi::SubmissionId id = device->submit(device->begin_commands("nothing"));
        device->end_frame();
        CHECK(device->is_complete(id));
        CHECK_FALSE(device->is_complete({id.value + 1}));  // not submitted yet
    }
    SUBCASE("GPU: a submission is seen complete without wait()") {
        test::LogCapture logs;
        {
            const std::unique_ptr<rhi::Device> device = test::gpu_device();
            if (!device) {
                MESSAGE("skipped: no GPU device");
                return;
            }
            CullScene scene(*device);
            static_cast<void>(device->begin_frame());
            render::RenderGraph graph(*device);
            static_cast<void>(scene.add_cull(graph));
            REQUIRE(graph.compile().has_value());
            auto cmd = device->begin_commands("cull");
            graph.execute(*cmd);
            const rhi::SubmissionId id = device->submit(std::move(cmd));
            device->end_frame();
            CHECK_FALSE(device->is_complete({id.value + 1}));
            // Poll only; the GPU finishes a 64-thread dispatch in well under the bound.
            bool complete = false;
            for (int i = 0; i < 100000 && !complete; ++i) {
                complete = device->is_complete(id);
                if (!complete) {
                    std::this_thread::yield();
                }
            }
            CHECK(complete);
        }
        CHECK_MESSAGE(logs.errors().empty(), logs.errors());
    }
}

namespace {

/// One frame of `device`: begin, deliver due readbacks, record with `record`, submit.
template <class Record>
std::uint64_t run_frame(rhi::Device& device, render::ReadbackQueue& queue, Record record) {
    const std::uint64_t frame = device.begin_frame().index;
    queue.deliver();
    render::RenderGraph graph(device);
    record(graph);
    REQUIRE(graph.compile().has_value());
    auto cmd = device.begin_commands("frame");
    graph.execute(*cmd);
    queue.submitted(device.submit(std::move(cmd)));
    device.end_frame();
    return frame;
}

struct Delivery {
    std::uint64_t at = 0;  ///< the frame during which it arrived
    std::uint64_t frame = 0;
    std::vector<std::byte> bytes;
};

}  // namespace

TEST_CASE("[rhi.AC5] a readback arrives exactly frames_in_flight frames later, without stalling") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        auto null_device = std::move(*rhi::create_device({}));
        for (rhi::Device* device : {gpu.get(), null_device.get()}) {
            if (device == nullptr) {
                MESSAGE("skipped on the GPU: no GPU device");
                continue;
            }
            CAPTURE(rhi::to_string(device->backend()));
            CullScene scene(*device);
            render::ReadbackQueue queue(*device);
            REQUIRE(queue.latency() == device->frames_in_flight());
            std::vector<Delivery> deliveries;
            std::uint64_t now = 0;
            const std::uint64_t first = run_frame(*device, queue, [&](render::RenderGraph& graph) {
                const auto visible = scene.add_cull(graph);
                queue.read(graph, visible, sizeof(std::uint32_t), [&](Result<render::ReadbackData> data) {
                    REQUIRE(data.has_value());
                    deliveries.push_back({now, data->frame, data->bytes});
                });
            });
            for (int i = 0; i < 4; ++i) {
                now = device->current_frame().index + 1;  // the frame run_frame is about to begin
                static_cast<void>(run_frame(*device, queue, [](render::RenderGraph&) {}));
                if (now < first + queue.latency()) {
                    CHECK_MESSAGE(deliveries.empty(), "delivered early, at frame ", now);
                }
            }
            REQUIRE(deliveries.size() == 1);
            CHECK(deliveries[0].frame == first);
            CHECK(deliveries[0].at == first + queue.latency());
            CHECK(queue.stalls() == 0);
            CHECK(queue.pending() == 0);
            REQUIRE(deliveries[0].bytes.size() == sizeof(std::uint32_t));
            std::uint32_t survivors = 0;
            std::memcpy(&survivors, deliveries[0].bytes.data(), sizeof(survivors));
            CHECK(survivors == (device->capabilities().gpu ? threshold : 0));  // the null RHI runs nothing
        }
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC6] unsubmitted readbacks are dropped, flush delivers at once, staging buffers are reused") {
    auto device = std::move(*rhi::create_device({}));
    auto source = device->create_buffer(
        {.size = 64, .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src, .debug_name = "source"});
    REQUIRE(source.has_value());
    render::ReadbackQueue queue(*device);
    const auto read = [&](render::RenderGraph& graph, std::vector<Result<render::ReadbackData>>& out) {
        const auto r = graph.import(*source, "source", ResourceState::undefined);
        queue.read(graph, r, 64, [&](Result<render::ReadbackData> data) { out.push_back(std::move(data)); });
    };

    SUBCASE("a frame that is not submitted drops its readbacks") {
        std::vector<Result<render::ReadbackData>> out;
        static_cast<void>(device->begin_frame());
        render::RenderGraph graph(*device);
        read(graph, out);
        queue.abandon();
        device->end_frame();
        REQUIRE(out.size() == 1);
        REQUIRE_FALSE(out[0].has_value());
        CHECK(out[0].error().code == ErrorCode::render_readback_dropped);
        CHECK(queue.pending() == 0);
    }
    SUBCASE("flush waits and delivers what is in flight") {
        std::vector<Result<render::ReadbackData>> out;
        const std::uint64_t frame = run_frame(*device, queue, [&](render::RenderGraph& graph) { read(graph, out); });
        CHECK(out.empty());
        queue.flush();
        REQUIRE(out.size() == 1);
        REQUIRE(out[0].has_value());
        CHECK(out[0]->frame == frame);
        CHECK(out[0]->bytes.size() == 64);
    }
    SUBCASE("a readback every frame needs latency() staging buffers") {
        std::vector<Result<render::ReadbackData>> out;
        for (int i = 0; i < 10; ++i) {
            static_cast<void>(run_frame(*device, queue, [&](render::RenderGraph& graph) { read(graph, out); }));
        }
        CHECK(out.size() == 10 - queue.latency());
        CHECK(queue.staging_buffers() == queue.latency());
    }
    device->destroy(*source);
}

namespace {

/// A module's feature that reads a GPU buffer back every frame, through the frame context only.
class CounterFeature final : public render::RenderFeature {
public:
    explicit CounterFeature(std::vector<std::pair<std::uint64_t, std::uint64_t>>& arrivals) : arrivals_(arrivals) {}
    ~CounterFeature() override {
        if (device_ != nullptr) {
            device_->destroy(buffer_);
        }
    }
    CounterFeature(const CounterFeature&) = delete;
    CounterFeature& operator=(const CounterFeature&) = delete;
    CounterFeature(CounterFeature&&) = delete;
    CounterFeature& operator=(CounterFeature&&) = delete;

    [[nodiscard]] std::string_view name() const override { return "test.counter"; }
    [[nodiscard]] Result<> prepare(rhi::Device& device) override {
        device_ = &device;
        auto buffer = device.create_buffer(
            {.size = 16, .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src, .debug_name = "counter"});
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        buffer_ = *buffer;
        return {};
    }
    void record(render::RenderGraph& graph, const render::FrameContext& frame) override {
        REQUIRE(frame.readbacks != nullptr);
        const auto counter = graph.import(buffer_, "counter", ResourceState::undefined);
        frame.readbacks->read(graph, counter, 16, [this, &device = frame.device](Result<render::ReadbackData> data) {
            REQUIRE(data.has_value());
            arrivals_.emplace_back(data->frame, device.current_frame().index);
        });
    }

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>>& arrivals_;
    rhi::Device* device_ = nullptr;
    rhi::BufferHandle buffer_;
};

}  // namespace

TEST_CASE("[rhi.AC7] features read back through FrameContext and get bytes frames_in_flight frames later") {
    test::LogCapture logs;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> arrivals;  // (recorded in, delivered in)
    {
        auto device = std::move(*rhi::create_device({}));
        {
            render::Renderer renderer(*device);
            renderer.add_feature(std::make_unique<CounterFeature>(arrivals));
            for (int i = 0; i < 5; ++i) {
                renderer.render();
            }
            CHECK(renderer.readbacks().pending() == renderer.readbacks().latency());
        }
    }
    REQUIRE(arrivals.size() == 3);  // frames 1 to 3 of 5, with 2 frames in flight
    for (const auto& [recorded, delivered] : arrivals) {
        CHECK(delivered == recorded + 2);
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
