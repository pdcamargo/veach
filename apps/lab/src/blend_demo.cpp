#include "blend_demo.hpp"

#include "lab_shaders.hpp"

#include <array>

namespace lab {

namespace {

namespace rhi = veach::rhi;
namespace render = veach::render;

struct BlendDemoState {
    float opacity = 0.5f;
};

/// Mirrors `BlendDemoParams` in shaders/blend_demo.slang.
struct alignas(16) BlendDemoParams {
    std::array<float, 4> color{};
    std::array<float, 4> rect{};
};
static_assert(rhi::parameters_match<BlendDemoParams>(lab_shaders::layout::blend_demo,
                                                     {VEACH_PARAM(BlendDemoParams, color),
                                                      VEACH_PARAM(BlendDemoParams, rect)}));

constexpr std::array<rhi::BlendMode, 4> modes{rhi::BlendMode::opaque, rhi::BlendMode::alpha,
                                              rhi::BlendMode::premultiplied, rhi::BlendMode::additive};

class BlendDemoFeature final : public render::RenderFeature {
public:
    [[nodiscard]] std::string_view name() const override { return "lab.blend_demo"; }

    [[nodiscard]] veach::Result<> prepare(rhi::Device& device) override {
        for (std::size_t i = 0; i < modes.size(); ++i) {
            rhi::GraphicsPipelineDesc desc;
            desc.library = &lab_shaders::blend_demo();
            desc.vertex_entry = "vs_main";
            desc.fragment_entry = "fs_main";
            desc.color_formats = {render::scene_color_format};
            desc.blend = {modes.at(i)};
            desc.debug_name = "blend demo";
            auto pipeline = device.create_graphics_pipeline(desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            pipelines_.at(i) = *pipeline;
        }
        return {};
    }

    void record(render::RenderGraph& graph, const render::FrameContext& frame) override {
        const BlendDemoState* state = frame.world.get<BlendDemoState>();
        if (!state) {
            return;
        }
        const float a = state->opacity;
        graph.add_graphics_pass(
            "blend demo", [&](render::PassBuilder& b) { b.color_target(frame.scene_color, rhi::LoadOp::load); },
            [this, a](render::PassContext& ctx) {
                // Opaque stripes: red, green and blue, top to bottom; a white one through the middle of each column.
                const std::array<std::array<float, 4>, 3> stripes{
                    {{0.8f, 0.15f, 0.1f, 1.0f}, {0.1f, 0.6f, 0.2f, 1.0f}, {0.1f, 0.25f, 0.8f, 1.0f}}};
                ctx.cmd.set_pipeline(pipelines_.at(0));
                for (std::size_t i = 0; i < stripes.size(); ++i) {
                    const float top = 0.8f - 0.5f * static_cast<float>(i);
                    ctx.cmd.set_parameters(BlendDemoParams{stripes.at(i), {-0.95f, top - 0.4f, 0.95f, top}});
                    ctx.cmd.draw(6);
                }
                ctx.cmd.set_parameters(BlendDemoParams{{1.0f, 1.0f, 1.0f, 1.0f}, {-0.95f, -0.05f, 0.95f, 0.05f}});
                ctx.cmd.draw(6);
                // One column per mode, the same amber at opacity a: premultiplied and additive get it multiplied in.
                const std::array<float, 3> amber{1.0f, 0.7f, 0.1f};
                for (std::size_t i = 0; i < modes.size(); ++i) {
                    const bool multiplied =
                        modes.at(i) == rhi::BlendMode::premultiplied || modes.at(i) == rhi::BlendMode::additive;
                    const float k = multiplied ? a : 1.0f;
                    const float x0 = -0.85f + 0.45f * static_cast<float>(i);
                    ctx.cmd.set_pipeline(pipelines_.at(i));
                    ctx.cmd.set_parameters(
                        BlendDemoParams{{amber[0] * k, amber[1] * k, amber[2] * k, a}, {x0, -0.9f, x0 + 0.3f, 0.9f}});
                    ctx.cmd.draw(6);
                }
            });
    }

private:
    std::array<rhi::PipelineHandle, modes.size()> pipelines_{};
};

}  // namespace

void register_blend_demo(veach::World& world) {
    world.component<BlendDemo>().member(&BlendDemo::opacity, "opacity").add<veach::Scriptable>();
    auto& renderer = veach::services(world).get<render::Renderer>();
    renderer.add_extractor("lab.blend_demo", [](const veach::World& w, render::RenderWorld& out) {
        w.each([&](const BlendDemo& demo) { out.emplace<BlendDemoState>({demo.opacity}); });
    });
    renderer.add_feature(std::make_unique<BlendDemoFeature>());
}

}  // namespace lab
