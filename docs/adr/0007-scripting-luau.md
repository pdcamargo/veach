# 0007: Scripting with Luau

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0006).

## Context

Gameplay iteration and AI agents need a fast, typed, sandboxable scripting layer with a curated surface, not a
mirror of Flecs. An error halfway through a script must not leave the world half-modified. Crossing the C++/VM
boundary per entity is the classic scripting performance trap. Spec:
[scripting-luau-api-v0](../specs/scripting-luau-api-v0/spec.md).

## Decision

**VM and typing.**

- Luau 0.748, pinned. The engine links `Luau.VM` and `Luau.Compiler`; `luau-analyze` (`Luau.Analyze.CLI`) is built
  from the same tree, so checker and VM agree.
- `--!strict` is the default (`.luaurc`: `"languageMode": "strict"`), and `.luaurc` maps `"veach"` to the generated
  `./types/veach.d`.

**Curated API.** `require("@veach")` is the only module.

| Area | API |
|---|---|
| Components | `veach.component(name, defaults)`, `veach.components.<Name>` |
| Systems | `veach.query(A[, B, C, D])`, `veach.system(name, phase, query, fn)` |
| Entities | `veach.spawn(name?)`, `Entity:get/set/has/remove/despawn/id` |
| Utilities | `veach.log.{debug,info,warn,error}`, `veach.time.{frame,delta,elapsed}` |

`veach.components.<Name>` exposes C++ components tagged `veach::Scriptable` ([0026](0026-reflection-everywhere.md)).
The full surface is in [guides/luau-api.md](../guides/luau-api.md).

**Script components.** Field types are inferred from defaults (boolean → `bool`, number → `f64`, string →
`std::string`, vector → `vec3`), sorted by name for a deterministic layout, and created as Flecs runtime structs, so
they are reflected like C++ components. Redefining one with different fields is `script.component_conflict`.

**Transactions.** Every invocation records into a `CommandBuffer` (`engine/ecs`):

- A script's top level is all-or-nothing: its components, entities, systems and writes apply only if it returns
  normally; systems are registered after success.
- Each system run applies its writes on success and discards them on error; it runs again next time.
- `veach.spawn` reserves an id immediately so chained `:set` works; on discard that entity is deleted.
- Reads (`get`, `has`) see the world as of invocation start, not pending writes.

**Batching.** A script system receives its whole query result in **one C++→Luau call** (an entity array plus one
array per component); a Luau dispatcher (`dispatcher_source` in `script_runtime.cpp`) runs the per-entity loop in the
VM. Luau→C++ calls per entity (`set`, `get`) are cheap command-buffer appends or direct reads, never callbacks.
Script systems are `.immediate()` and collect the full batch before calling Luau.

**Sandbox.** `luaL_sandbox` freezes globals and libraries; each script runs in its own thread with
`luaL_sandboxthread`.

**Typed API shape.** `veach.system(name, phase, veach.query(A, B), function(entity, a, b) ... end)`. The arity
overloads live on `query()`, which returns `Query<T...>` with a type-only phantom field `_row: (Entity, T...) -> ()`;
`system` is a single generic over the type pack, so Luau's solver infers the callback's parameter types (an
intersection-overloaded `system` could not). `veach types` generates `types/veach.d.luau` from reflection.

## Consequences

- Scripts are typed end to end. CTest runs `luau-analyze` on every `.luau` under `apps/` and checks the committed
  `veach.d.luau` is current.
- A script error cannot corrupt the world; the process still exits 1 because an error was logged ([0018](0018-engine-lifecycle-and-phases.md)).
- Script field types are limited (no nested tables, arrays or entity refs); queries take 1 to 4 components.
- No yielding, no `require` of user modules, and no interrupt or timeout yet. Procgen hooks in Luau will need some
  of these; that is a research-phase question.

## Alternatives considered

- **Lua 5.4 / LuaJIT.** No gradual static types; LuaJIT is unmaintained on arm64 Mac and has no sandboxing story.
- **C# (Mono/CoreCLR).** Heavy runtime, GC pauses, hard to sandbox and to embed.
- **Wren / AngelScript.** Small ecosystems, weak tooling, no mainstream type checker.
- **Binding raw Flecs to scripts.** Unstable, untyped surface with per-entity crossings by default.
