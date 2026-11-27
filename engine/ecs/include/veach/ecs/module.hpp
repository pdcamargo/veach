#pragma once

// Veach modules (ADR 0019). A module is a Flecs module: a struct whose
// constructor takes World& and registers components, systems and observers.
// Engine features are modules; the game is a module. Add one with
// engine.import<MyModule>() before engine.run().
//
//   struct GameModule {
//       explicit GameModule(veach::World& world) {
//           world.module<GameModule>();
//           world.component<Spin>().member(&Spin::speed, "speed");
//           world.system<Spin>("Spin").kind<veach::phase::Simulation>().each(...);
//       }
//   };

#include <veach/ecs/flecs.hpp>

#include <concepts>

namespace veach {

template <class T>
concept Module = std::constructible_from<T, World&>;

}  // namespace veach
