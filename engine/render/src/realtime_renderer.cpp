#include "realtime_renderer.hpp"

#include <veach/asset/scene.hpp>
#include <veach/core/profile.hpp>

#include "scene_records.hpp"
#include "veach_render_shaders.hpp"

#include <array>
#include <vector>

namespace veach::render {

namespace {

/// Mirrors VisibilityParams in shaders/realtime_visibility.slang.
struct alignas(16) VisibilityParams {
    math::mat4 view_projection{1.0f};
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    std::uint32_t instance = 0;
    std::uint32_t pad0 = 0;
    std::array<std::uint32_t, 2> pad1{};
};
static_assert(rhi::parameters_match<VisibilityParams>(
    veach_render_shaders::layout::realtime_visibility,
    {VEACH_PARAM(VisibilityParams, view_projection), VEACH_PARAM(VisibilityParams, instances),
     VEACH_PARAM(VisibilityParams, materials), VEACH_PARAM(VisibilityParams, instance),
     VEACH_PARAM(VisibilityParams, pad0), VEACH_PARAM(VisibilityParams, pad1)}));

/// Mirrors ShadeParams in shaders/realtime_shade.slang.
struct alignas(16) ShadeParams {
    math::mat4 inverse_view_projection{1.0f};
    math::vec4 camera_position{0.0f};  ///< xyz; w = exposure
    rhi::DescriptorHandle visibility;
    rhi::DescriptorHandle radiance;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle lights;
    rhi::DescriptorHandle sky;
    std::array<std::uint32_t, 2> size{};
    std::uint32_t light_count = 0;
    std::uint32_t pad = 0;
};
static_assert(rhi::parameters_match<ShadeParams>(
    veach_render_shaders::layout::realtime_shade,
    {VEACH_PARAM(ShadeParams, inverse_view_projection), VEACH_PARAM(ShadeParams, camera_position),
     VEACH_PARAM(ShadeParams, visibility), VEACH_PARAM(ShadeParams, radiance), VEACH_PARAM(ShadeParams, instances),
     VEACH_PARAM(ShadeParams, materials), VEACH_PARAM(ShadeParams, lights), VEACH_PARAM(ShadeParams, sky),
     VEACH_PARAM(ShadeParams, size), VEACH_PARAM(ShadeParams, light_count), VEACH_PARAM(ShadeParams, pad)}));

constexpr std::uint32_t no_geometry = 0xFFFFFFFFu;  ///< the visibility buffer's clear value

}  // namespace

struct RealtimeRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::PipelineHandle visibility_pipeline;
    rhi::PipelineHandle shade_pipeline;
    rhi::PipelineHandle display_pipeline;

    Impl() = default;
    ~Impl() {
        if (device == nullptr) {
            return;
        }
        for (rhi::PipelineHandle* p : {&visibility_pipeline, &shade_pipeline, &display_pipeline}) {
            if (p->is_valid()) {
                device->destroy(*p);
            }
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

RealtimeRenderer::RealtimeRenderer() : impl_(std::make_unique<Impl>()) {}

RealtimeRenderer::~RealtimeRenderer() = default;

Result<> RealtimeRenderer::prepare(rhi::Device& device, const SceneShaders& shaders) {
    Impl& s = *impl_;
    s.device = &device;
    rhi::GraphicsPipelineDesc visibility;
    visibility.library = &shaders.visibility;
    visibility.vertex_entry = "vis_vs";
    visibility.fragment_entry = "vis_fs";
    visibility.color_formats = {rhi::Format::rg32_uint};
    visibility.depth_format = rhi::Format::depth32_float;
    visibility.depth_test = true;
    visibility.depth_write = true;
    visibility.depth_compare = rhi::CompareOp::greater;  // reverse-Z: nearer is greater
    visibility.cull = rhi::CullMode::none;               // every surface is two-sided, as in photo mode
    visibility.debug_name = "visibility";
    auto visibility_pipeline = device.create_graphics_pipeline(visibility);
    if (!visibility_pipeline) {
        return std::unexpected(visibility_pipeline.error());
    }
    s.visibility_pipeline = *visibility_pipeline;
    auto shade = device.create_compute_pipeline({&shaders.shade, "shade", {8, 8, 1}, "shade"});
    if (!shade) {
        return std::unexpected(shade.error());
    }
    s.shade_pipeline = *shade;
    auto display = device.create_compute_pipeline({&veach_render_shaders::display(), "display", {8, 8, 1}, "display"});
    if (!display) {
        return std::unexpected(display.error());
    }
    s.display_pipeline = *display;
    return {};
}

void RealtimeRenderer::record(RenderGraph& graph, const FrameContext& frame, const RenderView& view,
                              const RealtimeScene& scene) {
    VEACH_ZONE("RealtimeRenderer::record");
    Impl& s = *impl_;
    const std::uint32_t width = frame.width;
    const std::uint32_t height = frame.height;
    const GraphResource visibility =
        graph.create_texture({width, height, rhi::Format::rg32_uint,
                              rhi::TextureUsage::color_target | rhi::TextureUsage::sampled, "visibility"});
    const GraphResource depth = graph.create_texture(
        {width, height, rhi::Format::depth32_float, rhi::TextureUsage::depth_target, "visibility depth"});
    const GraphResource radiance =
        graph.create_texture({width, height, rhi::Format::rgba16_float,
                              rhi::TextureUsage::storage | rhi::TextureUsage::sampled, "radiance"});

    // ---- visibility (ADR 0040): one draw per instance, one draw instance per meshlet, vertex pulling.
    VisibilityParams vis{.view_projection = view.view_projection};
    graph.add_graphics_pass(
        "visibility",
        [&](PassBuilder& b) {
            b.color_target_uint(visibility, rhi::LoadOp::clear, {no_geometry, no_geometry, 0u, 0u});
            b.depth_target(depth, rhi::LoadOp::clear, 0.0f);  // reverse-Z: far is 0
            scene.read_geometry(b);
        },
        [vis, counts = std::vector<std::uint32_t>(scene.meshlet_counts.begin(), scene.meshlet_counts.end()),
         pipeline = s.visibility_pipeline, instances = scene.instances,
         materials = scene.materials](PassContext& ctx) mutable {
            vis.instances = ctx.device.descriptor(instances);
            vis.materials = ctx.device.descriptor(materials);
            ctx.cmd.set_pipeline(pipeline);
            for (std::size_t i = 0; i < counts.size(); ++i) {
                if (counts[i] == 0) {
                    continue;
                }
                vis.instance = static_cast<std::uint32_t>(i);
                ctx.cmd.set_parameters(vis);
                ctx.cmd.draw(meshlet_max_triangles * 3, counts[i]);
            }
        });

    // ---- shade: lights, the sky's ambient light, emission, the sky and aerial perspective, exposed.
    ShadeParams shade{.inverse_view_projection = view.inverse_view_projection,
                      .camera_position = math::vec4(view.position, view.exposure),
                      .size = {width, height},
                      .light_count = scene.light_count};
    graph.add_compute_pass(
        "shade",
        [&](PassBuilder& b) {
            scene.read_geometry(b);
            scene.read_sky(b);
            b.read(visibility);
            b.write(radiance, rhi::ResourceState::shader_write);
        },
        [shade, pipeline = s.shade_pipeline, visibility, radiance, instances = scene.instances,
         materials = scene.materials, lights = scene.lights, sky = scene.sky](PassContext& ctx) mutable {
            shade.visibility = ctx.device.descriptor(ctx.texture(visibility));
            shade.radiance = ctx.device.descriptor(ctx.texture(radiance), rhi::View::storage);
            shade.instances = ctx.device.descriptor(instances);
            shade.materials = ctx.device.descriptor(materials);
            shade.lights = ctx.device.descriptor(lights);
            shade.sky = ctx.device.descriptor(sky);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(shade);
            ctx.cmd.dispatch((shade.size[0] + 7) / 8, (shade.size[1] + 7) / 8);
        });

    // ---- display: AgX on the already exposed radiance (ADR 0037).
    DisplayParams display;
    display.size = {width, height};
    display.input_size = {width, height};
    display.exposure = 1.0f;
    display.tonemap = 1;
    graph.add_compute_pass(
        "display",
        [&](PassBuilder& b) {
            b.read(radiance);
            b.write(frame.scene_color, rhi::ResourceState::shader_write);
        },
        [display, pipeline = s.display_pipeline, radiance, color = frame.scene_color](PassContext& ctx) mutable {
            display.radiance = ctx.device.descriptor(ctx.texture(radiance));
            display.output = ctx.device.descriptor(ctx.texture(color), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(display);
            ctx.cmd.dispatch((display.size[0] + 7) / 8, (display.size[1] + 7) / 8);
        });
}

}  // namespace veach::render
