# 0026: Reflection everywhere

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0022).

## Context

Tools, MCP, JSON data files, JSON Schemas, Luau marshalling and Luau type generation all need the shape of
components. If each derived it its own way, they would disagree. A component without reflection is invisible to
all of them.

## Decision

**Mandatory reflection.**

- C++ components are reflected with plain Flecs idioms: `world.component<T>().member(&T::field, "field")`.
- At the end of the `modules` init step the engine runs `check_reflection(world)`. If any component outside
  `flecs.*` has data (size > 0) and no `EcsType`, init fails with `ecs.unreflected_component`, listing the
  components and the fix.
- Tags, `flecs.*` components and module entities are exempt.

**One walker** (`veach/ecs/reflection.hpp`):

- `value_kind(type)` classifies a type as `boolean`, `integer`, `number`, `string`, `vec3`, `enumeration`,
  `structure` or `unsupported`.
- `struct_fields`, `enum_constants`, `read_number` / `write_number` and `read_enum` / `write_enum` handle the
  details, including unsigned-enum constants ([ADR 0005](0005-ecs-flecs-v4.md)).
- Its consumers can't disagree: JSON loading (`from_json`, with did-you-mean errors), JSON Schema generation
  (`json_schema`, with defaults), Luau marshalling (`ScriptRuntime`), Luau type generation
  (`generate_luau_types`), and the MCP server's component reads and writes.

**Builtin reflection** (`register_builtin_reflection`, part of `CoreModule`): `std::string` is an opaque type
serialized as a string; `veach::math` `vec2`, `vec3`, `vec4` and `quat` are structs; every engine component is
reflected.

**Script visibility is opt-in.**

- The `veach::Scriptable` tag exposes a C++ component as `veach.components.<Name>` and as an exported type in
  `veach.d.luau`.
- All its fields must be representable in Luau; otherwise the runtime warns and skips it.
- Script-defined components are Flecs runtime structs, so they are reflected by construction
  ([ADR 0007](0007-scripting-luau.md)).

## Consequences

- Adding a field updates the JSON reader, the schemas, script access, MCP and the script types at once; the
  generated-file tests then ask for a regeneration.
- Reflection boilerplate (`.member` calls) is manual. A macro or generator can come if it proves noisy.
- `unsupported` kinds (arrays, vectors, maps, non-string opaque types) are skipped by JSON and Luau today.
  Supporting them is incremental work in one place.
- Startup fails loudly on a forgotten reflection, which is the point.

## Alternatives considered

- **Per-consumer descriptions** (hand-written schemas, bindings, JSON code). Guaranteed drift.
- **A C++ reflection library (refl-cpp, Boost.Describe) or C++26 reflection.** A second reflection system next to
  Flecs's, which the ECS, REST and JSON already use.
- **Optional reflection with warnings.** Warnings get ignored.
