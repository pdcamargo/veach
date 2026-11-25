#pragma once

// The physical atmosphere's lookup tables (ADR 0050), built by compute passes each frame for both renderers. The
// CPU side (the sun, transmittance) is public: <veach/render/atmosphere.hpp>.

#include <veach/core/error.hpp>
#include <veach/render/atmosphere.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/scene.hpp>

#include <functional>
#include <memory>

namespace veach::render {

/// What the frame's passes read about the sky: one structured-buffer element and the LUTs.
struct SkyResources {
    rhi::BufferHandle frame;                 ///< one SkyFrame
    std::function<void(PassBuilder&)> read;  ///< declares the buffer and the LUTs
};

class AtmosphereRenderer {
public:
    AtmosphereRenderer();
    ~AtmosphereRenderer();
    AtmosphereRenderer(const AtmosphereRenderer&) = delete;
    AtmosphereRenderer& operator=(const AtmosphereRenderer&) = delete;
    AtmosphereRenderer(AtmosphereRenderer&&) = delete;
    AtmosphereRenderer& operator=(AtmosphereRenderer&&) = delete;

    [[nodiscard]] Result<> prepare(rhi::Device& device);
    /// Builds this frame's tables for `view` (the physical sky only) and uploads its SkyFrame.
    [[nodiscard]] SkyResources record(RenderGraph& graph, const ResolvedSky& sky, const RenderView& view);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
