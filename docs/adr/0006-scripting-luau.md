# 0006: Scripting with Luau

Date: 2026-11-24

## Status

Accepted

## Context

Gameplay iteration and AI agents both need a fast, typed, sandboxable scripting layer. The script surface must be curated (stable, typed, documented), not a mirror of Flecs. Script writes must be safe: an error halfway through a script must not leave the world half-modified. Crossing the C++/VM boundary per entity is the classic scripting performance trap.

## Decision

**VM and typing.**

- Luau 0.748 is pinned. The engine links `Luau.VM` and `Luau.Compiler`.
- `luau-analyze` is built from the same source tree (target `Luau.Analyze.CLI`), so the type checker and the VM always agree.
- `--!strict` is the default (`.luaurc`: `"languageMode": "strict"`).
- `.luaurc` maps the alias `"veach": "./types/veach.d"` to the generated definitions.

**Curated API.** `require("@veach")` is the only module. Scripts never see raw Flecs.

| Area | API |
|---|---|
| Components | `veach.component(name, defaults)`, `veach.components.<Name>` |
| Systems | `veach.query(A[, B, C, D])`, `veach.system(name, phase, query, fn)` |
| Entities | `veach.spawn(name?)`, `Entity:get/set/has/remove/despawn/id` |
| Utilities | `veach.log.{debug,info,warn,error}`, `veach.time.{frame,delta,elapsed}` |

`veach.components.<Name>` exposes C++ components tagged `veach::Scriptable` ([0022](0022-reflection-everywhere.md)).

**Script components.**

- Field types are inferred from the defaults: boolean → `bool`, number → `f64`, string → `std::string`, vector → `vec3`.
- Fields are sorted by name so the layout is deterministic.
- Each component is created as a Flecs runtime struct, so it is reflected like any C++ component.
- Redefining one with different fields is `script.component_conflict`.

**Transactions.** Every invocation records into a `CommandBuffer` (`engine/ecs`):

- A script's top level is all-or-nothing. Its components' entities, systems and writes apply only if it returns normally. Systems are registered after success.
- Each system run applies its writes on success and discards them on error. The system runs again next time.
- `veach.spawn` reserves an id immediately so chained `:set` works. On discard, that entity is deleted.
- Reads (`get`, `has`) see the world as of invocation start, not pending writes.

**Batching.**

- A script system receives the whole query result in **one C++→Luau call**. C++ builds an array of entities plus one array per component. A Luau dispatcher (`dispatcher_source` in `script_runtime.cpp`) runs the per-entity loop inside the VM.
- Luau→C++ calls per entity (`set`, `get`) are allowed: they are cheap command-buffer appends or direct reads, never callbacks.
- Script systems are `.immediate()` and collect the full batch before calling Luau, so applying the buffer afterwards is safe. Named spawns need a writable world ([0004](0004-ecs-flecs-v4.md)).

**Sandbox.** `luaL_sandbox` freezes the globals and libraries. Each script runs in its own thread with `luaL_sandboxthread`.

**Typed API shape.** `veach.system(name, phase, veach.query(A, B), function(entity, a, b) ... end)`. A first prototype overloaded `system` itself, as an intersection over arities. Luau's new solver then could not infer the lambda's parameter types. So the overloads moved to `query()`, which returns `Query<T...>` with a type-only phantom field `_row: (Entity, T...) -> ()`. `system` became a single generic over the type pack, and the callback is fully typed: field typos, wrong value types and misspelled phases are all caught. `veach types` generates `types/veach.d.luau` from reflection.

## Consequences

- Scripts are typed end to end. CTest runs `luau-analyze` on every `.luau` under `apps/` and checks that the committed `veach.d.luau` is current.
- An error in a script can't corrupt the world. The process still exits 1 because an error was logged ([0014](0014-engine-lifecycle-and-phases.md)).
- v0 script field types are limited (no nested tables, arrays, entity refs). Queries take 1 to 4 components.
- Per-run row tables allocate. Pooling is a performance backlog item, to be measured first.
- No yielding, no `require` of user modules, no interrupt or timeout yet (backlog).

## Alternatives considered

- **Lua 5.4 / LuaJIT.** No gradual static types. LuaJIT is unmaintained on arm64 Mac and has no sandboxing story.
- **C# (Mono/CoreCLR).** Heavy runtime, GC pauses, hard to sandbox, and slow to embed and build.
- **Wren / AngelScript.** Small ecosystems, weak tooling, no mainstream type checker.
- **Binding raw Flecs to scripts.** Unstable, untyped surface, and per-entity crossings by default. Rejected by design.
