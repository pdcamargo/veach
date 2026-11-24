# 0022: Reflection everywhere

Date: 2026-11-24

## Status

Accepted

## Context

Tools, MCP, JSON data files, JSON Schemas, Luau marshalling and Luau type generation all need to know the shape of components. If each derived that shape its own way, they would disagree. A component without reflection is invisible to all of them ([ARCHITECTURE.md](../ARCHITECTURE.md): "Every component is reflected").

## Decision

**Mandatory reflection.**

- C++ components are reflected with plain Flecs idioms: `world.component<T>().member(&T::field, "field")`.
- At the end of the `modules` init step the engine runs `check_reflection(world)`. If any component outside `flecs.*` has data (size > 0) and no `EcsType`, init fails with `ecs.unreflected_component`. The error lists the components and shows the fix.
- Tags, `flecs.*` components and module entities are exempt.

**One walker** (`veach/ecs/reflection.hpp`):

- `value_kind(type)` classifies a type as `boolean`, `integer`, `number`, `string`, `vec3`, `enumeration`, `structure` or `unsupported`.
- `struct_fields`, `enum_constants`, `read_number` / `write_number` and `read_enum` / `write_enum` handle the details, including unsigned-enum constants ([0004](0004-ecs-flecs-v4.md)).
- Four consumers use it, so they can't disagree:
  - JSON loading (`from_json`, with did-you-mean errors);
  - JSON Schema generation (`json_schema`, with defaults);
  - Luau marshalling (`ScriptRuntime`);
  - Luau type generation (`generate_luau_types`).

**Builtin reflection** (`register_builtin_reflection`, part of `CoreModule`):

- `std::string` is an opaque type serialized as a string.
- `veach::math` `vec2`, `vec3`, `vec4` and `quat` are structs.
- Engine components (`Time`, `Random`/`Rng`, `Window`, `WindowPlatformState`, `Input`, `InputAction`, `AssetMeta`, `ProjectConfig`) are all reflected.

**Script visibility is opt-in.**

- The `veach::Scriptable` tag exposes a C++ component as `veach.components.<Name>`, and as an exported type in `veach.d.luau`.
- All of its fields must be representable in Luau; otherwise the runtime warns and skips it.
- `Window` and the lab's `Spinner` are Scriptable. Script-defined components are Flecs runtime structs, so they are reflected by construction ([0006](0006-scripting-luau.md)).

## Consequences

- Adding a field to a reflected struct updates the JSON reader, the schemas, script access and the script types at once. The generated-file tests then ask for a regeneration.
- Reflection boilerplate (`.member` calls) is manual for now. A macro or generator to cut it down can come later, if it proves noisy.
- `unsupported` kinds (arrays, vectors, maps, non-string opaque types) are skipped by JSON and Luau today. Supporting them is incremental work in one place.
- Startup fails loudly on a forgotten reflection, which is the point.

## Alternatives considered

- **Per-consumer descriptions** (hand-written schemas, Luau bindings, JSON code). Guaranteed drift.
- **A C++ reflection library (refl-cpp, Boost.Describe) or waiting for C++26 reflection.** A second reflection system next to Flecs's. Flecs meta is already what the ECS, REST and JSON use.
- **Reflection optional with warnings.** Warnings get ignored. The non-negotiable makes it an error.
