# 0015: Veach modules and services

Date: 2026-11-24

## Status

Accepted

## Context

Engine features and the user's game need one extension mechanism. Flecs already has modules: a struct whose constructor registers components, systems and observers into a world. Engine objects that are not ECS data (the platform backend, the asset server, the script VM, the RHI device, the renderer) must still be reachable from module code, which only receives `World&`. We are not building a Bevy-style plugin/app model.

## Decision

**Modules.**

- A Veach module is a Flecs module: a struct whose constructor takes `veach::World&` and starts with `world.module<Self>()`.
- `veach::Module` (`veach/ecs/module.hpp`) is the concept `std::constructible_from<T, World&>`.
- `engine.import<T>()` queues the module. The `modules` init step imports queued modules in call order, after every engine subsystem exists ([0014](0014-engine-lifecycle-and-phases.md)).
- Engine features are modules too: `CoreModule`, `PlatformModule`, `AssetModule` and `render::RenderModule`. They are imported inside their own init steps.
- Module entities hold the module instance as component data, so `check_reflection` skips entities tagged `flecs::Module`. Module structs should stay stateless; state goes in components or services.

**Services.**

- `veach::Services` (`veach/ecs/services.hpp`) is a typed registry of non-owning pointers, keyed by a per-type static address.
- It is attached to the world through the world context (`attach_services`) and looked up with `services(world).get<T>()`, which asserts with a clear message if the service is absent, or `find<T>()`.
- Init steps `provide<T>()` the service they create and `revoke<T>()` it in their teardown: `Platform`, `AssetServer`, `ScriptRuntime`, `rhi::Device` and `render::Renderer`.
- Services are per world, not global. Tests build worlds with their own `Services` and fakes (`tests/platform_tests.cpp`).
- See [guides/extending.md](../guides/extending.md).

## Consequences

- The game is "just another module". There is no app builder, no plugin trait and no schedule configuration.
- Module order is explicit: call order. Modules can rely on every engine service already existing.
- Services are a locator. Overusing them for gameplay state would bypass the ECS, so the rule is: services hold engine machinery, components hold data.
- Teardown order is guaranteed by the init stack, so a service never outlives what it points to.

## Alternatives considered

- **Bevy-style plugins plus an App builder.** Duplicates Flecs modules and invites a second scheduling model. Explicitly not wanted.
- **Engine services as singleton components.** They would need to be reflected, can't be non-copyable cleanly, and would show up as "data" in tools.
- **Globals and singletons.** Hidden coupling, and multiple engines per process (tests) become impossible.
