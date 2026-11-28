// Spike SP-13 (VCH-089), throwaway: see sp13_module.hpp.

#include "sp13/sp13_module.hpp"

#include <memory>

namespace veach::sp13 {
namespace {

struct ModuleState {
    std::unique_ptr<Sim> sim;
};

std::int32_t control(const World& world, std::size_t index) {
    return quantize_axis(static_cast<double>(axis_value(world, action_names[index])));
}

Sp13Craft publish(const Sim& sim) {
    const CraftView v = sim.craft();
    const std::uint64_t h = sim.hash();
    return {.tick = v.tick,
            .altitude = v.altitude,
            .ve = v.ve,
            .vn = v.vn,
            .vu = v.vu,
            .offset_e = v.offset_e,
            .offset_n = v.offset_n,
            .cap = v.cap,
            .frame = v.frame,
            .scene = v.scene,
            .hash_hi = static_cast<std::int64_t>(h >> 32),
            .hash_lo = static_cast<std::int64_t>(h & 0xFFFFFFFFULL)};
}

}  // namespace

std::array<std::int32_t, 6> control_values(const Controls& c) {
    return {c.lift, c.strafe_x, c.strafe_z, c.engine, c.walk, c.turn};
}

CraftView view_of(const Sp13Craft& c) {
    return {.tick = c.tick,
            .altitude = c.altitude,
            .ve = c.ve,
            .vn = c.vn,
            .vu = c.vu,
            .offset_e = c.offset_e,
            .offset_n = c.offset_n,
            .cap = c.cap,
            .frame = c.frame,
            .scene = c.scene};
}

std::uint64_t hash_of(const Sp13Craft& c) {
    return (static_cast<std::uint64_t>(c.hash_hi) << 32) | static_cast<std::uint64_t>(c.hash_lo);
}

Sp13Module::Sp13Module(World& world) {
    world.module<Sp13Module>();
    world.component<Sp13Run>()
        .member(&Sp13Run::scale, "scale")
        .member(&Sp13Run::craft, "craft")
        .member(&Sp13Run::walkers, "walkers")
        .add<Scriptable>();
    world.component<Sp13Craft>()
        .member(&Sp13Craft::tick, "tick")
        .member(&Sp13Craft::altitude, "altitude")
        .member(&Sp13Craft::ve, "ve")
        .member(&Sp13Craft::vn, "vn")
        .member(&Sp13Craft::vu, "vu")
        .member(&Sp13Craft::offset_e, "offset_e")
        .member(&Sp13Craft::offset_n, "offset_n")
        .member(&Sp13Craft::cap, "cap")
        .member(&Sp13Craft::frame, "frame")
        .member(&Sp13Craft::scene, "scene")
        .member(&Sp13Craft::hash_hi, "hash_hi")
        .member(&Sp13Craft::hash_lo, "hash_lo")
        .add<Scriptable>();
    for (std::size_t i = 0; i < action_names.size(); ++i) {
        bind_axis(world, action_names[i], {.source = action_axes[i]});
    }

    auto state = std::make_shared<ModuleState>();
    world.system<const Sp13Run>("Sp13Step").kind<phase::Simulation>().each([state](Entity e, const Sp13Run& run) {
        if (state->sim == nullptr) {
            state->sim = std::make_unique<Sim>(Config{.scale = run.scale, .craft = run.craft, .walkers = run.walkers});
        }
        const World world = e.world();
        const Controls c{.lift = control(world, 0),
                         .strafe_x = control(world, 1),
                         .strafe_z = control(world, 2),
                         .engine = control(world, 3),
                         .walk = control(world, 4),
                         .turn = control(world, 5)};
        state->sim->step(c);
        e.set<Sp13Craft>(publish(*state->sim));
    });
}

}  // namespace veach::sp13
