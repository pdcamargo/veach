#pragma once

// Core engine components every world has. All are reflected (see core_module.cpp).

#include <veach/core/rng.hpp>
#include <veach/ecs/flecs.hpp>

#include <cstdint>

namespace veach {

/// Singleton. Simulation time; written only by the engine.
/// During Simulation/PostSimulation/TransformPropagation, `delta` is the fixed
/// step (1/60 s). During per-frame phases it is the frame's delta (equal to the
/// fixed step in headless runs).
struct Time {
    std::uint64_t frame = 0;       ///< Current frame number, starting at 1.
    std::uint64_t fixed_step = 0;  ///< Fixed steps completed before the current one.
    double delta = 0.0;            ///< Seconds covered by the current phase invocation.
    double elapsed = 0.0;          ///< Simulated seconds (fixed_step * fixed delta).
};

/// Singleton. Deterministic RNG seeded from the engine seed (--seed).
struct Random {
    std::uint64_t seed = 0;
    Rng rng{0};
};

/// Tag on a component type: expose it to Luau as `veach.components.<Name>` and
/// in the generated veach.d.luau. All of its fields must be Luau-representable
/// (boolean, number, string, vec3, enum, or nested structs of those).
///   world.component<Spin>().member(&Spin::speed, "speed").add<veach::Scriptable>();
struct Scriptable {};

/// The core ECS module: phases' companion components (Time, Random) and
/// reflection for std::string and veach::math types. Imported by the engine.
struct CoreModule {
    explicit CoreModule(World& world);
};

}  // namespace veach
