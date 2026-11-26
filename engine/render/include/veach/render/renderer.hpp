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

#include <veach/asset/scene.hpp>
#include <veach/core/error.hpp>
#include <veach/core/math.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/render/camera.hpp>
#include <veach/render/png.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/scene.hpp>
#include <veach/rhi/rhi.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace veach::render {

/// One camera's view of the frame, extracted from a Camera and its Transform (docs/specs/render-views/spec.md).
/// Column-major matrices, reverse-Z, infinite far plane (ADR 0010).
struct RenderView {
    math::mat4 view{1.0f};        ///< view from world
    math::mat4 projection{1.0f};  ///< clip from view
    math::mat4 view_projection{1.0f};
    math::mat4 inverse_view_projection{1.0f};
    math::vec3 position{0.0f};  ///< world space
    float near_plane = 0.1f;
    std::uint32_t width = 0;  ///< the target it was built for, pixels
    std::uint32_t height = 0;
    float exposure = 1.0f / 1.2f;           ///< 1 / (1.2 * 2^ev100)
    std::optional<PathTracer> path_tracer;  ///< the camera's PathTracer component, if it has one
    RealtimeLighting realtime;              ///< the camera's RealtimeLighting component, or the defaults
    Upscaling upscaling;                    ///< the camera's Upscaling component, or the defaults
    Candela candela;                        ///< the camera's Candela component, or the defaults
    std::uint64_t camera = 0;               ///< the camera entity: per-view history is kept by it (ADR 0040)
};

/// A mesh to draw this frame: an imported mesh, the material of each of its slots, and where it is.
struct MeshDraw {
    AssetId mesh;
    std::vector<AssetId> materials;
    math::mat4 world{1.0f};
    std::uint64_t entity = 0;  ///< identifies the draw across frames: last frame's transform gives motion
};

/// A punctual light this frame (ADR 0034). `radiance` is color x intensity (lux or candela).
struct RenderLight {
    LightKind kind = LightKind::point;
    math::vec3 position{0.0f};
    math::vec3 direction{0.0f, 0.0f, -1.0f};
    math::vec3 radiance{0.0f};
    float cos_inner = 1.0f;
    float cos_outer = 0.0f;
    float size = 0.0f;  ///< sphere radius (point, spot), angular radius (directional); 0 = punctual (soft-shadows spec)
};

/// The frame's sky (ADR 0050), resolved by extraction from the Sky component (or its defaults) and the lights.
struct ResolvedSky {
    SkyMode mode = SkyMode::physical;
    Sky settings;
    math::vec3 sun_direction{0.0f, 1.0f, 0.0f};  ///< toward the sun
    float sun_illuminance_top = 128000.0f;       ///< lux at the top of the atmosphere
};

/// Renderer-owned snapshot of what to draw this frame. Rebuilt by extraction every frame.
class RenderWorld {
public:
    std::uint64_t frame = 0;
    std::uint32_t target_width = 0;  ///< pixels: the primary window's drawable, or its Window size headless
    std::uint32_t target_height = 0;
    std::uint32_t window_count = 0;
    std::vector<RenderView> views;  ///< active cameras, in entity-id order; the first is the primary view
    std::vector<MeshDraw> meshes;   ///< in entity-id order
    std::vector<RenderLight> lights;
    ResolvedSky sky;

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
    const RenderView* view = nullptr;  ///< the primary view, or null when no camera is active
};

/// The frame's target size: the primary window's, or 1280x720 when nothing says otherwise.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> target_size(const RenderWorld& world);

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
    /// Capture the next rendered frame's scene color and hand it to `done` (after the GPU finished it).
    using CaptureCallback = std::function<void(Result<Image>)>;
    void request_capture(CaptureCallback done);

    /// Extract phase: rebuilds RenderWorld by running every extractor in registration order.
    void extract(const World& world);
    /// Render phase: builds, compiles and submits the frame's render graph.
    void render();

    [[nodiscard]] const RenderWorld& render_world() const { return render_world_; }
    /// The first feature of type T, or null.
    template <class T>
    [[nodiscard]] T* feature() {
        for (const auto& f : features_) {
            if (auto* typed = dynamic_cast<T*>(f.get())) {
                return typed;
            }
        }
        return nullptr;
    }
    [[nodiscard]] rhi::Device& device() { return device_; }
    /// The last compiled graph, as text (RenderGraph::describe()).
    [[nodiscard]] const std::string& last_graph() const { return last_graph_; }
    /// The last frame's passes in execution order (culled ones left out): what the frame budget maps (ADR 0049).
    [[nodiscard]] const std::vector<std::string>& last_passes() const { return last_passes_; }

private:
    struct NamedExtractor {
        std::string name;
        Extractor fn;
    };

    Result<> ensure_targets(std::uint32_t width, std::uint32_t height);
    Result<> ensure_present_pipeline();
    void finish_capture(std::uint32_t width, std::uint32_t height);

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

    CaptureCallback capture_;
    rhi::BufferHandle readback_;
    std::uint64_t readback_size_ = 0;

    std::string last_graph_;
    std::vector<std::string> last_passes_;
};

/// Registers the Extract and Render phase systems. Requires a Renderer service.
struct RenderModule {
    explicit RenderModule(World& world);
};

}  // namespace veach::render
