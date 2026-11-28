# Input actions in Luau

**Status:** Implemented
**Area:** script
**Owner:** dev lead

## Summary

Games read **actions**, never keys. `veach.project.json` binds each action to one or two keys, mouse buttons or
gamepad buttons, and each **analog action** (an axis) to a source (mouse motion or wheel, a gamepad stick or
trigger) and two keys as a digital fallback. Scripts ask `veach.input.down("jump")`, `pressed`, `released`,
`axis("look_x")` and `axis(negative, positive)`. Tests and agents inject events at the platform level
(`send_input` over MCP), so the whole path from key or stick to action to script runs headless, and recordings
replay analog input bit for bit. The lab's fly camera is a script built on it.

## Motivation

Gameplay needs input, and agents need to drive it without a keyboard. Binding through data keeps scripts
rebindable and lets tools list every action. Flight and walking (SP-13) need mouse look and analog sticks.
Backlog: VCH-006, VCH-101 (analog axes), VCH-036 (gamepads).

## Requirements

- `"input": { "<action>": ["<key>", "<key>"?] }` in `veach.project.json`. Key names are `veach::Key` constants:
  letters, digits, `space`, arrows, modifiers, `f1`–`f12`, and `mouse_left`/`mouse_right`/`mouse_middle`. An
  unknown key fails project loading with `input.unknown_key` and a suggestion. The generated schema lists every
  key. Each action becomes an entity `veach::actions::<name>` with `InputAction` (visible to MCP).
- `veach.input.down/pressed/released(action) -> boolean` and `veach.input.axis(negative, positive) -> number`
  (−1, 0 or 1). An unknown action raises `input.unknown_action` with a suggestion. `pressed` and `released` hold
  for the whole frame.
- `Platform::inject(event)` queues an event delivered with the next poll, after the OS ones. MCP
  `send_input { key, down }` uses it.
- An untranslated OS key (`Key::unknown`) never changes input state.

### Analog axes and gamepads

- `"axes": { "<action>": { "source", "negative", "positive", "dead_zone", "sensitivity", "invert" } }` in
  `veach.project.json`, every field optional. `source` is a `veach::Axis`: `mouse_x`, `mouse_y`,
  `mouse_wheel_x`, `mouse_wheel_y` (relative), `gamepad_left_x/y`, `gamepad_right_x/y` (−1..1) and
  `gamepad_left/right_trigger` (0..1) (absolute). +Y is up for the mouse and the sticks. An unknown source
  fails with `input.unknown_axis`, an unknown key with `input.unknown_key`, both with a suggestion; an unknown
  field is `data.unknown_field`; a `dead_zone` outside [0, 1) is `data.type_mismatch`. Each axis becomes an
  entity `veach::actions::<name>` with `InputAxis` (its binding and `value`).
- Value = (`sensitivity` × dead-zoned source + down(`positive`) − down(`negative`)), negated when `invert`. The
  dead zone reads |v| ≤ `dead_zone` as 0; beyond it an absolute axis is rescaled to start at 0 and still reach 1,
  a relative one loses `dead_zone`. Keys add exactly 1, unscaled.
- **Quantization.** Every analog value is fixed point, 1/65536 steps (`axis_one`), rounded half away from zero
  and saturated to int32, from the platform event on (`PlatformEvent::value`); axis values are rounded to the
  same grid. Recordings store the integers, so a replay applies exactly what the run applied.
- **Relative axes count once.** Mouse motion accumulates in `Input` until a fixed step runs; the engine clears it
  after a frame's first fixed step. That step (and the frame's `Input` phase) sees all of it; later steps and
  the per-frame phases after them see 0; a frame without steps keeps it for the next. Absolute axes hold their
  position until they move.
- **Gamepads.** The SDL backend opens every gamepad as it connects and closes it as it leaves; `Input.gamepads`
  counts them (`veach.input.gamepads()`). Buttons are keys (`gamepad_south`, `gamepad_east`, `gamepad_west`,
  `gamepad_north`, `gamepad_back`, `gamepad_guide`, `gamepad_start`, `gamepad_left_stick`,
  `gamepad_right_stick`, `gamepad_left_shoulder`, `gamepad_right_shoulder`, `gamepad_dpad_*`). Any connected
  pad drives the one input map. When the last pad leaves, its axes read 0 and its held buttons are released.
- **Mouse capture.** A `RelativeMouse { enabled }` component on a window entity puts the OS window in relative
  mouse mode (cursor hidden and kept in the window, motion as deltas); removing it or `enabled = false` frees
  the cursor. Headless runs record the state in `WindowPlatformState.relative_mouse` and do nothing else.
- `veach.input.axis(action) -> number` reads an analog action; an unknown name raises `input.unknown_action`
  with a suggestion (and says so when the name is a key action).
- MCP `send_input { axis, value }` moves an axis (relative axes add `value`, absolute ones take it), and
  `send_input { gamepad }` connects (`true`) or disconnects a gamepad. Both are platform events like keys, so
  they are recorded and replayed.

## Acceptance criteria

- **input.AC1**: A project's `"input"` creates one action entity per entry with its keys. `"spcae"` fails with
  `input.unknown_key` and `Did you mean 'space'?`. Three keys for one action fail with `data.type_mismatch`.
- **input.AC2**: With `jump` bound to `space`, an injected key-down makes `pressed("jump")` and `down("jump")`
  true in that frame's `Input` phase. The next frame has `down` without `pressed`. Key-up gives `released`.
  `axis` combines two actions.
- **input.AC3**: `veach.input.down("jmp")` raises `input.unknown_action` with `Did you mean 'jump'?`.
- **input.AC4**: An event with `Key::unknown` does not make actions whose second key is unbound read as held.
- **input.AC5**: Over MCP only, the lab's dev fly camera (`scripts/dev/fly_camera.luau`, run after the scene
  `scripts/triangle_fly.luau`) moves forward 1 m after holding `w` for 30 frames (2 m/s), and turns 0.75 rad
  after holding `left` for 30 frames.
- **input.AC6**: A project's `"dev_scripts"` run after the startup script in every run. The lab's dev fly camera
  flies the active camera of any scene along that camera's own forward, and writes nothing without input: a
  scene's camera is exactly where the scene put it after 30 frames, so goldens, benchmarks and replays keep their
  view. `"dev_scripts"` that is not an array of paths fails with `data.type_mismatch`.

- **input.AC7**: A project's `"axes"` creates one `InputAxis` entity per entry. `"source": "mouse_xx"` fails
  with `input.unknown_axis` and `Did you mean 'mouse_x'?`; `"positive": "spcae"` with `input.unknown_key`;
  `"sensitivty"` with `data.unknown_field`; `"dead_zone": 1.0` and an array entry with `data.type_mismatch`.
- **input.AC8**: With a 0.2 dead zone, a stick at 0.19 and −0.2 reads 0, at 0.6 reads 0.5 and at −1 reads −1;
  the positive key alone reads exactly 1, both keys 0. Mouse motion adds up (10 + 6 = 16 points), sensitivity
  scales it (0.25 → 4), `invert` negates it, and a dead zone on it subtracts without rescaling.
- **input.AC9**: `quantize_axis` maps 1 to 65536 and 0.1 to 6554, rounds halves away from zero, saturates and
  maps NaN to 0; an axis value with any sensitivity lies on the 1/65536 grid.
- **input.AC10**: 10 + 6 points of injected mouse motion with sensitivity 0.5 read 8 in that frame's `Input`
  phase and its fixed step, and 0 in the next frame; a stick stays where it was put across frames;
  `consume_relative_axes` clears relative axes and the values bound to them.
- **input.AC11**: An injected gamepad connection makes `Input.gamepads` and `veach.input.gamepads()` 1, and its
  `gamepad_south` drives an action bound to it; the disconnection makes them 0, releases the button and centers
  the stick.
- **input.AC12**: `veach.input.axis("lookx")` raises `input.unknown_action` with `Did you mean 'look_x'?`;
  `axis("look")` on a key action says it is a key action.
- **input.AC13**: `RelativeMouse` on the primary window turns relative mouse mode on, `enabled = false` or
  removing it turns it off (`WindowPlatformState.relative_mouse`).
- **input.AC14**: A recording of stick and mouse input stores the quantized integers, and its replay reads the
  same axis values, bit for bit, with no `replay.diverged`.
- **input.AC15**: Over MCP only, the lab's fly camera ignores mouse motion until the right button is held;
  holding it adds `RelativeMouse` to the window, 100 points of `mouse_x` then turn the camera 0.25 rad right
  once (not again in later frames), and releasing it removes `RelativeMouse`. A connected gamepad's left stick
  at 0.575 (dead zone 0.15) flies 0.5 m in 30 frames along the turned forward. `send_input` rejects
  `"mouse_xx"` with `input.unknown_axis` and a suggestion.
- **input.AC16**: That MCP session, recorded, stores axis events as integers (0.575 as 37683) and replays
  headless to the same camera with every frame's state matching.
- **input.AC17**: An action or axis bound from C++ inside a module's scope (`bind_action` and `bind_axis` called
  from a module constructor) lives at `veach::actions::<name>`, and `action_down` and `axis_value` read it.

Tests: `[input.AC1]`–`[input.AC14]` and `[input.AC17]` in `tests/input_tests.cpp`; AC5 and AC6 in `mcp: fly camera through
send_input (null RHI)`; AC15 and AC16 in `mcp: analog axes through send_input, recorded and replayed (null RHI)`
(`tests/mcp/test_mcp_lab.py`).

## Out of scope

- Text input, rebinding at run time from scripts, per-player maps (one gamepad per player), gamepad rumble,
  gyro and touchpads, radial (two-axis) dead zones and response curves: when a game needs them.
- Tests drive gamepads and the mouse through injected events: CI and the push gate have no gamepad, and relative
  mouse mode needs a window and a person. The SDL translation (axes, buttons, connect and disconnect, relative
  mode) needs a check by hand with a real gamepad and the lab's fly camera (`--window`).

## Links

- Specs: [platform-and-windows](../platform-and-windows/spec.md), [mcp-server](../mcp-server/spec.md),
  [transforms](../transforms/spec.md)
- Backlog: VCH-006, VCH-101, VCH-036
