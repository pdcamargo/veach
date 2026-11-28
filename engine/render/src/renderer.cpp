#include <veach/render/renderer.hpp>

#include <veach/asset/asset_server.hpp>
#include <veach/core/env.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/services.hpp>
#include <veach/platform/platform_module.hpp>
#include <veach/platform/window.hpp>
#include <veach/render/png.hpp>

#include "atmosphere.hpp"
#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <utility>

namespace veach::render {

namespace {

/// Mirrors `Params` in shaders/present.slang (ADR 0027 layout rules).
struct PresentParams {
    rhi::DescriptorHandle source;
    rhi::DescriptorHandle sampler;
};
static_assert(rhi::parameters_match<PresentParams>(veach_render_shaders::layout::present,
                                                   {VEACH_PARAM(PresentParams, source),
                                                    VEACH_PARAM(PresentParams, sampler)}));

/// SceneInstance: replaces the entity's children with the scene's nodes (ADR 0011).
void spawn_scene(Entity root, const SceneInstance& instance) {
    World world = root.world();
    std::vector<Entity> old;
    root.children([&](Entity child) { old.push_back(child); });
    for (Entity child : old) {
        child.destruct();
    }
    Services* registry = find_services(world);
    auto* assets = registry ? registry->find<AssetServer>() : nullptr;
    if (assets == nullptr || instance.path.empty()) {
        return;
    }
    const Handle<SceneAsset> handle = assets->load<SceneAsset>(instance.path);
    auto scene = assets->wait(handle);
    if (!scene) {
        log::error("render", Error{scene.error().code,
                                   std::format("SceneInstance on '{}': {}", root.name().c_str(), scene.error().message),
                                   scene.error().hint});
        return;
    }
    const std::function<void(std::uint32_t, Entity)> spawn = [&](std::uint32_t index, Entity parent) {
        const SceneNode& node = (*scene)->nodes.at(index);
        Entity e = world.entity().child_of(parent);
        // Names are unique per parent in Flecs; glTF repeats them freely.
        std::string name = node.name.empty() ? std::format("node {}", index) : node.name;
        for (int k = 2; parent.lookup(name.c_str()); ++k) {
            name = std::format("{} ({})", node.name.empty() ? std::format("node {}", index) : node.name, k);
        }
        e.set_name(name.c_str());
        e.set(Transform{math::dvec3(node.translation), node.rotation, node.scale});
        if (node.mesh) {
            e.set(MeshInstance{handle.id, index});
        }
        if (node.light) {
            const SceneLight& light = (*scene)->lights.at(*node.light);
            switch (light.kind) {
                case LightKind::directional: e.set(DirectionalLight{light.color, light.intensity}); break;
                case LightKind::point: e.set(PointLight{light.color, light.intensity}); break;
                case LightKind::spot:
                    e.set(SpotLight{light.color, light.intensity, light.inner_cone, light.outer_cone});
                    break;
            }
        }
        for (const std::uint32_t child : node.children) {
            spawn(child, e);
        }
    };
    for (const std::uint32_t root_node : (*scene)->roots) {
        spawn(root_node, root);
    }
}

float luminance_of(math::vec3 c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

/// The frame's sky (ADR 0038): the first Sky in entity order, or the defaults. A Sky that drives the sun replaces
/// the scene's directional lights with the atmosphere's sun; without one, the default sky follows the scene's first
/// directional light, scaled so its ground illuminance is that light's.
void resolve_sky(const World& world, RenderWorld& out) {
    std::optional<std::pair<std::uint64_t, Sky>> found;
    world.each([&](Entity e, const Sky& sky) {
        if (!found || e.id() < found->first) {
            found = std::pair{e.id(), sky};
        }
    });
    ResolvedSky& r = out.sky;
    // The space backdrop (ADR 0091): the creator's settings, and a backdrop of the component's own when it has a
    // seed, with the galactic center toward -Z and the north galactic pole +Y.
    std::optional<std::pair<std::uint64_t, SpaceBackdrop>> backdrop;
    world.each([&](Entity e, const SpaceBackdrop& b) {
        if (!backdrop || e.id() < backdrop->first) {
            backdrop = std::pair{e.id(), b};
        }
    });
    r.backdrop_settings = backdrop ? backdrop->second : SpaceBackdrop{};
    r.backdrop.reset();
    if (backdrop && backdrop->second.seed != 0) {
        GalaxyView view = galaxy_view_from_seed(backdrop->second.seed);
        view.north = math::dvec3(0.0, 1.0, 0.0);
        view.center = math::dvec3(0.0, 0.0, -1.0);
        r.backdrop = generate_backdrop(backdrop->second.seed, view);
    }
    r.settings = found ? found->second : Sky{};
    r.mode = r.settings.mode;
    r.sun_direction = sun_direction(r.settings);
    r.sun_illuminance_top = r.settings.sun_illuminance;
    if (r.mode != SkyMode::physical) {
        return;
    }
    const AtmosphereParams atmosphere = atmosphere_params(r.settings);
    if (found && r.settings.drives_sun) {
        std::erase_if(out.lights, [](const RenderLight& l) { return l.kind == LightKind::directional; });
        const math::vec3 transmittance = atmosphere_transmittance(atmosphere, 0.0f, r.sun_direction);
        if (math::length(transmittance) > 0.0f) {
            RenderLight sun{.kind = LightKind::directional,
                            .direction = -r.sun_direction,
                            .radiance = transmittance * r.settings.sun_illuminance};
            sun.size = 0.004625f;  // the sun's angular radius: 0.53 degrees across
            out.lights.push_back(sun);
        }
        return;
    }
    // The scene's sun is its brightest directional light (glTF scenes can carry disabled, zero-intensity ones).
    const RenderLight* light = nullptr;
    for (const RenderLight& l : out.lights) {
        if (l.kind == LightKind::directional &&
            (light == nullptr || luminance_of(l.radiance) > luminance_of(light->radiance))) {
            light = &l;
        }
    }
    if (light == nullptr) {
        return;
    }
    r.sun_direction = -light->direction;
    const float ground = luminance_of(atmosphere_transmittance(atmosphere, 0.0f, r.sun_direction));
    r.sun_illuminance_top = ground > 1e-4f ? luminance_of(light->radiance) / ground : 0.0f;
}

}  // namespace

math::dvec3 render_origin(const math::dvec3& position) {
    constexpr double cell = 64.0;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        return math::dvec3(0.0);
    }
    return math::dvec3(std::round(position.x / cell), std::round(position.y / cell), std::round(position.z / cell)) *
           cell;
}

RenderSpaceMap::RenderSpaceMap(const World& world, const RenderSpace& space) : world_(world.c_ptr()), space_(space) {}

const FramePose* RenderSpaceMap::pose_of(std::uint64_t frame) {
    auto it = poses_.find(frame);
    if (it == poses_.end()) {
        const Entity from = frame != 0 ? world_.entity(frame) : Entity{};
        const Entity to = space_.frame != 0 ? world_.entity(space_.frame) : Entity{};
        Result<FramePose> pose = frame_pose(world_, from, to);
        it = poses_.emplace(frame, pose ? std::optional<FramePose>(*pose) : std::nullopt).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::optional<math::vec3> RenderSpaceMap::point(std::uint64_t frame, const math::dvec3& p) {
    if (frame == space_.frame) {
        return math::vec3(p - space_.origin);  // the common case: no composition, one subtraction in double
    }
    const FramePose* pose = pose_of(frame);
    if (pose == nullptr) {
        return std::nullopt;
    }
    return math::vec3(pose->point(p) - space_.origin);
}

std::optional<math::vec3> RenderSpaceMap::direction(std::uint64_t frame, const math::vec3& d) {
    if (frame == space_.frame) {
        return d;
    }
    const FramePose* pose = pose_of(frame);
    if (pose == nullptr) {
        return std::nullopt;
    }
    return math::vec3(pose->rotation * math::dvec3(d));
}

std::optional<math::dmat3> RenderSpaceMap::rotation(std::uint64_t frame) {
    if (frame == space_.frame) {
        return math::dmat3(1.0);
    }
    const FramePose* pose = pose_of(frame);
    if (pose == nullptr) {
        return std::nullopt;
    }
    return pose->rotation;
}

std::optional<math::mat4> RenderSpaceMap::matrix(const FrameTransform& pose) {
    if (pose.frame == space_.frame) {
        math::mat4 m(pose.basis);
        m[3] = math::vec4(math::vec3(pose.translation - space_.origin), 1.0f);
        return m;
    }
    const FramePose* frame = pose_of(pose.frame);
    if (frame == nullptr) {
        return std::nullopt;
    }
    math::mat4 m(math::mat3(frame->rotation * math::dmat3(pose.basis)));
    m[3] = math::vec4(math::vec3(frame->point(pose.translation) - space_.origin), 1.0f);
    return m;
}

namespace {

/// The radical inverse of `index` in `base`: a coordinate of the Halton sequence.
float halton(std::uint32_t index, std::uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

}  // namespace

math::vec2 taa_jitter(std::uint64_t frame) {
    const auto index = static_cast<std::uint32_t>(frame % 8) + 1;
    return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

math::mat4 jittered_view_projection(const math::mat4& view_projection, math::vec2 jitter, std::uint32_t width,
                                    std::uint32_t height) {
    // A clip-space shear: x + ox * w and y + oy * w move every point by (ox, oy) in NDC, whose y points up.
    math::mat4 offset(1.0f);
    offset[3][0] = 2.0f * jitter.x / static_cast<float>(std::max(width, 1u));
    offset[3][1] = -2.0f * jitter.y / static_cast<float>(std::max(height, 1u));
    return offset * view_projection;
}

std::pair<std::uint32_t, std::uint32_t> target_size(const RenderWorld& world) {
    return {world.target_width != 0 ? world.target_width : 1280, world.target_height != 0 ? world.target_height : 720};
}

Renderer::Renderer(rhi::Device& device) : device_(device), pool_(device), readbacks_(device), uploads_(device) {
    if (const std::optional<std::string> spec = env("VEACH_GPU_TIMINGS")) {
        TimingReport report;
        char* end = nullptr;
        report.samples = static_cast<std::uint32_t>(std::strtoul(spec->c_str(), &end, 10));
        if (end != nullptr && *end == ',') {
            report.warmup = static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 10));
        }
        if (report.samples > 0) {
            timing_report_ = std::move(report);
        }
    }
    add_extractor("frame", [](const World& world, RenderWorld& out) {
        if (const Time* time = world.try_get<Time>()) {
            out.frame = time->frame;
        }
    });
    add_extractor("target", [](const World& world, RenderWorld& out) {
        world.each([&](Entity e, const Window& window) {
            ++out.window_count;
            if (!e.has<PrimaryWindow>()) {
                return;
            }
            const NativeWindowHandle handle = native_window_handle(world, e);
            out.target_width = handle.width != 0 ? handle.width : window.width;
            out.target_height = handle.height != 0 ? handle.height : window.height;
        });
    });
    add_extractor("views", [no_taa = env_is_one("VEACH_DISABLE_TAA"), no_bloom = env_is_one("VEACH_DISABLE_BLOOM")](
                               const World& world, RenderWorld& out) {
        const auto [width, height] = target_size(out);
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        std::vector<std::pair<std::uint64_t, const Camera*>> cameras;
        world.each([&](Entity e, const Camera& camera) {
            if (camera.active) {
                cameras.emplace_back(e.id(), &camera);
            }
        });
        std::ranges::sort(cameras, {}, &std::pair<std::uint64_t, const Camera*>::first);
        // One render origin per frame (ADR 0045, SP-03): the primary camera's position in its frame, on a 64 m grid.
        const FrameTransform identity;
        const auto pose_of = [&](std::uint64_t camera) {
            const FrameTransform* pose = world.entity(camera).try_get<FrameTransform>();
            return pose != nullptr ? pose : &identity;
        };
        if (!cameras.empty()) {
            const FrameTransform& primary = *pose_of(cameras.front().first);
            out.space = {.frame = primary.frame, .origin = render_origin(primary.translation)};
        }
        RenderSpaceMap space(world, out.space);
        for (const auto& [id, camera] : cameras) {
            const FrameTransform& pose = *pose_of(id);
            const std::optional<math::vec3> position = space.point(pose.frame, pose.translation);
            const std::optional<math::dmat3> axes = space.rotation(pose.frame);
            if (!position || !axes) {
                continue;  // in a frame render space can't reach (removed this tick)
            }
            // The camera's pose without scale: a scaled camera would skew its view.
            math::quat rotation = pose.rotation();
            if (pose.frame != out.space.frame) {
                rotation = math::quat(math::normalize(math::quat_cast(*axes) * math::dquat(rotation)));
            }
            math::mat4 world_from_camera = math::mat4_cast(rotation);
            world_from_camera[3] = math::vec4(*position, 1.0f);
            RenderView view;
            view.view = math::inverse(world_from_camera);
            view.projection = math::perspective_reverse_z(camera->fov_y, aspect, camera->near_plane);
            view.view_projection = view.projection * view.view;
            view.inverse_view_projection = math::inverse(view.view_projection);
            view.position = math::vec3(world_from_camera[3]);
            const Entity frame = pose.frame != 0 ? world.entity(pose.frame) : Entity{};
            const math::dvec3 up_here = up(world, frame, pose.translation);
            view.height_in_frame = math::dot(up_here, pose.translation);
            view.up = math::vec3(*axes * up_here);
            view.near_plane = camera->near_plane;
            view.width = width;
            view.height = height;
            view.exposure = 1.0f / (1.2f * std::exp2(ev100_of(*camera)));
            if (const PhotoMode* photo = world.entity(id).try_get<PhotoMode>()) {
                view.photo_mode = *photo;
            }
            if (const SkyVisibility* sky = world.entity(id).try_get<SkyVisibility>()) {
                view.sky_visibility = *sky;
            }
            if (const AmbientOcclusion* ao = world.entity(id).try_get<AmbientOcclusion>()) {
                view.ambient_occlusion = *ao;
            }
            if (const SunShadows* shadows = world.entity(id).try_get<SunShadows>()) {
                view.shadows = *shadows;
            }
            if (const Bloom* bloom = world.entity(id).try_get<Bloom>()) {
                view.bloom = *bloom;
            }
            if (no_bloom || view.photo_mode) {
                view.bloom.intensity = 0.0f;
            }
            const AntiAliasing* aa = world.entity(id).try_get<AntiAliasing>();
            view.temporal_aa = !no_taa && !view.photo_mode && (aa == nullptr || aa->temporal);
            view.jitter = view.temporal_aa ? taa_jitter(out.frame) : math::vec2(0.0f);
            view.camera = id;
            out.views.push_back(view);
        }
    });
    add_extractor("scene", [this](const World& world, RenderWorld& out) {
        RenderSpaceMap space(world, out.space);
        const Services* registry = find_services(world);
        const auto* assets = registry ? registry->find<AssetServer>() : nullptr;
        std::vector<std::pair<std::uint64_t, MeshDraw>> draws;
        world.each([&](Entity e, const MeshInstance& instance, const FrameTransform& pose) {
            const SceneAsset* scene = assets ? assets->get(Handle<SceneAsset>{instance.scene}) : nullptr;
            if (scene == nullptr || instance.node >= scene->nodes.size() || !scene->nodes[instance.node].mesh) {
                return;
            }
            const std::optional<math::mat4> matrix = space.matrix(pose);
            if (!matrix) {
                return;
            }
            const SceneNode& node = scene->nodes[instance.node];
            MeshDraw draw{.mesh = scene->meshes.at(*node.mesh), .world = *matrix, .entity = e.id()};
            for (const std::uint32_t material : node.materials) {
                draw.materials.push_back(scene->materials.at(material));
            }
            draws.emplace_back(e.id(), std::move(draw));
        });
        // Runtime meshes (MeshStore): the snapshot is the same pointer until the store changes.
        out.runtime_assets = meshes_.snapshot();
        world.each([&](Entity e, const RuntimeMeshInstance& instance, const FrameTransform& pose) {
            const auto it = out.runtime_assets->meshes.find(instance.mesh);
            const std::optional<math::mat4> matrix = space.matrix(pose);
            if (it == out.runtime_assets->meshes.end() || !matrix) {
                return;  // removed (an evicted terrain chunk): nothing to draw
            }
            draws.emplace_back(
                e.id(),
                MeshDraw{
                    .mesh = instance.mesh, .materials = it->second->materials, .world = *matrix, .entity = e.id()});
        });
        std::ranges::sort(draws, {}, &std::pair<std::uint64_t, MeshDraw>::first);
        for (auto& [id, draw] : draws) {
            out.meshes.push_back(std::move(draw));
        }
        const auto light = [&](LightKind kind, const FrameTransform& g,
                               math::vec3 radiance) -> std::optional<RenderLight> {
            const std::optional<math::vec3> position = space.point(g.frame, g.translation);
            const std::optional<math::vec3> direction = space.direction(g.frame, g.rotation() * math::world_forward);
            if (!position || !direction) {
                return std::nullopt;
            }
            return RenderLight{
                .kind = kind, .position = *position, .direction = math::normalize(*direction), .radiance = radiance};
        };
        world.each([&](const DirectionalLight& d, const FrameTransform& g) {
            if (std::optional<RenderLight> l = light(LightKind::directional, g, d.color * d.illuminance)) {
                l->size = 0.5f * d.angle;
                out.lights.push_back(*l);
            }
        });
        world.each([&](const PointLight& p, const FrameTransform& g) {
            if (std::optional<RenderLight> l = light(LightKind::point, g, p.color * p.intensity)) {
                l->size = p.radius;
                out.lights.push_back(*l);
            }
        });
        world.each([&](const SpotLight& s, const FrameTransform& g) {
            std::optional<RenderLight> l = light(LightKind::spot, g, s.color * s.intensity);
            if (!l) {
                return;
            }
            l->cos_inner = std::cos(s.inner_angle);
            l->cos_outer = std::cos(s.outer_angle);
            l->size = s.radius;
            out.lights.push_back(*l);
        });
        resolve_sky(world, out);
    });
}

Renderer::~Renderer() {
    device_.wait_idle();
    if (scene_color_.is_valid()) {
        device_.destroy(scene_color_);
    }
    if (readback_.is_valid()) {
        device_.destroy(readback_);
    }
    if (present_pipeline_.is_valid()) {
        device_.destroy(present_pipeline_);
    }
    if (present_sampler_.is_valid()) {
        device_.destroy(present_sampler_);
    }
}

void Renderer::add_extractor(std::string name, Extractor extractor) {
    extractors_.push_back({std::move(name), std::move(extractor)});
}

void Renderer::add_feature(std::unique_ptr<RenderFeature> feature) {
    log::debug("render", "feature '{}' added", feature->name());
    features_.push_back(std::move(feature));
    features_prepared_ = false;
}

void Renderer::set_surface(rhi::SurfaceId surface, std::uint32_t width, std::uint32_t height) {
    surface_ = surface;
    surface_width_ = width;
    surface_height_ = height;
}

void Renderer::request_screenshot(std::filesystem::path path) {
    request_capture([path = std::move(path)](Result<Image> image) {
        if (!image) {
            log::error("render", image.error());
            return;
        }
        if (auto written = write_png(path, image->width, image->height, image->rgba); !written) {
            log::error("render",
                       Error{ErrorCode::render_screenshot_failed, written.error().message, written.error().hint});
            return;
        }
        log::info("render", "screenshot written to {} ({}x{})", path.generic_string(), image->width, image->height);
    });
}

void Renderer::request_capture(CaptureCallback done) {
    capture_ = std::move(done);
}

void Renderer::extract(const World& world) {
    VEACH_ZONE("Renderer::extract");
    render_world_ = {};
    for (const NamedExtractor& extractor : extractors_) {
        VEACH_ZONE_TEXT("extractor", extractor.name.data(), extractor.name.size());
        extractor.fn(world, render_world_);
    }
}

Result<> Renderer::ensure_targets(std::uint32_t width, std::uint32_t height) {
    if (scene_color_.is_valid() && width == width_ && height == height_) {
        return {};
    }
    if (scene_color_.is_valid()) {
        device_.destroy(scene_color_);
    }
    auto texture = device_.create_texture({width, height, scene_color_format,
                                           rhi::TextureUsage::color_target | rhi::TextureUsage::storage |
                                               rhi::TextureUsage::sampled | rhi::TextureUsage::copy_src,
                                           "scene_color"});
    if (!texture) {
        return std::unexpected(texture.error());
    }
    scene_color_ = *texture;
    scene_color_state_ = rhi::ResourceState::undefined;
    width_ = width;
    height_ = height;
    return {};
}

Result<> Renderer::ensure_present_pipeline() {
    if (present_pipeline_.is_valid()) {
        return {};
    }
    rhi::GraphicsPipelineDesc desc;
    desc.library = &veach_render_shaders::present();
    desc.vertex_entry = "vs_main";
    desc.fragment_entry = "fs_main";
    desc.color_formats = {rhi::surface_format};
    desc.debug_name = "present";
    auto pipeline = device_.create_graphics_pipeline(desc);
    if (!pipeline) {
        return std::unexpected(pipeline.error());
    }
    present_pipeline_ = *pipeline;
    auto sampler = device_.create_sampler({rhi::Filter::linear, rhi::AddressMode::clamp});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    present_sampler_ = *sampler;
    return {};
}

void Renderer::render() {
    VEACH_ZONE("Renderer::render");
    const auto [width, height] = target_size(render_world_);

    if (!features_prepared_) {
        prepared_.assign(features_.size(), false);
        for (std::size_t i = 0; i < features_.size(); ++i) {
            if (auto ready = features_[i]->prepare(device_); ready) {
                prepared_[i] = true;
            } else {
                log::error("render",
                           Error{ready.error().code,
                                 std::format("feature '{}' disabled: {}", features_[i]->name(), ready.error().message),
                                 ready.error().hint});
            }
        }
        features_prepared_ = true;
    }
    if (auto targets = ensure_targets(width, height); !targets) {
        log::error("render", targets.error());
        return;
    }

    // Frames in flight (ADR 0030): blocks only if the GPU is frames_in_flight frames behind.
    device_.begin_frame();
    readbacks_.deliver();  // frame N - frames_in_flight's readbacks: begin_frame has waited for that frame

    RenderGraph graph(device_, &pool_);
    const GraphResource color = graph.import(scene_color_, "scene_color", scene_color_state_);
    graph.add_graphics_pass(
        "clear", [&](PassBuilder& b) { b.color_target(color, rhi::LoadOp::clear, background_color); },
        [](PassContext&) {});

    FrameBlackboard blackboard;
    const FrameContext frame{
        device_,     render_world_, color,
        width,       height,        render_world_.views.empty() ? nullptr : &render_world_.views.front(),
        &readbacks_, &uploads_,     &blackboard};
    for (std::size_t i = 0; i < features_.size(); ++i) {
        if (prepared_[i]) {
            features_[i]->record(graph, frame);
        }
    }

    rhi::SurfaceId present;
    if (surface_.is_valid()) {
        if (width != surface_width_ || height != surface_height_) {
            device_.resize_surface(surface_, width, height);
            surface_width_ = width;
            surface_height_ = height;
        }
        auto ready = ensure_present_pipeline();
        auto image =
            ready ? device_.acquire_image(surface_) : Result<rhi::TextureHandle>(std::unexpected(ready.error()));
        if (image) {
            present = surface_;
            const GraphResource swapchain =
                graph.import(*image, "swapchain", rhi::ResourceState::present, rhi::ResourceState::present);
            graph.add_graphics_pass(
                "present",
                [&](PassBuilder& b) {
                    b.read(color, rhi::ResourceState::shader_read);
                    b.color_target(swapchain, rhi::LoadOp::discard);
                },
                [this, color](PassContext& ctx) {
                    ctx.cmd.set_pipeline(present_pipeline_);
                    ctx.cmd.set_parameters(PresentParams{ctx.device.descriptor(ctx.texture(color)),
                                                         ctx.device.descriptor(present_sampler_)});
                    ctx.cmd.draw(3);
                });
        } else {
            log::warn("render", "skipping present: {}", image.error().message);
        }
    }

    const bool capture = static_cast<bool>(capture_);
    if (capture) {
        const std::uint64_t size = std::uint64_t{width} * height * 4;
        if (!readback_.is_valid() || readback_size_ < size) {
            if (readback_.is_valid()) {
                device_.destroy(readback_);
            }
            auto buffer =
                device_.create_buffer({size, rhi::BufferUsage::copy_dst, rhi::MemoryType::readback, "screenshot"});
            if (!buffer) {
                std::exchange(capture_, {})(std::unexpected(buffer.error()));
                readbacks_.abandon();
                return;
            }
            readback_ = *buffer;
            readback_size_ = size;
        }
        const GraphResource readback = graph.import(readback_, "screenshot", rhi::ResourceState::undefined);
        graph.add_transfer_pass(
            "screenshot",
            [&](PassBuilder& b) {
                b.read(color, rhi::ResourceState::copy_src);
                b.write(readback, rhi::ResourceState::copy_dst);
            },
            [color, readback](PassContext& ctx) {
                ctx.cmd.copy_texture_to_buffer(ctx.texture(color), ctx.buffer(readback));
            });
    }

    if (auto compiled = graph.compile(); !compiled) {
        log::error("render", compiled.error());
        readbacks_.abandon();
        return;
    }
    std::string described = graph.describe();
    if (described != last_graph_) {
        log::debug("render", "{}", described);
        last_graph_ = std::move(described);
    }

    auto cmd = device_.begin_commands(std::format("frame {}", render_world_.frame));
    graph.execute(*cmd);
    const rhi::SubmissionId submission = device_.submit(std::move(cmd), present);
    readbacks_.submitted(submission);
    device_.end_frame();
    collect_timings();
    scene_color_state_ = graph.final_state(color);

    if (capture) {
        device_.wait(submission);
        finish_capture(width, height);
    }
}

void Renderer::collect_timings() {
    if (!timing_report_ || timing_report_->samples == 0) {
        return;
    }
    TimingReport& r = *timing_report_;
    const rhi::FrameTimings t = device_.last_frame_timings();
    if (t.frame == 0 || t.frame == r.last_frame || t.frame <= r.warmup) {
        return;
    }
    r.last_frame = t.frame;
    std::map<std::string, double> frame;
    double total = 0.0;
    for (const rhi::PassTiming& p : t.passes) {
        frame[p.name] += p.gpu_ms;
        total += p.gpu_ms;
    }
    for (const auto& [name, ms] : frame) {
        r.passes[name].push_back(ms);
    }
    r.totals.push_back(total);
    r.frames.push_back(device_.last_frame_gpu_ms());
    if (r.totals.size() < r.samples) {
        return;
    }
    const auto median = [](std::vector<double> v) {
        std::ranges::sort(v);
        return v.size() % 2 == 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    };
    std::string passes;
    for (const auto& [name, values] : r.passes) {
        std::vector<double> padded = values;
        padded.resize(r.samples, 0.0);  // a pass missing from some frames counts 0 there
        passes += std::format("{}\"{}\": {:.4f}", passes.empty() ? "" : ", ", name, median(padded));
    }
    // total_ms sums the passes; frame_ms is the command buffer's span, where passes that overlap count once.
    log::info("render",
              "gpu_timings {{\"frames\": {}, \"total_ms\": {:.4f}, \"frame_ms\": {:.4f}, \"passes\": {{{}}}}}",
              r.samples, median(r.totals), median(r.frames), passes);
    r.samples = 0;  // once
}

void Renderer::finish_capture(std::uint32_t width, std::uint32_t height) {
    CaptureCallback done = std::exchange(capture_, {});
    if (!device_.capabilities().gpu) {
        done(make_error(ErrorCode::render_screenshot_failed, "the null RHI draws nothing to capture",
                        "Pass --rhi metal (macOS) to render on the GPU."));
        return;
    }
    const auto* pixels = static_cast<const std::uint8_t*>(device_.map(readback_));
    if (!pixels) {
        done(make_error(ErrorCode::render_screenshot_failed, "the readback buffer is not mappable"));
        return;
    }
    Image image{width, height, {}};
    image.rgba.assign(pixels, pixels + std::size_t{width} * height * 4);
    done(std::move(image));
}

RenderModule::RenderModule(World& world) {
    world.module<RenderModule>("veach::render");
    world.component<Camera>()
        .member(&Camera::fov_y, "fov_y")
        .member(&Camera::near_plane, "near_plane")
        .member(&Camera::active, "active")
        .member(&Camera::ev100, "ev100")
        .member(&Camera::aperture, "aperture")
        .member(&Camera::shutter, "shutter")
        .member(&Camera::iso, "iso")
        .add<Scriptable>();
    world.component<AssetId>().member(&AssetId::hi, "hi").member(&AssetId::lo, "lo");
    world.component<SceneInstance>().member(&SceneInstance::path, "path").add<Scriptable>();
    world.component<MeshInstance>().member(&MeshInstance::scene, "scene").member(&MeshInstance::node, "node");
    world.component<RuntimeMeshInstance>().member(&RuntimeMeshInstance::mesh, "mesh");
    world.component<DirectionalLight>()
        .member(&DirectionalLight::color, "color")
        .member(&DirectionalLight::illuminance, "illuminance")
        .member(&DirectionalLight::angle, "angle")
        .add<Scriptable>();
    world.component<PointLight>()
        .member(&PointLight::color, "color")
        .member(&PointLight::intensity, "intensity")
        .member(&PointLight::radius, "radius")
        .add<Scriptable>();
    world.component<SpotLight>()
        .member(&SpotLight::color, "color")
        .member(&SpotLight::intensity, "intensity")
        .member(&SpotLight::inner_angle, "inner_angle")
        .member(&SpotLight::outer_angle, "outer_angle")
        .member(&SpotLight::radius, "radius")
        .add<Scriptable>();
    world.component<SkyMode>();
    world.component<Sky>()
        .member(&Sky::mode, "mode")
        .member(&Sky::color, "color")
        .member(&Sky::drives_sun, "drives_sun")
        .member(&Sky::time_of_day, "time_of_day")
        .member(&Sky::latitude, "latitude")
        .member(&Sky::day_of_year, "day_of_year")
        .member(&Sky::north, "north")
        .member(&Sky::sun_illuminance, "sun_illuminance")
        .member(&Sky::ground_albedo, "ground_albedo")
        .member(&Sky::rayleigh, "rayleigh")
        .member(&Sky::mie, "mie")
        .member(&Sky::ozone, "ozone")
        .member(&Sky::mie_g, "mie_g")
        .member(&Sky::aerial_perspective, "aerial_perspective")
        .add<Scriptable>();
    world.component<SpaceBackdrop>()
        .member(&SpaceBackdrop::seed, "seed")
        .member(&SpaceBackdrop::visible, "visible")
        .member(&SpaceBackdrop::exposure_compensation, "exposure_compensation")
        .member(&SpaceBackdrop::exposure_follow, "exposure_follow")
        .add<Scriptable>();
    world.component<PhotoMode>()
        .member(&PhotoMode::samples_per_frame, "samples_per_frame")
        .member(&PhotoMode::max_bounces, "max_bounces")
        .add<Scriptable>();
    world.component<AntiAliasing>().member(&AntiAliasing::temporal, "temporal").add<Scriptable>();
    world.component<SkyVisibility>()
        .member(&SkyVisibility::visibility, "visibility")
        .member(&SkyVisibility::reach, "reach")
        .add<Scriptable>();
    world.component<AmbientOcclusion>()
        .member(&AmbientOcclusion::enabled, "enabled")
        .member(&AmbientOcclusion::radius, "radius")
        .member(&AmbientOcclusion::intensity, "intensity")
        .member(&AmbientOcclusion::sky_distance, "sky_distance")
        .add<Scriptable>();
    world.component<Bloom>().member(&Bloom::intensity, "intensity").member(&Bloom::radius, "radius").add<Scriptable>();
    world.component<SunShadows>()
        .member(&SunShadows::suns, "suns")
        .member(&SunShadows::distance, "distance")
        .member(&SunShadows::cascades, "cascades")
        .member(&SunShadows::resolution, "resolution")
        .add<Scriptable>();
    world.observer<const SceneInstance>("SpawnSceneInstances")
        .event(flecs::OnSet)
        .each([](Entity e, const SceneInstance& instance) { spawn_scene(e, instance); });
    world.system("Extract").kind<phase::Extract>().run([](Iter& it) {
        const World w = it.world();
        services(w).get<Renderer>().extract(w);
    });
    world.system("Render").kind<phase::Render>().run([](Iter& it) { services(it.world()).get<Renderer>().render(); });
}

}  // namespace veach::render

namespace veach {

float ev100_of(const Camera& camera) {
    if (camera.ev100 != 0.0f || camera.shutter <= 0.0f || camera.iso <= 0.0f) {
        return camera.ev100;
    }
    return std::log2(camera.aperture * camera.aperture / camera.shutter * 100.0f / camera.iso);
}

}  // namespace veach
