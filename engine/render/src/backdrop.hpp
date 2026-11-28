#pragma once

// The space backdrop's GPU side (ADR 0090): bakes render::Backdrop's band and nebulae into a lat-long texture when
// they change, and hands the sky (atmosphere.cpp) the texture to sample. The data and the generator are public:
// <veach/render/backdrop.hpp>.

#include <veach/core/error.hpp>
#include <veach/render/backdrop.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/rhi/rhi.hpp>

#include <memory>
#include <optional>

namespace veach::render {

/// The baked texture's width for `backdrop` (its height is half).
[[nodiscard]] std::uint32_t backdrop_width(const Backdrop& backdrop);

class BackdropRenderer {
public:
    BackdropRenderer();
    ~BackdropRenderer();
    BackdropRenderer(const BackdropRenderer&) = delete;
    BackdropRenderer& operator=(const BackdropRenderer&) = delete;
    BackdropRenderer(BackdropRenderer&&) = delete;
    BackdropRenderer& operator=(BackdropRenderer&&) = delete;

    [[nodiscard]] Result<> prepare(rhi::Device& device);
    /// This frame's backdrop texture, imported into `graph`, baked first when `backdrop` bakes differently from the
    /// last one; nullopt when it cannot be made (logged).
    [[nodiscard]] std::optional<GraphResource> record(RenderGraph& graph, const Backdrop& backdrop);
    [[nodiscard]] rhi::TextureHandle texture() const;
    [[nodiscard]] rhi::SamplerHandle sampler() const;
    /// How many times it has baked (tests).
    [[nodiscard]] std::uint32_t bakes() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::render
