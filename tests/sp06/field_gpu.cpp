// Spike SP-06 (VCH-082): both GPU evaluators as render-graph compute passes, timed by the graph (VCH-049).

#include "sp06/field_gpu.hpp"

#include "sp06_shaders.hpp"

#include <veach/render/render_graph.hpp>
#include <veach/rhi/shader.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace veach::sp06 {

namespace {

// Mirrors FieldParams in shaders/sp06_field_noise.slang.
struct alignas(16) FieldParams {
    rhi::DescriptorHandle program;
    rhi::DescriptorHandle output;
    float center[4];
    float right[4];
    float up[4];
    std::uint32_t resolution = 0;
    std::uint32_t words = 0;
    std::uint32_t result = 0;
    std::uint32_t unused = 0;
};
static_assert(rhi::parameters_match<FieldParams>(sp06_shaders::layout::sp06_field_interpreter,
                                                 {VEACH_PARAM(FieldParams, program), VEACH_PARAM(FieldParams, output),
                                                  VEACH_PARAM(FieldParams, center), VEACH_PARAM(FieldParams, right),
                                                  VEACH_PARAM(FieldParams, up), VEACH_PARAM(FieldParams, resolution),
                                                  VEACH_PARAM(FieldParams, words), VEACH_PARAM(FieldParams, result),
                                                  VEACH_PARAM(FieldParams, unused)}));
static_assert(rhi::parameters_match<FieldParams>(sp06_shaders::layout::sp06_field_handwritten,
                                                 {VEACH_PARAM(FieldParams, program), VEACH_PARAM(FieldParams, output),
                                                  VEACH_PARAM(FieldParams, center), VEACH_PARAM(FieldParams, right),
                                                  VEACH_PARAM(FieldParams, up), VEACH_PARAM(FieldParams, resolution),
                                                  VEACH_PARAM(FieldParams, words), VEACH_PARAM(FieldParams, result),
                                                  VEACH_PARAM(FieldParams, unused)}));

constexpr std::string_view interpreter_pass = "sp06: field interpreter";
constexpr std::string_view handwritten_pass = "sp06: field hand-written";

/// Destroys what it holds when the run ends, on every path.
struct Resources {
    explicit Resources(rhi::Device& d) : device(d) {}
    rhi::Device& device;
    std::vector<rhi::BufferHandle> buffers;
    std::vector<rhi::PipelineHandle> pipelines;
    ~Resources() {
        device.wait_idle();
        for (const rhi::BufferHandle b : buffers) {
            device.destroy(b);
        }
        for (const rhi::PipelineHandle p : pipelines) {
            device.destroy(p);
        }
    }
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;
    Resources(Resources&&) = delete;
    Resources& operator=(Resources&&) = delete;
};

}  // namespace

Result<GpuRun> run_gpu(rhi::Device& device, const Program& program, const FaceRegion& region, std::uint32_t frames) {
    const std::uint32_t res = region.resolution;
    if (res == 0 || res % 8 != 0) {
        return make_error(ErrorCode::cli_invalid_value, "sp06: the resolution must be a multiple of 8");
    }
    Resources owned(device);
    const std::uint64_t bytes = std::uint64_t{res} * res * sizeof(float);

    auto make_pipeline = [&](std::string_view entry) -> Result<rhi::PipelineHandle> {
        const rhi::ShaderLibrary& library = entry == "field_interpret" ? sp06_shaders::sp06_field_interpreter()
                                                                       : sp06_shaders::sp06_field_handwritten();
        auto p = device.create_compute_pipeline({&library, entry, {8, 8, 1}, std::string(entry)});
        if (p) {
            owned.pipelines.push_back(*p);
        }
        return p;
    };
    auto make_buffer = [&](const rhi::BufferDesc& desc,
                           std::span<const std::byte> data = {}) -> Result<rhi::BufferHandle> {
        auto b = device.create_buffer(desc, data);
        if (b) {
            owned.buffers.push_back(*b);
        }
        return b;
    };

    auto interpreter = make_pipeline("field_interpret");
    if (!interpreter) {
        return std::unexpected(interpreter.error());
    }
    auto handwritten = make_pipeline("field_handwritten");
    if (!handwritten) {
        return std::unexpected(handwritten.error());
    }
    auto ops = make_buffer({.size = program.words.size() * sizeof(std::uint32_t),
                            .debug_name = "sp06 op list",
                            .stride = sizeof(std::uint32_t)},
                           std::as_bytes(std::span(program.words)));
    std::array<rhi::BufferHandle, 2> outputs{};
    std::array<rhi::BufferHandle, 2> readbacks{};
    for (std::size_t i = 0; i < 2; ++i) {
        auto out = make_buffer({.size = bytes,
                                .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src,
                                .debug_name = i == 0 ? "sp06 interpreted" : "sp06 hand-written",
                                .stride = sizeof(float)});
        auto back = make_buffer({.size = bytes,
                                 .usage = rhi::BufferUsage::copy_dst,
                                 .memory = rhi::MemoryType::readback,
                                 .debug_name = "sp06 readback"});
        if (!out || !back) {
            return std::unexpected(out ? back.error() : out.error());
        }
        outputs[i] = *out;
        readbacks[i] = *back;
    }
    if (!ops) {
        return std::unexpected(ops.error());
    }

    FieldParams params{};
    params.program = device.descriptor(*ops);
    std::memcpy(params.center, region.center, sizeof(region.center));
    params.center[3] = 2.0f / static_cast<float>(res);
    std::memcpy(params.right, region.right, sizeof(region.right));
    std::memcpy(params.up, region.up, sizeof(region.up));
    params.resolution = res;
    params.words = static_cast<std::uint32_t>(program.words.size());
    params.result = program.output;
    std::array<FieldParams, 2> per_kernel{params, params};
    per_kernel[0].output = device.descriptor(outputs[0], rhi::View::storage);
    per_kernel[1].output = device.descriptor(outputs[1], rhi::View::storage);
    const std::array<rhi::PipelineHandle, 2> pipelines{*interpreter, *handwritten};
    const std::array<std::string_view, 2> names{interpreter_pass, handwritten_pass};

    GpuRun run;
    std::array<rhi::ResourceState, 2> state{rhi::ResourceState::undefined, rhi::ResourceState::undefined};
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const bool last = frame + 1 == frames;
        static_cast<void>(device.begin_frame());
        render::RenderGraph graph(device);
        const render::GraphResource op_list = graph.import(*ops, "sp06 op list", rhi::ResourceState::shader_read);
        std::array<render::GraphResource, 2> out{};
        for (std::size_t i = 0; i < 2; ++i) {
            out[i] = graph.import(outputs[i], std::string(names[i]), state[i]);
        }
        for (std::size_t n = 0; n < 2; ++n) {
            const std::size_t k = (frame % 2 == 0) ? n : 1 - n;  // alternate which kernel runs first
            graph.add_compute_pass(
                std::string(names[k]),
                [&, k](render::PassBuilder& b) {
                    b.read(op_list);
                    b.write(out[k]);
                },
                [&, k](render::PassContext& ctx) {
                    ctx.cmd.set_pipeline(pipelines[k]);
                    ctx.cmd.set_parameters(per_kernel[k]);
                    ctx.cmd.dispatch(res / 8, res / 8);
                });
        }
        if (last) {
            for (std::size_t i = 0; i < 2; ++i) {
                const render::GraphResource back =
                    graph.import(readbacks[i], "sp06 readback", rhi::ResourceState::undefined);
                graph.add_transfer_pass(
                    "sp06: read back",
                    [&, i, back](render::PassBuilder& b) {
                        b.read(out[i], rhi::ResourceState::copy_src);
                        b.write(back, rhi::ResourceState::copy_dst);
                    },
                    [&, i, back](render::PassContext& ctx) {
                        ctx.cmd.copy_buffer(ctx.buffer(out[i]), ctx.buffer(back), bytes);
                    });
            }
        }
        if (auto compiled = graph.compile(); !compiled) {
            return std::unexpected(compiled.error());
        }
        auto cmd = device.begin_commands("sp06 field graph");
        graph.execute(*cmd);
        static_cast<void>(device.submit(std::move(cmd)));
        device.end_frame();
        device.wait_idle();
        for (std::size_t i = 0; i < 2; ++i) {
            state[i] = graph.final_state(out[i]);
        }
        const rhi::FrameTimings timings = device.last_frame_timings();
        run.timing_unavailable = timings.unavailable;
        for (const rhi::PassTiming& t : timings.passes) {
            if (t.name == interpreter_pass) {
                run.interpreted_ms.push_back(t.gpu_ms);
            } else if (t.name == handwritten_pass) {
                run.handwritten_ms.push_back(t.gpu_ms);
            }
        }
    }

    for (std::size_t i = 0; i < 2; ++i) {
        const auto* values = static_cast<const float*>(device.map(readbacks[i]));
        if (values == nullptr) {
            return make_error(ErrorCode::internal, "sp06: a readback buffer has no host pointer");
        }
        (i == 0 ? run.interpreted : run.handwritten).assign(values, values + std::size_t{res} * res);
    }
    return run;
}

}  // namespace veach::sp06
