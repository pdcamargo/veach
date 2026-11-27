# 0019: Veach modules and services

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0015).

## Context

Engine features, toolbox layers and the user's game need one extension mechanism. Flecs already has modules: a struct
whose constructor registers components, systems and observers into a world. Engine objects that are not ECS data (the
platform backend, asset server, script VM, RHI device, renderer) must still be reachable from module code, which only
receives `World&`. We are not building a Bevy-style plugin/app model.

## Decision

**Modules.**

- A Veach module is a Flecs module: a struct whose constructor takes `veach::World&` and starts with
  `world.module<Self>()`. `veach::Module` (`veach/ecs/module.hpp`) is the concept `std::constructible_from<T, World&>`.
- `engine.import<T>()` queues it; the `modules` init step imports queued modules in call order, after every engine
  subsystem exists ([0018](0018-engine-lifecycle-and-phases.md)).
- Engine features are modules too (`CoreModule`, `PlatformModule`, `AssetModule`, `render::RenderModule`), imported in
  their own init steps. A layer above the engine (the space toolbox, [0042](0042-space-toolbox-layering.md)) ships
  its features as modules the same way.
- Module entities hold the module instance as component data, so `check_reflection` skips entities tagged
  `flecs::Module`. Module structs stay stateless: state goes in components or services.

**Services.**

- `veach::Services` (`veach/ecs/services.hpp`) is a typed registry of non-owning pointers, keyed by a per-type static
  address, attached through the world context (`attach_services`) and looked up with `services(world).get<T>()`
  (asserts with a clear message if absent) or `find<T>()`.
- Init steps `provide<T>()` what they create and `revoke<T>()` it in their teardown: `Platform`, `AssetServer`,
  `ScriptRuntime`, `rhi::Device`, `render::Renderer`.
- Services are per world, not global; tests build worlds with their own `Services` and fakes.
- Guide: [extending](../guides/extending.md).

## Consequences

- The game is "just another module": no app builder, plugin trait or schedule configuration.
- Module order is explicit (call order), and modules can rely on every engine service existing.
- Rule: services hold engine machinery, components hold data. Overusing services for gameplay state bypasses the ECS.
- Teardown order is guaranteed by the init stack, so a service never outlives what it points to.

## Alternatives considered

- **Bevy-style plugins plus an App builder:** duplicates Flecs modules and invites a second scheduling model.
- **Engine services as singleton components:** would need reflection, can't be non-copyable cleanly, and would show up
  as data in tools.
- **Globals and singletons:** hidden coupling, and several engines per process (tests) become impossible.
