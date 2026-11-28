#include "lab_module.hpp"

#include "runtime_mesh_demo.hpp"
#include "sp04_spike.hpp"
#include "sp09_spike.hpp"
#include "sp10_spike.hpp"
#ifdef VEACH_LAB_SP13
#include "sp13_spike.hpp"
#endif
#include "sp14_spike.hpp"
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
    register_runtime_mesh_demo(world);  // VCH-102: a terrain-like patch meshed at run time

    // glTF scenes, with the lab's materials: in real time, or in photo mode (scripts put a PhotoMode on the camera).
    auto& renderer = veach::services(world).get<veach::render::Renderer>();
    renderer.add_feature(std::make_unique<veach::render::SceneRenderer>(
        veach::render::SceneShaders{lab_shaders::path_tracer(), lab_shaders::realtime_visibility(),
                                    lab_shaders::realtime_shade()},
        lab_shaders::material_kinds, veach::services(world).get<veach::AssetServer>()));

    register_sp04_spike(world);  // spike SP-04 (VCH-080), throwaway: the sun from the system
    register_sp09_spike(world);  // spike SP-09 (VCH-085), throwaway: planet octree and governed descent
    register_sp10_spike(world);  // spike SP-10 (VCH-086), throwaway: a planet's atmosphere from ground to orbit
#ifdef VEACH_LAB_SP13
    register_sp13_spike(world);  // spike SP-13 (VCH-089), throwaway: walk, hover and land, drawn
#endif
    register_sp14_spike(world);  // spike SP-14 (VCH-090), throwaway: exposure and depth, cockpit to deep space

    world.system<const Spinner>("ReportSpinner").kind<veach::phase::Shutdown>().each([](const Spinner& s) {
        veach::log::info("lab", "spinner turned {:.4f} rad", s.angle);
    });
}

}  // namespace lab
