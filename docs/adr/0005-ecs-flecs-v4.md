# 0005: ECS: Flecs v4

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0004).

## Context

Veach needs an ECS with runtime reflection (tools, schemas, Luau types and MCP all derive from it,
[0026](0026-reflection-everywhere.md)), relationships, modules, a query language, an introspection REST API and a
scene text format ([0010](0010-asset-formats-and-identity.md)). Flecs offers all of these in one MIT-licensed
C/C++ library. Its API changed heavily between v3 and v4, and models often produce v3 code.

## Decision

- **Flecs 4.2.0**, pinned in `cmake/VeachDependencies.cmake`, built as `flecs::flecs_static` with all default addons.
- **Write v4 code only.** The relevant v4 docs are vendored in [docs/guides/flecs-v4/](../guides/flecs-v4/).
- v4.2 specifics that are rules:
  - Reflection with member pointers puts the **pointer first**: `world.component<T>().member(&T::field, "field")`.
  - Accessors: `get<T>()` (const ref, asserts presence), `try_get<T>()` (const pointer or null), `get_mut<T>()`
    (mutable ref), `try_get_mut<T>()` (mutable pointer).
  - Type-erased values use `ecs_ptr_new` / `ecs_ptr_copy` / `ecs_ptr_free`; the `ecs_value_*` functions take
    `ecs_value_t` handles.
  - Constants of enums with an **unsigned** underlying type live in `ecs_enum_constant_t::value_unsigned`
    (`value` is 0); `engine/ecs/src/reflection.cpp` handles both.
  - C++ enum reflection **fails for types in an anonymous namespace**. Reflected enums go in named namespaces.
  - Creating a *named* entity (or `set_name`) needs a writable, non-staged world. Systems that do it are
    `.immediate()`.
  - Singletons are declared with `.add(flecs::Singleton)` on the component; then `world.set<T>()` and
    `world.get<T>()` work.
- The engine never calls `world.progress()`; it runs per-phase pipelines itself ([0018](0018-engine-lifecycle-and-phases.md)).
- The REST/explorer addon is enabled at runtime with `--rest` (any build), on port 27750.

## Consequences

- Reflection, JSON, REST and modules are not ours to build or maintain.
- Flecs semantics leak into engine code by design ([0006](0006-flecs-barrel-not-wrapper.md)). A future Flecs major
  version is a migration project, not a dependency bump.
- Agents must be steered away from v3 APIs; `AGENTS.md` says "Flecs v4 only" and points to the vendored docs.

## Alternatives considered

- **EnTT.** Fast, but no runtime reflection, query language, REST API or scene format; we would rebuild them.
- **Our own ECS.** A large effort for a worse result, and not where the engine's value is.
- **Flecs v3.** Superseded upstream.
