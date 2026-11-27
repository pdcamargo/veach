# 0046: 64-bit values and positions across Luau, JSON and MCP

Date: 2026-11-27

## Status

Accepted.

## Context

Positions become doubles relative to a frame ([0045](0045-large-world-positions-and-reference-frames.md)). Generated
content is named by 64-bit addresses ([0050](0050-universe-addresses-and-seeds.md)), persistent entities by 64-bit or
UUID ids (VCH-103), seeds are `uint64`, and ticks are `int64` ([0047](0047-universe-clock.md)). All of these must cross
the three text boundaries Veach is built on: strict Luau scripts, JSON (scenes, saves, schemas, the replay log) and MCP,
whose clients are often JavaScript.

What breaks today:

- Luau's native `vector` is float32 ([0013](0013-conventions-and-math.md)): a body-local position at 10^7 m has 1 m
  resolution.
- Luau numbers and JSON numbers in JavaScript are doubles: integers above 2^53 round silently. The marshaller loads
  `i64`/`u64` fields through `double` (`engine/script/src/marshal.cpp`), so an id loses its low bits in scripts with no
  error.
- The reflection walker has no int64 kind, no arrays and no entity references ([0026](0026-reflection-everywhere.md)).

Research: [01-coordinates](../research/01-coordinates.md) §7, [03-generation-pipeline](../research/03-generation-pipeline.md)
§7.1, [11-persistence-versioning](../research/11-persistence-versioning.md) §4.5.

## Decision

**Positions in Luau: a `Position` userdata.**

- Holds three doubles, with fields `x`, `y`, `z` (Luau numbers are doubles, so they are exact).
- Arithmetic: `Position + vector → Position`, `Position − vector → Position`, `Position − Position → vector`. A
  float32 difference keeps its relative precision, so a direction and distance between far points stay correct. No
  `Position + Position`, no scaling of a `Position`.
- `Transform.translation` is a `Position` in Luau. `vector` stays float32 for offsets, directions, velocities and
  local shapes.
- Frame API in `@veach`: `frame_of(entity)`, `convert(position, from_frame, to_frame)`, `offset(a, b) → vector`
  (computed in double through the common ancestor), `up(frame, position)`.
- `veach types` emits `Position` into `types/veach.d.luau`; luau-analyze checks it like any other type.

**64-bit integers never cross Luau, JSON or MCP as numbers.**

| Boundary | Representation |
|---|---|
| Luau | Tagged userdata. Entities already are. Typed ids get their own tag (`Address`, `PersistentId`); other `int64`/`uint64` fields use `I64` / `U64`, with `tostring`, `==`, `<`, construction from a decimal string (`veach.i64("123")`) and `:to_number()`, which errors above 2^53 |
| JSON (scenes, saves, schemas, replay log) | Decimal strings: `"seed": "13377345120000000001"`. Schemas generated from reflection type them `{"type": "string", "pattern": "^-?[0-9]+$"}` |
| MCP | Decimal strings in arguments and results, the same as JSON |

**The one bounded exception is the universe tick.** The clock refuses ticks at or above 2^53
([0047](0047-universe-clock.md)), so Luau sees `UniverseClock.tick` as a plain number. In JSON and MCP it is a string
like every other int64, so JSON consumers have one rule.

**Doubles in JSON** are written with shortest round-trip formatting (the writer's Grisu2 or `std::to_chars`), so
save → load → save is byte-identical. A `dvec3` is `[x, y, z]`. A stored position always names its frame (entity path
or persistent id) next to the coordinates.

**MCP.** `get_component` and `set_component` read and write frame-relative positions. A `frames` tool, registered by
the frames module through module-registered tools (VCH-100), returns the frame tree with each frame's kind and pose,
so an agent can see where things are without system coordinates.

**Engine work** (all generic, [0042](0042-space-toolbox-layering.md)):

- The reflection walker gains an int64 kind, arrays and entity references, carried through JSON, Luau, MCP, schemas
  and the replay hash (VCH-099).
- Until the id types exist, the marshaller's `i64`/`u64` → `double` path fails with a new error code,
  `script.int64_out_of_range` (hint: use the id type or a string), for any value above 2^53 in magnitude. After that,
  the path is removed.
- Record/replay hashes include double and int64 fields bit for bit.

The toolbox registers its own tagged types (`Address`, with a canonical string form) through the same mechanism.

## Consequences

- No silent precision loss at any boundary: an id either arrives exact or the call fails with a code.
- Scripts that move things across long distances use `Position` arithmetic; everything local keeps the cheap `vector`.
- Userdata arithmetic costs more than native `vector` math in Luau; gameplay scripts that do heavy position math
  will feel it.
- Every JSON consumer, MCP clients included, must handle int64 as strings. Switching that later would break all of
  them, so it is fixed now.
- `Position` and the frame API become part of every script; changing them later breaks user content.

**Validated by** VCH-079 (the cost of `Position` arithmetic in Luau: how many operations fit in 0.5 ms on the M5 Max,
scaled to an M1-class estimate, must stay within the script budget) and VCH-094 (a save with int64 ids as strings
round-trips byte-identically). **Reopen if** VCH-079 shows `Position` too slow for gameplay scripts: then
`Transform.translation` stays double in C++ but Luau sees frame-local `vector`s, with an error (not rounding) when a
value is beyond float32's safe range for its frame.

## Alternatives considered

- **Doubles as plain Luau tables `{x, y, z}`:** no type checking against `vector`, and allocation per value.
- **Positions as two `vector`s (high and low float32):** works on the GPU, but unreadable in scripts and in JSON.
- **int64 as JSON numbers, documented as "don't exceed 2^53":** what breaks every JavaScript MCP client the first
  time a real address appears.
- **Make the tick a string in Luau too:** uniform, but every time computation in scripts would go through
  conversions for a value that never exceeds 2^53.
