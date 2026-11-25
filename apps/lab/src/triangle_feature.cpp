#include "triangle_feature.hpp"

#include "lab_shaders.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <span>

namespace lab {

namespace {

namespace rhi = veach::rhi;
namespace render = veach::render;

/// What the extractor hands the renderer (render::RenderWorld, ADR 0016).
struct TriangleState {
    TriangleMode mode = TriangleMode::raster;
};

/// Mirrors `Vertex` in shaders/triangle*.slang.
struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};
static_assert(sizeof(Vertex) == 32);

constexpr std::array<Vertex, 3> triangle{{
    {{0.0f, 0.7f, 0.5f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    {{-0.7f, -0.6f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    {{0.7f, -0.6f, 0.5f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
}};

/// Mirrors `Params` in shaders/triangle.slang (ADR 0024 layout rules).
struct alignas(16) RasterParams {       // Metal rounds a struct with a float4x4 up to 16 bytes
    veach::math::mat4 view_projection;  ///< identity without a camera: the vertices are already NDC
    rhi::DescriptorHandle vertices;
};
static_assert(rhi::parameters_match<RasterParams>(lab_shaders::layout::triangle,
                                                  {VEACH_PARAM(RasterParams, view_projection),
                                                   VEACH_PARAM(RasterParams, vertices)}));

/// Mirrors `Params` in shaders/triangle_rt.slang.
struct TraceParams {
    veach::math::mat4 inverse_view_projection;  ///< identity without a camera: orthographic rays through NDC
    rhi::DescriptorHandle output;
    rhi::DescriptorHandle scene;
    rhi::DescriptorHandle vertices;
    std::array<std::uint32_t, 2> size;
};
static_assert(rhi::parameters_match<TraceParams>(lab_shaders::layout::triangle_rt,
                                                 {VEACH_PARAM(TraceParams, inverse_view_projection),
                                                  VEACH_PARAM(TraceParams, output), VEACH_PARAM(TraceParams, scene),
                                                  VEACH_PARAM(TraceParams, vertices), VEACH_PARAM(TraceParams, size)}));

class TriangleFeature final : public render::RenderFeature {
public:
    [[nodiscard]] std::string_view name() const override { return "lab.triangle"; }

    [[nodiscard]] veach::Result<> prepare(rhi::Device& device) override {
        auto vertices =
            device.create_buffer({sizeof(triangle), rhi::BufferUsage::storage | rhi::BufferUsage::accel_input,
                                  rhi::MemoryType::gpu, "triangle vertices", sizeof(Vertex)},
                                 std::as_bytes(std::span(triangle)));
        if (!vertices) {
            return std::unexpected(vertices.error());
        }
        vertices_ = *vertices;

        rhi::GraphicsPipelineDesc raster;
        raster.library = &lab_shaders::triangle();
        raster.vertex_entry = "vs_main";
        raster.fragment_entry = "fs_main";
        raster.color_formats = {render::scene_color_format};
        raster.debug_name = "triangle (raster)";
        auto pipeline = device.create_graphics_pipeline(raster);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        raster_ = *pipeline;

        if (!device.capabilities().hardware_ray_tracing) {
            rt_error_ = veach::Error{veach::ErrorCode::rhi_backend_unavailable,
                                     std::format("'{}' has no hardware ray tracing", device.capabilities().device_name),
                                     "The raytraced triangle needs an Apple GPU with Metal ray tracing."};
            return {};
        }
        auto trace =
            device.create_compute_pipeline({&lab_shaders::triangle_rt(), "trace", {8, 8, 1}, "triangle (ray query)"});
        if (!trace) {
            return std::unexpected(trace.error());
        }
        trace_ = *trace;

        auto blas = device.create_blas({vertices_, 3, sizeof(Vertex), {}, 0, true, true, "triangle BLAS"});
        if (!blas) {
            return std::unexpected(blas.error());
        }
        blas_ = *blas;
        const std::array<rhi::AccelHandle, 1> blases{blas_};
        auto tlas = device.create_tlas({1, blases, true, "triangle TLAS"});
        if (!tlas) {
            return std::unexpected(tlas.error());
        }
        tlas_ = *tlas;
        return {};
    }

    void record(render::RenderGraph& graph, const render::FrameContext& frame) override {
        const TriangleState* state = frame.world.get<TriangleState>();
        if (!state) {
            return;  // no TriangleDemo entity: draw nothing
        }
        const render::GraphResource vertices =
            graph.import(vertices_, "triangle vertices", rhi::ResourceState::shader_read);
        // Through the primary camera if there is one (VCH-011); otherwise the vertices are NDC as they are.
        const veach::math::mat4 view_projection = frame.view ? frame.view->view_projection : veach::math::mat4(1.0f);
        const veach::math::mat4 inverse_view_projection =
            frame.view ? frame.view->inverse_view_projection : veach::math::mat4(1.0f);
        if (state->mode == TriangleMode::raster) {
            graph.add_graphics_pass(
                "triangle (raster)",
                [&](render::PassBuilder& b) {
                    b.read(vertices);
                    b.color_target(frame.scene_color, rhi::LoadOp::load);
                },
                [this, vertices, view_projection](render::PassContext& ctx) {
                    ctx.cmd.set_pipeline(raster_);
                    ctx.cmd.set_parameters(RasterParams{view_projection, ctx.device.descriptor(ctx.buffer(vertices))});
                    ctx.cmd.draw(3);
                });
            return;
        }

        if (!tlas_.is_valid()) {
            if (!rt_error_reported_) {
                veach::log::error("lab", *rt_error_);
                rt_error_reported_ = true;
            }
            return;
        }
        const auto as_state = built_ ? rhi::ResourceState::accel_read : rhi::ResourceState::undefined;
        const render::GraphResource blas = graph.import(blas_, "triangle BLAS", as_state);
        const render::GraphResource tlas = graph.import(tlas_, "triangle TLAS", as_state);
        const std::array<rhi::TlasInstance, 1> instances{};  // identity transform, BLAS 0

        if (!built_) {
            graph.add_transfer_pass(
                "build triangle BLAS+TLAS",
                [&](render::PassBuilder& b) {
                    b.read(vertices);
                    b.write(blas, rhi::ResourceState::accel_build);
                    b.write(tlas, rhi::ResourceState::accel_build);
                },
                [this, instances](render::PassContext& ctx) {
                    ctx.cmd.build_blas(blas_);
                    ctx.cmd.build_tlas(tlas_, instances);
                });
            built_ = true;
        } else {
            // The triangle doesn't move; refitting every frame keeps the update path exercised.
            graph.add_transfer_pass(
                "refit triangle TLAS", [&](render::PassBuilder& b) { b.write(tlas, rhi::ResourceState::accel_build); },
                [this, instances](render::PassContext& ctx) { ctx.cmd.refit_tlas(tlas_, instances); });
        }

        const std::uint32_t width = frame.width;
        const std::uint32_t height = frame.height;
        graph.add_compute_pass(
            "triangle (ray query)",
            [&](render::PassBuilder& b) {
                b.read(tlas, rhi::ResourceState::accel_read);
                b.read(blas, rhi::ResourceState::accel_read);
                b.read(vertices);
                b.write(frame.scene_color, rhi::ResourceState::shader_write);
            },
            [this, vertices, tlas, color = frame.scene_color, width, height,
             inverse_view_projection](render::PassContext& ctx) {
                ctx.cmd.set_pipeline(trace_);
                ctx.cmd.set_parameters(TraceParams{inverse_view_projection,
                                                   ctx.device.descriptor(ctx.texture(color), rhi::View::storage),
                                                   ctx.device.descriptor(ctx.accel(tlas)),
                                                   ctx.device.descriptor(ctx.buffer(vertices)),
                                                   {width, height}});
                ctx.cmd.dispatch((width + 7) / 8, (height + 7) / 8);
            });
    }

private:
    rhi::BufferHandle vertices_;
    rhi::PipelineHandle raster_;
    rhi::PipelineHandle trace_;
    rhi::AccelHandle blas_;
    rhi::AccelHandle tlas_;
    bool built_ = false;
    std::optional<veach::Error> rt_error_;
    bool rt_error_reported_ = false;
};

}  // namespace

void register_triangle_demo(veach::World& world) {
    world.component<TriangleMode>();
    world.component<TriangleDemo>().member(&TriangleDemo::mode, "mode").add<veach::Scriptable>();

    auto& renderer = veach::services(world).get<render::Renderer>();
    renderer.add_extractor("lab.triangle", [](const veach::World& w, render::RenderWorld& out) {
        w.each([&](const TriangleDemo& demo) { out.emplace<TriangleState>({demo.mode}); });
    });
    renderer.add_feature(std::make_unique<TriangleFeature>());
}

}  // namespace lab
