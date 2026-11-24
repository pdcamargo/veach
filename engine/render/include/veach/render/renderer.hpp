#pragma once

// The extraction boundary (ADR 0016). Placeholder renderer: no drawing yet.
//
// Renderer data — BVHs, GPU buffers, light structures, reservoirs — lives in
// RenderWorld, outside the ECS. The Extract phase is the ONLY place where ECS
// data crosses into the renderer: extractors read components and write
// RenderWorld. Gameplay never sees GPU handles, and the renderer never reads
// the ECS outside extraction.
//
// Modules extend extraction instead of reaching into the renderer:
//   veach::services(world).get<veach::render::Renderer>().add_extractor(
//       "lights", [](const veach::World& w, veach::render::RenderWorld& rw) { ... });

#include <veach/ecs/flecs.hpp>
#include <veach/rhi/rhi.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace veach::render {

/// Renderer-owned snapshot of what to draw this frame.
struct RenderWorld {
    std::uint64_t frame = 0;
    std::uint32_t window_count = 0;  // placeholder until views/cameras exist (VCH-011)
};

using Extractor = std::function<void(const World& world, RenderWorld& out)>;

class Renderer {
public:
    explicit Renderer(rhi::Device& device);

    void add_extractor(std::string name, Extractor extractor);

    /// Extract phase: clears RenderWorld and runs every extractor in registration order.
    void extract(const World& world);
    /// Render phase: consumes RenderWorld. No-op until the render graph lands (VCH-010).
    void render();

    [[nodiscard]] const RenderWorld& render_world() const { return render_world_; }
    [[nodiscard]] rhi::Device& device() { return device_; }

private:
    struct NamedExtractor {
        std::string name;
        Extractor fn;
    };

    rhi::Device& device_;
    RenderWorld render_world_;
    std::vector<NamedExtractor> extractors_;
};

/// Registers the Extract and Render phase systems. Requires a Renderer service.
struct RenderModule {
    explicit RenderModule(World& world);
};

}  // namespace veach::render
