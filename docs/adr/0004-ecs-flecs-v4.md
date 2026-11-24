# 0004: ECS: Flecs v4

Date: 2026-11-24

## Status

Accepted

## Context

Veach needs an ECS with these properties:

- runtime reflection, because tools, schemas, Luau types and MCP all derive from it ([0022](0022-reflection-everywhere.md));
- relationships (hierarchies first);
- modules;
- a query language;
- an introspection REST API;
- a scene text format (Flecs Script, [0007](0007-asset-formats-and-identity.md)).

Flecs offers all of these in one MIT-licensed C/C++ library. Its API changed heavily between v3 and v4, and models often produce v3 code.

## Decision

- **Flecs 4.2.0**, pinned in `cmake/VeachDependencies.cmake`, built as the static target `flecs::flecs_static` with all default addons.
- **Write v4 code only.** v3 idioms are not allowed. The relevant v4 docs are vendored in [docs/guides/flecs-v4/](../guides/flecs-v4/).
- These are the v4.2 specifics we hit while building v0. They are the rules, so check them before writing Flecs code:
  - Reflection with member pointers puts the **pointer first**: `world.component<T>().member(&T::field, "field")`.
  - Accessors are `get<T>()` (const ref, asserts presence), `try_get<T>()` (const pointer or null), `get_mut<T>()` (mutable ref) and `try_get_mut<T>()` (mutable pointer).
  - Type-erased values use `ecs_ptr_new` / `ecs_ptr_copy` / `ecs_ptr_free`. The `ecs_value_*` functions take `ecs_value_t` handles in 4.2.
  - Constants of enums with an **unsigned** underlying type live in `ecs_enum_constant_t::value_unsigned`; `value` is 0. `engine/ecs/src/reflection.cpp` handles both.
  - C++ enum reflection derives constant names at compile time and **fails for types in an anonymous namespace**. Reflected enums go in named namespaces.
  - Creating a *named* entity (or `set_name`) needs a writable, non-staged world. Systems that do it are declared `.immediate()`.
  - Singletons are declared with `.add(flecs::Singleton)` on the component; `world.set<T>()` and `world.get<T>()` then work.
- The engine never calls `world.progress()`. It runs per-phase pipelines itself ([0014](0014-engine-lifecycle-and-phases.md)).
- The REST/explorer addon is enabled at runtime with `--rest` (any build), serving on port 27750.

## Consequences

- Reflection, JSON, REST and modules are not ours to build or maintain.
- Flecs semantics leak into engine code by design ([0005](0005-flecs-barrel-not-wrapper.md)). A future Flecs major version is a migration project, not a dependency bump.
- Agents must be steered away from v3 APIs. `AGENTS.md` says "Flecs v4 only" and points to the vendored docs.

## Alternatives considered

- **EnTT.** Excellent performance, but no runtime reflection, query language, REST API or scene format. We would build most of what we need from Flecs.
- **Own ECS.** Months of work for a worse result. Not where the engine's value is.
- **Flecs v3.** Superseded upstream. v4 query and reflection APIs are better and maintained.
