# ECS and phases

**Status:** Implemented
**Area:** ecs
**Owner:** dev lead

## Summary

Flecs v4 is the ECS, exposed through `veach::` aliases. Systems attach to fixed engine phases. Every component is
reflected. World writes from scripts and tools go through a transactional `CommandBuffer`. Features are added as
Veach modules.

## Motivation

One data model for engine, game, scripts, tools and agents. If a component is reflected, everything can see it:
JSON files, schemas, Luau, the type generator, the Flecs explorer and, later, MCP. Fixed phases are what let the
engine own the loop and keep simulation deterministic.

## Requirements

- MUST expose Flecs through `#include <veach/veach.hpp>` as aliases (`veach::World`, `Entity`, `Query`, `Iter`,
  `System`, ...), with raw Flecs reachable as `veach::ecs::`. This is a barrel, not a wrapper.
- MUST NOT contain `flecs::` anywhere under `apps/`.
- MUST provide the phases Startup, Input, Simulation, PostSimulation, TransformPropagation, PreExtract, Extract,
  Render and Shutdown, run only by the engine, with systems in a phase running in declaration order.
- MUST refuse to start when a component with data has no reflection (`ecs.unreflected_component`, listing them).
- MUST provide a `CommandBuffer` with spawn/despawn/set/remove that applies in recording order or discards
  completely (including entities it reserved). Destroying it discards.
- MUST load JSON data files into reflected types, with did-you-mean errors for unknown fields and enum values,
  and generate JSON Schemas from the same reflection.
- MUST let features be modules (`struct M { explicit M(veach::World&); }`), imported with `engine.import<M>()`.
- SHOULD reflect `std::string` and the `veach::math` vector types out of the box.

## API sketch

```cpp
struct Spinner { float radians_per_second = 1.0f; float angle = 0.0f; };

struct GameModule {
    explicit GameModule(veach::World& world) {
        world.module<GameModule>();
        world.component<Spinner>()
            .member(&Spinner::radians_per_second, "radians_per_second")
            .member(&Spinner::angle, "angle")
            .add<veach::Scriptable>();                    // also visible to Luau
        world.system<Spinner>("Spin").kind<veach::phase::Simulation>().each(
            [](veach::Iter& it, std::size_t, Spinner& s) { s.angle += s.radians_per_second * it.delta_time(); });
    }
};

veach::CommandBuffer cmd(world);
auto e = cmd.spawn("player");
cmd.set(e, Health{100});
cmd.apply();   // or cmd.discard()
```

## Acceptance criteria

- **ecs.AC1** — No `flecs::` under `apps/` (CTest `repo: hygiene ...` and the CI lint job).
- **ecs.AC2** — Modules passed to `engine.import<T>()` are imported in the `modules` init step, after every engine
  subsystem exists (covered by lifecycle.AC1 and the lab app).
- **ecs.AC3** — `CommandBuffer::apply` applies spawn, set (with name), remove and despawn in order.
- **ecs.AC4** — `CommandBuffer::discard`, or destruction without apply, leaves the world untouched and deletes
  reserved entities.
- **ecs.AC5** — An unreflected data component makes `check_reflection` fail with the component named and a fix.
- **ecs.AC6** — Phases are in the documented order; systems in a phase run in declaration order; running one
  phase runs no other phase's systems.
- **ecs.AC7** — JSON loading assigns all value kinds and reports unknown fields, bad enum values and type
  mismatches with codes and suggestions.
- **ecs.AC8** — Generated schemas have types, enums, defaults and `additionalProperties: false`.

## Out of scope

- Multithreaded systems (VCH-107). Transforms and hierarchy (VCH-007). Relationships beyond parent/child stay
  out of the script API.

## Open questions

None.

## Links

- ADRs: [0005 Flecs v4](../../adr/0005-ecs-flecs-v4.md), [0006 barrel](../../adr/0006-flecs-barrel-not-wrapper.md),
  [0018 phases](../../adr/0018-engine-lifecycle-and-phases.md), [0019 modules](../../adr/0019-veach-modules-and-services.md),
  [0026 reflection](../../adr/0026-reflection-everywhere.md)
- Guides: [Flecs v4 notes](../../guides/flecs-v4/README.md), [extending](../../guides/extending.md)
- Code: `engine/ecs/`
