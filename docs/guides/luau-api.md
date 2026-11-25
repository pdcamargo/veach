# The `@veach` Luau API (v0)

Scripts are Luau, always `--!strict` ([ADR 0006](../adr/0006-scripting-luau.md)). They see only this curated
module — never raw Flecs. Spec: [scripting-luau-api-v0](../specs/scripting-luau-api-v0/spec.md).
Implementation: `engine/script/src/script_runtime.cpp`. Types: `types/veach.d.luau` (generated).

```lua
--!strict
local veach = require("@veach")

local Greeting = veach.component("Greeting", { text = "" })

veach.system("Greet", "Simulation", veach.query(Greeting), function(_entity, greeting)
	veach.log.info(`{greeting.text} — frame {veach.time.frame()}`)
end)

local greeter = veach.spawn("greeter")
greeter:set(Greeting, { text = "Hello from Luau" })
```

`build/debug/apps/lab/veach-lab --script scripts/hello.luau --frames 3 --headless` runs this
(`apps/lab/scripts/hello.luau`).

## Reference

### `require("@veach")`

The only module in v0. Any other name fails with `script.unknown_module`.

### `veach.component(name, defaults) -> Component<T>`

Defines a script component. The **defaults table defines the fields and their types**:

| Default value | Field type | Stored as |
|---|---|---|
| `true` / `false` | `boolean` | `bool` |
| a number | `number` | `f64` |
| a string | `string` | `std::string` |
| `vector.create(x, y, z)` | `vector` | `veach::math::vec3` |

When a default value can't express the type you need, use an **explicit type** instead of a plain default:

```lua
local Health = veach.component("Health", {
	hp = veach.f32(100),                  -- number, stored as f32 (default 100)
	level = veach.i32(1),                 -- number, stored as i32
	armor = veach.u32(),                  -- number, stored as u32 (default 0)
	precise = veach.f64(0.5),             -- number, stored as f64 (what a plain number gives)
	offset = veach.vec3(),                -- vector (default 0, 0, 0)
	target = veach.entity(),              -- Entity?, starts as nil
	nickname = veach.optional(""),        -- string?, starts absent
	shield = veach.optional(veach.f32()), -- number?, stored as an optional f32
	alive = true,                         -- inferred: boolean
})
```

| Explicit type | Luau type | Stored as |
|---|---|---|
| `veach.f32(default?)`, `veach.f64(default?)` | `number` | `f32`, `f64` |
| `veach.i32(default?)`, `veach.u32(default?)` | `number` | `i32`, `u32` |
| `veach.vec3(default?)` | `vector` | `veach::math::vec3` |
| `veach.entity()` | `Entity?` | an entity reference; reads as `nil` when unset or once the entity is despawned |
| `veach.optional(inner)` | `T?` for the inner type | a reflected `{ present, value }` struct; `nil` means absent |

Each explicit type checks as the value it stores, so strict-mode inference is the same as with plain defaults:
`Health` is a `Component<{ hp: number, ..., target: Entity?, nickname: string? }>`. In JSON data files, entity
fields are entity paths (`"level::player"`) or `null`, and optional fields are a value or `null`.

Nested tables are not supported yet (`script.unsupported_field`). The component is a
real reflected Flecs component (fields sorted by name), visible to tools like any C++ one.
Defining the same name again with identical fields returns the existing component; different
fields, or a name already used by a C++ component or entity, fail with `script.component_conflict`.

A `Component<T>` is a read-only table `{ name: string, defaults: T }`.

### `veach.query(A [, B, C, D]) -> Query<A, ...>`

One to four components. Only used as the third argument of `veach.system`.

### `veach.system(name, phase, query, fn)`

Registers a system. `fn(entity, a, b, ...)` is called once per entity matching the query,
every time `phase` runs. `phase` is one of `"Startup"`, `"Input"`, `"Simulation"`,
`"PostSimulation"`, `"TransformPropagation"`, `"PreExtract"`, `"Extract"`, `"Render"`,
`"Shutdown"` (see [ecs-and-phases](../specs/ecs-and-phases/spec.md)); a typo fails with
`ecs.unknown_phase` and a suggestion. Simulation phases run once per fixed 1/60 s step.

System names are global: a duplicate fails with `script.duplicate_system`.

The component values passed to `fn` are **copies**. To change a component, write it back with `entity:set`.

### `veach.spawn(name?) -> Entity` / `veach.spawn({ name = ..., parent = e }) -> Entity`

Spawns an entity. Its id is valid immediately (so you can call `:set` on it); its name, parent and
components appear when the current invocation succeeds. With a `parent`, the entity is its child (its
`Transform` is relative to the parent, and its name is scoped to it: `rig::eye`). See
[transforms](../specs/transforms/spec.md).

### `Entity` methods

| Method | Effect |
|---|---|
| `e:id() -> number` | The entity id. |
| `e:get(C) -> T?` | The component value, or `nil`. Reads the world **now**. |
| `e:set(C, value)` | Records a write. Fields missing from `value` take the component's defaults. Unknown fields or wrong types fail with `script.invalid_value` and a suggestion. |
| `e:has(C) -> boolean` | Whether the entity has the component now. |
| `e:remove(C)` | Records a removal. |
| `e:despawn()` | Records a deletion. |
| `e:parent() -> Entity?` | The parent, or `nil` at the root. Reads the world now. |
| `e:children() -> { Entity }` | The children, in id order. Reads the world now. |

Entities compare with `==`, print as `Entity(<id> '<name>')` and `typeof(e) == "Entity"`.

### `veach.input`

Actions, never keys. Bind them in `veach.project.json` (one or two keys or mouse buttons each; names are
`veach::Key` constants, and the schema lists them):

```json
"input": { "jump": ["space"], "move_forward": ["w", "up"], "fire": ["mouse_left"] }
```

| Function | Result |
|---|---|
| `down(action) -> boolean` | Held this frame. |
| `pressed(action) -> boolean` / `released(action)` | Went down / up this frame (true for the whole frame: read them in `"Input"` to act once). |
| `axis(negative, positive) -> number` | −1, 0 or 1 from two held actions. |

An unknown action raises `input.unknown_action` with a suggestion. The lab's `scripts/fly_camera.luau` is a
complete example. Agents press keys with the MCP tool `send_input`. Spec: [input](../specs/input/spec.md).

### `veach.quat`

Quaternions are `{ x, y, z, w }` tables (the `rotation` field of `Transform`). These helpers use glm, so they
match C++ exactly. Conventions: right-handed, +Y up, −Z forward, radians.

| Function | Result |
|---|---|
| `identity()` | No rotation. |
| `from_axis_angle(axis: vector, angle)` | `angle` radians about `axis` (counter-clockwise looking down the axis). |
| `from_euler(yaw, pitch, roll?)` | Yaw about +Y, then pitch about the turned +X (positive looks up), then roll. |
| `mul(a, b)` | `a * b`: rotates by `b` first, then by `a`. |
| `rotate(q, v: vector) -> vector` | `v` rotated by `q`. `rotate(q, vector.create(0, 0, -1))` is where `q` looks. |
| `inverse(q)`, `normalize(q)` | |
| `look_rotation(forward: vector, up: vector?)` | The rotation that turns −Z to `forward`, keeping +Y close to `up`. |

### `veach.log.debug / info / warn / error(message: string)`

Logs under category `script`. Anything logged at `error` makes the process exit with code 1.

### `veach.time`

| Function | Returns |
|---|---|
| `veach.time.frame()` | Current frame number, starting at 1. |
| `veach.time.delta()` | Seconds covered by the current phase: exactly 1/60 in simulation phases; the frame's delta in per-frame phases (1/60 in headless runs). |
| `veach.time.elapsed()` | Simulated seconds since start (fixed steps × 1/60). |

There is no wall-clock time in the API, on purpose (determinism).

### `veach.components.<Name>`

C++ components tagged `veach::Scriptable` (all fields Luau-representable). The engine exposes:

- `Window` (`title`, `width`, `height`, `mode: "windowed" | "fullscreen"`, `vsync`)
- `Transform` (`translation: vector`, `rotation: { x, y, z, w }` quaternion, `scale: vector`)
- `Camera` (`fov_y` radians, `near_plane` meters, `active`); see [render-views](../specs/render-views/spec.md)

```lua
local camera = veach.spawn("camera")
camera:set(veach.components.Transform, {
	translation = vector.create(1.5, -0.05, 2.6),
	rotation = { x = 0, y = 0.258819, z = 0, w = 0.965926 }, -- 30° about +Y
	scale = vector.create(1, 1, 1),
})
camera:set(veach.components.Camera, { fov_y = math.rad(50), near_plane = 0.1, active = true })
```

Changing a window:

```lua
local Window = veach.components.Window
veach.system("Retitle", "Input", veach.query(Window), function(e, w)
	if veach.time.frame() == 1 then
		e:set(Window, { title = "Hello", width = w.width, height = w.height, mode = w.mode, vsync = w.vsync })
	end
end)
```

Changing `Window` changes the real OS window ([platform-and-windows](../specs/platform-and-windows/spec.md)).
Enums are their constant names as strings.

## Hot reload

A windowed run reloads changed scripts every 250 ms (`"hot_reload": false` in `veach.project.json` turns it
off); a headless run reloads when asked (MCP `reload`). A reloaded script runs again **as if fresh**: its
systems are replaced by name, and the entities its top level spawned are despawned and spawned again, so state
kept on them resets. Entities your systems spawned later are kept. If the new version fails (syntax or
runtime error), nothing changes and the old version keeps running. Changing a `veach.component`'s fields needs
a restart. Spec: [hot-reload](../specs/hot-reload/spec.md).

## Semantics

- **Transactions.** Every *invocation* — running a script's top level, or one run of one
  system — records its writes (`spawn`, `set`, `remove`, `despawn`) into a command buffer.
  It is applied if the invocation returns normally and **discarded** if it raises.
  - A failing top level applies nothing: no writes, and none of its systems are registered.
    (Components it defined stay defined.)
  - A failing system run discards only that run's writes (for every entity of the batch); the
    system runs again next time. Errors are logged with code `script.runtime_error`.
- **Reads vs writes.** `get` and `has` see the world as it was when the invocation started;
  your own writes become visible after it returns.
- **Batching.** C++ calls into Luau once per system run with the whole query result; the
  per-entity loop runs inside Luau. There is no per-entity query API — put what you read in
  the query.
- **Load time only.** `veach.component` and `veach.system` only work while a script's top
  level runs (`script.load_time_only` otherwise).
- **Sandbox.** Each script runs in its own sandboxed environment; globals and builtin libraries are read-only.
- **Errors** carry a stable code and usually a fix, e.g.
  `unknown component 'Greting' — Did you mean 'Greeting'? [script.unknown_component]`.
  All codes: [../reference/errors.md](../reference/errors.md).
- **Exit code.** An app exits 0 only if nothing was logged at error level during the run,
  so a script error in any frame fails CI.

## Type checking

`types/veach.d.luau` is generated from reflection (`veach types --out types/veach.d.luau`, see
[building.md](building.md#regenerating-generated-files)) and committed. The root `.luaurc` maps
the `@veach` alias to it and sets strict mode:

```json
{ "languageMode": "strict", "aliases": { "veach": "./types/veach.d" } }
```

Run the checker built from the pinned Luau:

```sh
build/debug/_deps/luau-build/luau-analyze apps/lab/scripts/hello.luau
ctest --preset debug -R luau-analyze      # every .luau under apps/
```

The types catch misspelled phases, wrong field types and unknown fields at edit time; the
runtime checks the same things with actionable errors.

Not in v0 (see the backlog): input actions from Luau, parent/child relationships, nested
table fields, `Behavior` components with `on_update`, hot reload.
