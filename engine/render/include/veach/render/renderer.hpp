#pragma once

// The renderer and the extraction boundary (ADR 0020).
//
// Renderer data (GPU resources, acceleration structures) lives outside the ECS.
// The Extract phase is the ONLY place ECS data crosses into the renderer:
// extractors read components and write RenderWorld. The Render phase builds a
// render graph (ADR 0029) from the registered features and submits it.
//
// Modules extend rendering without touching renderer internals:
//   auto& renderer = veach::services(world).get<veach::render::Renderer>();
//   renderer.add_extractor("spin", [](const veach::World& w, veach::render::RenderWorld& rw) {...});
//   renderer.add_feature(std::make_unique<MyFeature>());

#include <veach/asset/scene.hpp>
#include <veach/core/error.hpp>
#include <veach/core/math.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/ecs/transform.hpp>
#include <veach/render/atmosphere.hpp>
#include <veach/render/backdrop.hpp>
#include <veach/render/camera.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/render/png.hpp>
#include <veach/render/readback.hpp>
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

/// Spike SP-14 (VCH-090), throwaway (docs/research/spikes/sp-14-exposure-depth.md): histogram auto-exposure that stays
/// on the GPU, with pre-exposure (ADR 0063). A module's extractor sets RenderView::auto_exposure; without it the
/// camera's ev100 is used as before. EV values are EV100: luminance L (nits) meters to log2(L * 100 / 12.5).
struct AutoExposureSettings {
    float min_ev100 = -4.0f;  ///< the adapted exposure's range
    float max_ev100 = 18.0f;
    float histogram_min_ev = -8.0f;  ///< the histogram's range (128 bins); darker pixels are not metered
    float histogram_max_ev = 24.0f;
    float low_percent = 0.5f;  ///< the metered band of the sorted metered pixels: the darkest half is ignored
    float high_percent = 0.9f;
    float speed_to_bright = 3.0f;      ///< exponential adaptation toward a brighter scene (1/s)
    float speed_to_dark = 1.5f;        ///< and toward a darker one (1/s)
    float max_rate_to_bright = 15.0f;  ///< the most it moves per second (stops)
    float max_rate_to_dark = 6.0f;
    /// Stops added to the metered EV (positive = darker image). -1 shows the metered band near AgX's mid grey.
    float compensation = -1.0f;
    float highlight_percent = 0.99f;  ///< this percentile of the metered pixels is kept unclipped...
    float highlight_white = 16.0f;    ///< ...at or below this exposed luminance (AgX's white is 16.3)...
    float highlight_max_pull = 4.0f;  ///< ...darkening the metered exposure by at most this (stops)
    float delta_time = 1.0f / 60.0f;
    bool reset = false;      ///< snap to the metered exposure this frame
    bool clamp_fp16 = true;  ///< clamp pre-exposed radiance to the fp16 maximum (off only to show the overflow)
    bool report = false;     ///< log each frame's exposure and counters (a few frames late)
};

/// Where render space is this frame (ADR 0045, docs/specs/frames/spec.md). Everything in RenderWorld is relative to
/// `origin`, in the axes of `frame`: the primary camera's reference frame and its position there, rounded to a 64 m
/// grid (so static content keeps its matrices while the camera moves inside a cell, and a flat scene near the origin
/// renders exactly as it did in world space). The one absolute position here, for extractors that rebase their own
/// content (RenderSpaceMap); nothing on the GPU sees it.
struct RenderSpace {
    std::uint64_t frame = 0;  ///< the ReferenceFrame entity; 0 is the root world
    math::dvec3 origin{0.0};  ///< meters, in that frame
};

/// The render origin for a camera at `position` in its frame: the nearest point of the 64 m grid.
[[nodiscard]] math::dvec3 render_origin(const math::dvec3& position);

/// Puts poses from any frame into render space: their frame's pose in the render frame (composed in double through
/// the lowest common ancestor, once per frame and map), minus the origin, cast to float32. Build one per extraction.
class RenderSpaceMap {
public:
    RenderSpaceMap(const World& world, const RenderSpace& space);
    /// A point given in `frame`, in render space; nullopt if `frame` is not a live reference frame (or 0).
    [[nodiscard]] std::optional<math::vec3> point(std::uint64_t frame, const math::dvec3& p);
    /// A direction given in `frame`, in render space's axes.
    [[nodiscard]] std::optional<math::vec3> direction(std::uint64_t frame, const math::vec3& d);
    /// render space from local, for an entity's FrameTransform.
    [[nodiscard]] std::optional<math::mat4> matrix(const FrameTransform& pose);
    /// The rotation of `frame`'s axes in render space's (identity for the render frame itself).
    [[nodiscard]] std::optional<math::dmat3> rotation(std::uint64_t frame);

private:
    const FramePose* pose_of(std::uint64_t frame);
    World world_;
    RenderSpace space_;
    std::unordered_map<std::uint64_t, std::optional<FramePose>> poses_;
};

/// One camera's view of the frame, extracted from a Camera and its FrameTransform (docs/specs/render-views/spec.md).
/// Column-major matrices, reverse-Z, infinite far plane (ADR 0013), in render space (RenderSpace).
struct RenderView {
    math::mat4 view{1.0f};        ///< view from world
    math::mat4 projection{1.0f};  ///< clip from view
    math::mat4 view_projection{1.0f};
    math::mat4 inverse_view_projection{1.0f};
    math::vec3 position{0.0f};  ///< render space (relative to RenderWorld::space.origin)
    /// The body context (ADR 0045): the camera's height along `up` from its frame's origin (its y in a flat frame,
    /// its distance from the center in a radial one) and that up, in render space. Use these, not position.y.
    double height_in_frame = 0.0;
    math::vec3 up{0.0f, 1.0f, 0.0f};
    float near_plane = 0.1f;
    std::uint32_t width = 0;  ///< the target it was built for, pixels
    std::uint32_t height = 0;
    float exposure = 1.0f / 1.2f;         ///< 1 / (1.2 * 2^ev100)
    std::optional<PhotoMode> photo_mode;  ///< the camera's PhotoMode component, if it has one (ADR 0041)
    std::uint64_t camera = 0;             ///< the camera entity
    /// Spike SP-14 (VCH-090), throwaway: auto-exposure in the real-time renderer; `exposure` is then its first value.
    std::optional<AutoExposureSettings> auto_exposure;
};

/// A mesh to draw this frame: an imported or runtime mesh (MeshStore), the material of each of its slots, and where
/// it is.
struct MeshDraw {
    AssetId mesh;
    std::vector<AssetId> materials;
    math::mat4 world{1.0f};
    std::uint64_t entity = 0;  ///< the entity it was extracted from: identifies the draw across frames
};

/// A punctual light this frame (ADR 0036). `radiance` is color x intensity (lux or candela).
struct RenderLight {
    LightKind kind = LightKind::point;
    math::vec3 position{0.0f};
    math::vec3 direction{0.0f, 0.0f, -1.0f};
    math::vec3 radiance{0.0f};
    float cos_inner = 1.0f;
    float cos_outer = 0.0f;
    float size = 0.0f;  ///< sphere radius (point, spot), angular radius (directional); 0 = punctual (ADR 0036)
};

/// The frame's sky (ADR 0038), resolved by extraction from the Sky component (or its defaults) and the lights.
struct ResolvedSky {
    SkyMode mode = SkyMode::physical;
    Sky settings;
    math::vec3 sun_direction{0.0f, 1.0f, 0.0f};  ///< toward the sun
    float sun_illuminance_top = 128000.0f;       ///< lux at the top of the atmosphere
    /// Spike SP-10 (VCH-086), throwaway: a planet's sky, set by a module's extractor; replaces the Sky's atmosphere
    /// in the real-time renderer when `mode` is physical.
    std::optional<PlanetSky> planet;
    /// Spike SP-14 (VCH-090), throwaway: a starfield, in planet mode or with mode none.
    std::optional<Starfield> stars;
    /// The space backdrop (ADR 0090): drawn behind the sky when set and `backdrop_settings.visible`. From the
    /// SpaceBackdrop component's seed, or a module's extractor (which sets its orientation every frame).
    std::optional<Backdrop> backdrop;
    SpaceBackdrop backdrop_settings;  ///< the first SpaceBackdrop in entity order, or the defaults
};

/// Renderer-owned snapshot of what to draw this frame. Rebuilt by extraction every frame.
class RenderWorld {
public:
    std::uint64_t frame = 0;
    std::uint32_t target_width = 0;  ///< pixels: the primary window's drawable, or its Window size headless
    std::uint32_t target_height = 0;
    std::uint32_t window_count = 0;
    RenderSpace space;              ///< where render space is; every position below is relative to it
    std::vector<RenderView> views;  ///< active cameras, in entity-id order; the first is the primary view
    std::vector<MeshDraw> meshes;   ///< in entity-id order
    std::vector<RenderLight> lights;
    ResolvedSky sky;
    /// The MeshStore's meshes and materials this frame (runtime meshes in `meshes` name them by id).
    std::shared_ptr<const RuntimeAssets> runtime_assets;

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
    /// GPU-to-CPU readback that never stalls (VCH-073): results arrive readbacks->latency() frames later. Never null
    /// inside Renderer::render().
    ReadbackQueue* readbacks = nullptr;
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
    /// Meshes and materials made at run time (also the engine's MeshStore service).
    [[nodiscard]] MeshStore& meshes() { return meshes_; }
    /// Non-blocking readback of this frame's GPU buffers (also FrameContext::readbacks).
    [[nodiscard]] ReadbackQueue& readbacks() { return readbacks_; }
    /// The last compiled graph, as text (RenderGraph::describe()).
    [[nodiscard]] const std::string& last_graph() const { return last_graph_; }

private:
    struct NamedExtractor {
        std::string name;
        Extractor fn;
    };

    Result<> ensure_targets(std::uint32_t width, std::uint32_t height);
    Result<> ensure_present_pipeline();
    void finish_capture(std::uint32_t width, std::uint32_t height);

    rhi::Device& device_;
    MeshStore meshes_;
    RenderWorld render_world_;
    std::vector<NamedExtractor> extractors_;
    std::vector<std::unique_ptr<RenderFeature>> features_;
    std::vector<bool> prepared_;  ///< per feature: true = ready, false = disabled
    bool features_prepared_ = false;
    TransientPool pool_;
    ReadbackQueue readbacks_;

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
};

/// Registers the Extract and Render phase systems. Requires a Renderer service.
struct RenderModule {
    explicit RenderModule(World& world);
};

}  // namespace veach::render
