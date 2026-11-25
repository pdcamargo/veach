#include "lab_module.hpp"

#include "triangle_feature.hpp"

#include "lab_shaders.hpp"
#include "lab_shaders_materials.hpp"

#include <veach/render/scene_renderer.hpp>

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
