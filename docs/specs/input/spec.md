# Input actions in Luau

**Status:** Implemented
**Area:** script
**Owner:** dev lead

## Summary

Games read **actions**, never keys. `veach.project.json` binds each action to one or two keys or mouse buttons.
Scripts ask `veach.input.down("jump")`, `pressed`, `released` and `axis(negative, positive)`. Tests and agents
inject key events at the platform level (`send_input` over MCP), so the whole path from key to action to script
runs headless. The lab's fly camera is a script built on it.

## Motivation

Gameplay needs input, and agents need to drive it without a keyboard. Binding through data keeps scripts
rebindable and lets tools list every action. Backlog: VCH-006.

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

## Out of scope

- Mouse motion and gamepads (analog axes), text input, rebinding at run time from scripts, and per-player
  maps: when a game needs them.

## Links

- Specs: [platform-and-windows](../platform-and-windows/spec.md), [mcp-server](../mcp-server/spec.md),
  [transforms](../transforms/spec.md)
- Backlog: VCH-006
