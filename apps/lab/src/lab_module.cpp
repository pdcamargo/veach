#include "lab_module.hpp"

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

    world.system<const Spinner>("ReportSpinner").kind<veach::phase::Shutdown>().each([](const Spinner& s) {
        veach::log::info("lab", "spinner turned {:.4f} rad", s.angle);
    });
}

}  // namespace lab
