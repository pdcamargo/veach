# 0005: Flecs barrel, not a wrapper

Date: 2026-11-24

## Status

Accepted

## Context

User code should see one namespace (`veach::`) and one header. Wrapping Flecs semantically, with our own entity, query and system classes, would duplicate a large, well-documented API. It would drift from upstream and hide the idioms agents already know. Leaving `flecs::` everywhere would make the engine's surface undefined: anything Flecs can do would become "the API".

## Decision

- `engine/ecs/include/veach/ecs/flecs.hpp` is a **barrel of aliases**, not a wrapper:
  - Types: `veach::World`, `Entity`, `EntityView`, `Id`, `Iter`, `System`, `Observer`, `Cursor`.
  - Templates: `Query<C...>`, `QueryBuilder<C...>`, `Component<T>`.
  - Builtins pulled in with `using` declarations: `ChildOf`, `IsA`, `Wildcard`, `OnAdd`, `OnRemove`, `OnSet`, `Singleton`.
  - `veach::ecs` is a namespace alias for `::flecs`, for anything without an alias.
- C++ code uses Flecs idioms directly through the aliases, for example `world.system<Spin>("Spin").kind<veach::phase::Simulation>().each(...)`.
- Engine components live in `veach::`. Phase tags live in `veach::phase::`.
- **One umbrella header:** `#include <veach/veach.hpp>` (`engine/include/veach/veach.hpp`) gives users everything. Heavy optional headers (for example `veach/ecs/json.hpp`, which needs nlohmann) can be included separately.
- **No `flecs::` outside `engine/`.** `tools/scripts/check_repo.py` fails if `flecs::` appears anywhere under `apps/`. It runs as a CTest test and in CI. Engine code may use `flecs::` directly.
- Scripts never see Flecs at all ([0006](0006-scripting-luau.md)).

## Consequences

- Flecs documentation applies unchanged, with `veach::` in place of `flecs::`.
- Adding an alias is a one-line change in `flecs.hpp`. Until one exists, `veach::ecs::Foo` works.
- The barrel gives no isolation from Flecs API changes. That is accepted; see [0004](0004-ecs-flecs-v4.md).
- The grep check is deliberately simple: it also rejects `flecs::` in comments under `apps/`.

## Alternatives considered

- **Semantic wrapper.** Hides Flecs, costs a parallel API plus docs, and every new Flecs feature needs wrapping. Rejected.
- **Raw `flecs::` everywhere.** No defined user surface, and two names for the same concept in examples. Rejected.
- **`using namespace flecs` inside `veach`.** Pulls in hundreds of names, including C macros' companions, with collisions. Explicit aliases are easier to read and to grep.
