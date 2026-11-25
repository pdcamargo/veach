#pragma once

// The renderer and the extraction boundary (ADR 0016).
//
// Renderer data (GPU resources, acceleration structures) lives outside the ECS.
// The Extract phase is the ONLY place ECS data crosses into the renderer:
// extractors read components and write RenderWorld. The Render phase builds a
// render graph (ADR 0025) from the registered features and submits it.
//
// Modules extend rendering without touching renderer internals:
//   auto& renderer = veach::services(world).get<veach::render::Renderer>();
//   renderer.add_extractor("spin", [](const veach::World& w, veach::render::RenderWorld& rw) {...});
//   renderer.add_feature(std::make_unique<MyFeature>());

#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/rhi/rhi.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace veach::render {

/// Renderer-owned snapshot of what to draw this frame. Rebuilt by extraction every frame.
class RenderWorld {
public:
    std::uint64_t frame = 0;
    std::uint32_t target_width = 0;  ///< pixels: the primary window's drawable, or its Window size headless
    std::uint32_t target_height = 0;
    std::uint32_t window_count = 0;

    /// Per-frame data extracted by modules, one value per type.
    template <class T>
    T& emplace(T value = {}) {
        auto holder = std::make_shared<T>(std::move(value));
        T& ref = *holder;
        data_[std::type_index(typeid(T))] = std::move(holder);
        return ref;
    }
    template <class T>
    [[nodiscard]] const T* get() const {
        const auto it = data_.find(std::type_index(typeid(T)));
        return it == data_.end() ? nullptr : static_cast<const T*>(it->second.get());
    }

private:
    std::unordered_map<std::type_index, std::shared_ptr<void>> data_;
};

using Extractor = std::function<void(const World& world, RenderWorld& out)>;

/// What a feature gets each frame.
struct FrameContext {
    rhi::Device& device;
    const RenderWorld& world;
    GraphResource scene_color;  ///< rgba8_unorm; cleared to the background color by the renderer first
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// A unit of rendering contributed by a module: creates its GPU resources once
/// and records passes into each frame's graph.
class RenderFeature {
public:
    virtual ~RenderFeature() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    /// Called once before the first frame. A failure disables the feature (and is logged).
    [[nodiscard]] virtual Result<> prepare(rhi::Device& device) = 0;
    virtual void record(RenderGraph& graph, const FrameContext& frame) = 0;
};

inline constexpr rhi::Format scene_color_format = rhi::Format::rgba8_unorm;
inline constexpr std::array<float, 4> background_color{0.05f, 0.05f, 0.08f, 1.0f};

class Renderer {
public:
    explicit Renderer(rhi::Device& device);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void add_extractor(std::string name, Extractor extractor);
    void add_feature(std::unique_ptr<RenderFeature> feature);
    /// The swapchain to present into (windowed runs). Headless runs have none.
    void set_surface(rhi::SurfaceId surface, std::uint32_t width, std::uint32_t height);
    /// Capture the next rendered frame's scene color to a PNG file.
    void request_screenshot(std::filesystem::path path);

    /// Extract phase: rebuilds RenderWorld by running every extractor in registration order.
    void extract(const World& world);
    /// Render phase: builds, compiles and submits the frame's render graph.
    void render();

    [[nodiscard]] const RenderWorld& render_world() const { return render_world_; }
    [[nodiscard]] rhi::Device& device() { return device_; }
    /// The last compiled graph, as text (RenderGraph::describe()).
    [[nodiscard]] const std::string& last_graph() const { return last_graph_; }

private:
    struct NamedExtractor {
        std::string name;
        Extractor fn;
    };

    Result<> ensure_targets(std::uint32_t width, std::uint32_t height);
    Result<> ensure_present_pipeline();
    void write_screenshot(std::uint32_t width, std::uint32_t height);

    rhi::Device& device_;
    RenderWorld render_world_;
    std::vector<NamedExtractor> extractors_;
    std::vector<std::unique_ptr<RenderFeature>> features_;
    std::vector<bool> prepared_;  ///< per feature: true = ready, false = disabled
    bool features_prepared_ = false;
    TransientPool pool_;

    rhi::TextureHandle scene_color_;
    rhi::ResourceState scene_color_state_ = rhi::ResourceState::undefined;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    rhi::SurfaceId surface_;
    std::uint32_t surface_width_ = 0;
    std::uint32_t surface_height_ = 0;
    rhi::PipelineHandle present_pipeline_;
    rhi::SamplerHandle present_sampler_;

    std::optional<std::filesystem::path> screenshot_;
    rhi::BufferHandle readback_;
    std::uint64_t readback_size_ = 0;

    std::optional<rhi::SubmissionId> in_flight_;
    std::string last_graph_;
};

/// Registers the Extract and Render phase systems. Requires a Renderer service.
struct RenderModule {
    explicit RenderModule(World& world);
};

}  // namespace veach::render
