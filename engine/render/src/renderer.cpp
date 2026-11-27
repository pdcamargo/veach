#include <veach/render/renderer.hpp>

#include <veach/asset/asset_server.hpp>
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
        e.set(Transform{node.translation, node.rotation, node.scale});
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

std::pair<std::uint32_t, std::uint32_t> target_size(const RenderWorld& world) {
    return {world.target_width != 0 ? world.target_width : 1280, world.target_height != 0 ? world.target_height : 720};
}

Renderer::Renderer(rhi::Device& device) : device_(device), pool_(device) {
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
    add_extractor("views", [](const World& world, RenderWorld& out) {
        const auto [width, height] = target_size(out);
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        std::vector<std::pair<std::uint64_t, RenderView>> views;
        world.each([&](Entity e, const Camera& camera) {
            if (!camera.active) {
                return;
            }
            // The camera's world pose without scale: a scaled camera would skew its view.
            math::mat4 world_from_camera(1.0f);
            if (const GlobalTransform* global = e.try_get<GlobalTransform>()) {
                world_from_camera = math::mat4_cast(global->rotation());
                world_from_camera[3] = math::vec4(global->translation(), 1.0f);
            }
            RenderView view;
            view.view = math::inverse(world_from_camera);
            view.projection = math::perspective_reverse_z(camera.fov_y, aspect, camera.near_plane);
            view.view_projection = view.projection * view.view;
            view.inverse_view_projection = math::inverse(view.view_projection);
            view.position = math::vec3(world_from_camera[3]);
            view.near_plane = camera.near_plane;
            view.width = width;
            view.height = height;
            view.exposure = 1.0f / (1.2f * std::exp2(ev100_of(camera)));
            if (const PhotoMode* photo = e.try_get<PhotoMode>()) {
                view.photo_mode = *photo;
            }
            view.camera = e.id();
            views.emplace_back(e.id(), view);
        });
        std::ranges::sort(views, {}, &std::pair<std::uint64_t, RenderView>::first);
        for (auto& [id, view] : views) {
            out.views.push_back(view);
        }
    });
    add_extractor("scene", [](const World& world, RenderWorld& out) {
        const Services* registry = find_services(world);
        const auto* assets = registry ? registry->find<AssetServer>() : nullptr;
        std::vector<std::pair<std::uint64_t, MeshDraw>> draws;
        world.each([&](Entity e, const MeshInstance& instance, const GlobalTransform& global) {
            const SceneAsset* scene = assets ? assets->get(Handle<SceneAsset>{instance.scene}) : nullptr;
            if (scene == nullptr || instance.node >= scene->nodes.size() || !scene->nodes[instance.node].mesh) {
                return;
            }
            const SceneNode& node = scene->nodes[instance.node];
            MeshDraw draw{.mesh = scene->meshes.at(*node.mesh), .world = global.matrix, .entity = e.id()};
            for (const std::uint32_t material : node.materials) {
                draw.materials.push_back(scene->materials.at(material));
            }
            draws.emplace_back(e.id(), std::move(draw));
        });
        std::ranges::sort(draws, {}, &std::pair<std::uint64_t, MeshDraw>::first);
        for (auto& [id, draw] : draws) {
            out.meshes.push_back(std::move(draw));
        }
        const auto light = [&](LightKind kind, const GlobalTransform& g, math::vec3 radiance) {
            RenderLight l{.kind = kind, .position = g.translation(), .radiance = radiance};
            l.direction = math::normalize(g.rotation() * math::world_forward);
            return l;
        };
        world.each([&](const DirectionalLight& d, const GlobalTransform& g) {
            RenderLight l = light(LightKind::directional, g, d.color * d.illuminance);
            l.size = 0.5f * d.angle;
            out.lights.push_back(l);
        });
        world.each([&](const PointLight& p, const GlobalTransform& g) {
            RenderLight l = light(LightKind::point, g, p.color * p.intensity);
            l.size = p.radius;
            out.lights.push_back(l);
        });
        world.each([&](const SpotLight& s, const GlobalTransform& g) {
            RenderLight l = light(LightKind::spot, g, s.color * s.intensity);
            l.cos_inner = std::cos(s.inner_angle);
            l.cos_outer = std::cos(s.outer_angle);
            l.size = s.radius;
            out.lights.push_back(l);
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

    RenderGraph graph(device_, &pool_);
    const GraphResource color = graph.import(scene_color_, "scene_color", scene_color_state_);
    graph.add_graphics_pass(
        "clear", [&](PassBuilder& b) { b.color_target(color, rhi::LoadOp::clear, background_color); },
        [](PassContext&) {});

    const FrameContext frame{device_, render_world_,
                             color,   width,
                             height,  render_world_.views.empty() ? nullptr : &render_world_.views.front()};
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
    device_.end_frame();
    scene_color_state_ = graph.final_state(color);

    if (capture) {
        device_.wait(submission);
        finish_capture(width, height);
    }
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
    world.component<PhotoMode>()
        .member(&PhotoMode::samples_per_frame, "samples_per_frame")
        .member(&PhotoMode::max_bounces, "max_bounces")
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
