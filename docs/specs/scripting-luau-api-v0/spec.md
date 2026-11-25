# Scripting: Luau API v0

**Status:** Implemented
**Area:** script
**Owner:** dev lead

## Summary

Gameplay scripts are Luau, `--!strict`, using a small curated `@veach` module: define components, register
systems, spawn entities, read and write components, log, and read time. Types for the module are generated from
reflection, so `luau-analyze` checks scripts against the real engine.

## Motivation

Scripts are where agents iterate fastest. They need a surface that is small, strictly typed, impossible to
corrupt the world with, and cheap to call. Raw Flecs would be none of those.

## Requirements

- MUST expose only `require("@veach")`; scripts never see Flecs.
- MUST provide `veach.component(name, defaults)`: field types come from the defaults (boolean, number, string,
  vector). It creates a reflected runtime component with fields sorted by name. Redefining it identically is
  allowed; redefining it differently is `script.component_conflict`.
- MUST also accept explicit field types where a default cannot express the type: `veach.f32/f64/i32/u32(default?)`,
  `veach.vec3(default?)`, `veach.entity()` (`Entity?`) and `veach.optional(inner)` (`T?`). Each one type-checks as
  the value it stores, so inference from the defaults table is unchanged and `luau-analyze` accepts both forms.
- MUST provide `veach.query(A[, B, C, D])` and `veach.system(name, phase, query, fn(entity, a, b, ...))`.
- MUST provide `veach.spawn(name?)` and Entity methods `id`, `get`, `set`, `has`, `remove`, `despawn`.
- MUST provide `veach.log.{debug,info,warn,error}` and `veach.time.{frame,delta,elapsed}`.
- MUST expose C++ components tagged `veach::Scriptable` as `veach.components.<Name>`.
- MUST make every invocation transactional: a script's top level (components, systems and writes) is
  all-or-nothing. A system run's writes are discarded if it raises an error.
- MUST cross the C++/Luau boundary in batches: one call per system run carrying the whole query result. The
  per-entity loop runs in Luau.
- MUST allow `veach.component` and `veach.system` only while a script's top level runs
  (`script.load_time_only` otherwise).
- MUST generate `types/veach.d.luau` from reflection (`veach types`) so that `luau-analyze` in strict mode
  type-checks component fields, system callbacks and phase names.
- MUST run scripts sandboxed (frozen globals, one environment per script).
- Reads (`get`, query rows) see the world as of invocation start; writes apply when the invocation returns.
- `Entity:set` starts from the component's defaults, so fields missing from the table keep their default.
  Strict types require the full value anyway; the leniency only matters for non-strict code.

## API sketch

```lua
--!strict
local veach = require("@veach")

local Greeting = veach.component("Greeting", { text = "" })

veach.system("Greet", "Simulation", veach.query(Greeting), function(_entity, greeting)
	veach.log.info(`{greeting.text} — frame {veach.time.frame()}`)
end)

veach.spawn("greeter"):set(Greeting, { text = "Hello from Luau" })
```

## Performance

The call model is one C++→Luau call per system run. That call carries the whole query result (all matched
tables), and a Luau-side dispatcher loops over the rows and calls `fn(entity, a, b, ...)`. There is never one
C++→Luau call per entity. Rows are marshalled with cached per-type plans (`engine/script/src/marshal.cpp`).

Measured 2026-11-24 with `build/release/tests/veach-bench-script` (Apple M5 Max, release build). One system
over `Pos` and `Vel` (each one `vector` field):

| System body | 10k entities | 100k entities | per entity |
|---|---|---|---|
| empty `fn` (marshal + dispatch only) | 0.82 ms | 9.2 ms | ~92 ns |
| read both components | 0.90 ms | 10.1 ms | ~101 ns |
| `e:set(Pos, ...)` every row | 1.63 ms | 17.9 ms | ~179 ns |

Before the marshal plans (bootstrap version, which rebuilt reflection data per value): 22.0 ms read-only and
38.4 ms read-write at 100k. The remaining cost is allocation: one Entity userdata and one table per component
per row, every run. Reusing them across runs is VCH-035. It changes semantics (row values become views that are
only valid during the call), so it needs its own decision.

## Acceptance criteria

- **script.AC1** — `apps/lab/scripts/hello.luau` logs `Hello from Luau — frame N` once per frame for 3 frames and
  the run exits 0.
- **script.AC2** — Scripts load through the asset system (a `.meta` is created). Syntax errors fail the run with
  `script.compile_failed`.
- **script.AC3** — An error in a script's top level applies nothing: no writes, no systems.
- **script.AC4** — An error in one system run discards that run's writes only; later runs proceed.
- **script.AC5** — Systems over 1–4 components receive whole batches; vectors and strings round-trip.
- **script.AC6** — Unknown components, phases, fields and modules produce errors with did-you-mean.
- **script.AC7** — `veach.components.Window` is readable and writable from Luau, with enum values as strings.
- **script.AC9** — Explicit field types store the declared storage type, start from their defaults (none:
  zero/nil/absent), and round-trip through Luau. An entity field reads as nil once its target is despawned. A
  default of the wrong type is `script.invalid_value`. `tests/scripts/typed_components.luau` passes
  `luau-analyze` in strict mode.
- **script.AC8** — Generated types cover every `@veach` binding and phase, and the committed
  `types/veach.d.luau` is up to date. `luau-analyze` passes on all scripts in `apps/` (CTest).

## Out of scope

- Input (VCH-006), parent/child (VCH-007), `Behavior` (VCH-005), nested tables and arrays in fields (VCH-008), hot reload (VCH-004),
  native codegen and time budgets (VCH-034), MCP `eval` (VCH-003; it will use `ScriptRuntime::execute_source`).

## Open questions

None.

## Links

- ADRs: [0006 scripting](../../adr/0006-scripting-luau.md), [0022 reflection](../../adr/0022-reflection-everywhere.md)
- Guides: [Luau API](../../guides/luau-api.md), [extending: new binding](../../guides/extending.md)
- Code: `engine/script/`, `types/veach.d.luau`, `.luaurc`
