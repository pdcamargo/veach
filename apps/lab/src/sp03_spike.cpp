#include "sp03_spike.hpp"

#include <veach/render/renderer.hpp>

#include <cstdlib>
#include <string>

namespace lab {

namespace {

/// The environment variable `name`, or an empty string (portable: MSVC deprecates std::getenv).
std::string env(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    std::string result;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        result = value;
    }
    std::free(value);  // NOLINT(cppcoreguidelines-no-malloc)
    return result;
#else
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once at startup
    return value != nullptr ? value : "";
#endif
}

/// The double offset of the nearest ancestor-or-self with Sp03LargePosition, or zero.
veach::math::dvec3 anchor_of(veach::Entity e) {
    for (; e.is_valid(); e = e.parent()) {
        if (const auto* large = e.try_get<Sp03LargePosition>()) {
            return {large->x, large->y, large->z};
        }
    }
    return veach::math::dvec3(0.0);
}

/// A float32 world position (small, inside its anchor) made camera-relative: the subtraction is in double.
veach::math::vec3 relative(veach::math::vec3 local, veach::math::dvec3 anchor, veach::math::dvec3 camera) {
    return veach::math::vec3(veach::math::dvec3(local) + (anchor - camera));
}

void camera_relative(const veach::World& world, veach::render::RenderWorld& out) {
    namespace math = veach::math;
    using veach::render::RenderView;
    if (out.views.empty()) {
        return;
    }
    // MeshDraw holds one world matrix for every view, so only the primary view can be the render origin.
    RenderView& view = out.views.front();
    const veach::Entity camera(world, view.camera);
    if (!camera.has<Sp03LargePosition>()) {
        return;
    }
    const math::dvec3 origin = anchor_of(camera) + math::dvec3(view.position);

    // The camera at the render origin: rotation only.
    const math::mat4 world_from_camera = math::inverse(view.view);
    math::mat4 rotation_only = world_from_camera;
    rotation_only[3] = math::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    view.view = math::inverse(rotation_only);
    view.view_projection = view.projection * view.view;
    view.inverse_view_projection = math::inverse(view.view_projection);
    view.position = math::vec3(0.0f);

    for (veach::render::MeshDraw& draw : out.meshes) {
        const math::dvec3 anchor = anchor_of(veach::Entity(world, draw.entity));
        draw.world[3] = math::vec4(relative(math::vec3(draw.world[3]), anchor, origin), 1.0f);
    }

    // Lights carry no entity: walk them in the order the engine's scene extractor wrote them.
    std::size_t index = 0;
    const auto rebase = [&](veach::Entity e) {
        if (index < out.lights.size()) {
            out.lights[index].position = relative(out.lights[index].position, anchor_of(e), origin);
        }
        ++index;
    };
    world.each([&](veach::Entity e, const veach::DirectionalLight&, const veach::GlobalTransform&) { rebase(e); });
    world.each([&](veach::Entity e, const veach::PointLight&, const veach::GlobalTransform&) { rebase(e); });
    world.each([&](veach::Entity e, const veach::SpotLight&, const veach::GlobalTransform&) { rebase(e); });
    if (index != out.lights.size()) {
        veach::log::warn("sp03", "lights: rebased {} of {}; the rest stay absolute", index, out.lights.size());
    }
}

}  // namespace

void register_sp03_spike(veach::World& world) {
    world.component<Sp03LargePosition>()
        .member(&Sp03LargePosition::x, "x")
        .member(&Sp03LargePosition::y, "y")
        .member(&Sp03LargePosition::z, "z")
        .add<veach::Scriptable>();
    world.component<Sp03Ladder>()
        .member(&Sp03Ladder::distance, "distance")
        .member(&Sp03Ladder::large, "large")
        .member(&Sp03Ladder::photo, "photo")
        .add<veach::Scriptable>();

    if (const std::string distance = env("VEACH_SP03_DISTANCE"); !distance.empty()) {
        const Sp03Ladder ladder{.distance = std::strtod(distance.c_str(), nullptr),
                                .large = env("VEACH_SP03_MODE") == "double",
                                .photo = env("VEACH_SP03_PHOTO") == "1"};
        world.entity("sp03_ladder").set<Sp03Ladder>(ladder);
        veach::log::info("sp03", "ladder: {:.6g} m, {}, {}", ladder.distance, ladder.large ? "double" : "float",
                         ladder.photo ? "photo mode" : "real time");
    }

    auto& renderer = veach::services(world).get<veach::render::Renderer>();
    renderer.add_extractor("sp03_camera_relative", camera_relative);
}

}  // namespace lab
