#include "lab_module.hpp"

#include "triangle_feature.hpp"

#include "lab_shaders.hpp"
#include "lab_shaders_materials.hpp"

#include <veach/render/scene_renderer.hpp>

#include <algorithm>
#include <cmath>

namespace lab {

LabModule::LabModule(veach::World& world) {
    world.module<LabModule>();

    world.component<Spinner>()
        .member(&Spinner::radians_per_second, "radians_per_second")
        .member(&Spinner::angle, "angle")
        .add<veach::Scriptable>();

    // immediate(): creating a *named* entity needs a writable (non-staged) world.
    world.system("SpawnSpinner").kind<veach::phase::Startup>().immediate().run([](veach::Iter& it) {
        it.world().entity("spinner").set<Spinner>({});
    });

    // Simulation runs once per fixed step, so the angle is deterministic: after
    // N steps it is exactly N * (1/60) * radians_per_second.
    world.system<Spinner>("Spin").kind<veach::phase::Simulation>().each(
        [](veach::Iter& it, std::size_t, Spinner& s) { s.angle += s.radians_per_second * it.delta_time(); });

    world.component<FreeCamera>()
        .member(&FreeCamera::speed, "speed")
        .member(&FreeCamera::turn_speed, "turn_speed")
        .add<veach::Scriptable>();

    world.system<const FreeCamera, veach::Transform>("FlyFreeCamera")
        .kind<veach::phase::Simulation>()
        .each([](veach::Iter& it, std::size_t, const FreeCamera& fly, veach::Transform& t) {
            const veach::World world = it.world();
            const auto axis = [&](std::string_view negative, std::string_view positive) {
                return (veach::action_down(world, positive) ? 1.0f : 0.0f) -
                       (veach::action_down(world, negative) ? 1.0f : 0.0f);
            };
            const float turn = axis("turn_left", "turn_right");
            const float look = axis("look_down", "look_up");
            // In camera space: +X right, +Y up, -Z forward.
            const veach::math::vec3 move{axis("move_left", "move_right"), axis("move_down", "move_up"),
                                         -axis("move_back", "move_forward")};
            if (turn == 0.0f && look == 0.0f && move == veach::math::vec3(0.0f)) {
                return;
            }
            // Yaw and pitch from where the camera looks now (any roll is dropped once it moves).
            const float dt = it.delta_time();
            const veach::math::vec3 forward = t.rotation * veach::math::world_forward;
            const float yaw = std::atan2(-forward.x, -forward.z) - turn * fly.turn_speed * dt;
            const float pitch =
                std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)) + look * fly.turn_speed * dt, -1.5f, 1.5f);
            t.rotation = veach::math::angleAxis(yaw, veach::math::world_up) *
                         veach::math::angleAxis(pitch, veach::math::world_right);
            const float speed = fly.speed * (veach::action_down(world, "sprint") ? 4.0f : 1.0f);
            t.translation += t.rotation * move * (speed * dt);
        });

    register_triangle_demo(world);

    // The reference path tracer, with the lab's materials (scenes/*.luau put a PathTracer on their camera).
    auto& renderer = veach::services(world).get<veach::render::Renderer>();
    renderer.add_feature(std::make_unique<veach::render::SceneRenderer>(
        veach::render::SceneShaders{lab_shaders::path_tracer(), lab_shaders::realtime_visibility(),
                                    lab_shaders::realtime_shade()},
        lab_shaders::material_kinds, veach::services(world).get<veach::AssetServer>()));

    world.system<const Spinner>("ReportSpinner").kind<veach::phase::Shutdown>().each([](const Spinner& s) {
        veach::log::info("lab", "spinner turned {:.4f} rad", s.angle);
    });
}

}  // namespace lab
