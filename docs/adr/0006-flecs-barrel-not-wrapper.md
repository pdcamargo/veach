# 0006: Flecs barrel, not a wrapper

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0005).

## Context

User code should see one namespace (`veach::`) and one header. A semantic wrapper around Flecs would duplicate a
large, well-documented API, drift from upstream and hide idioms agents already know. Leaving `flecs::` everywhere
would leave the engine's surface undefined.

## Decision

- `engine/ecs/include/veach/ecs/flecs.hpp` is a **barrel of aliases**, not a wrapper:
  - types `veach::World`, `Entity`, `EntityView`, `Id`, `Iter`, `System`, `Observer`, `Cursor`;
  - templates `Query<C...>`, `QueryBuilder<C...>`, `Component<T>`;
  - builtins by `using`: `ChildOf`, `IsA`, `Wildcard`, `OnAdd`, `OnRemove`, `OnSet`, `Singleton`;
  - `veach::ecs` is a namespace alias for `::flecs`, for anything without an alias.
- C++ uses Flecs idioms through the aliases, e.g. `world.system<Spin>("Spin").kind<veach::phase::Simulation>().each(...)`.
- Engine components live in `veach::`; phase tags in `veach::phase::`.
- **One umbrella header:** `#include <veach/veach.hpp>` (`engine/include/veach/veach.hpp`). Heavy optional headers
  (e.g. `veach/ecs/json.hpp`, which needs nlohmann) are included separately.
- **No `flecs::` outside `engine/`.** `tools/scripts/check_repo.py` fails if `flecs::` appears under `apps/` (and
  any other non-engine code tree it checks), comments included. It runs in CTest and CI. Engine code may use
  `flecs::` directly.
- Scripts never see Flecs ([0007](0007-scripting-luau.md)).

## Consequences

- Flecs documentation applies unchanged, with `veach::` in place of `flecs::`.
- Adding an alias is one line in `flecs.hpp`; until then `veach::ecs::Foo` works.
- The barrel gives no isolation from Flecs API changes; accepted ([0005](0005-ecs-flecs-v4.md)).

## Alternatives considered

- **Semantic wrapper.** A parallel API plus docs, and every new Flecs feature needs wrapping.
- **Raw `flecs::` everywhere.** No defined user surface; two names for one concept in examples.
- **`using namespace flecs` inside `veach`.** Hundreds of names and collisions; explicit aliases read and grep better.
