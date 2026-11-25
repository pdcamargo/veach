# Platform and windows

**Status:** Implemented
**Area:** platform
**Owner:** dev lead

## Summary

A thin platform layer with two backends: SDL3, for windows and input on macOS and Windows, and headless, with no
SDL and no windows. Windows are entities with a reflected `veach::Window` component, kept in sync with the OS
window. OS events become Veach input state and named actions. The RHI gets a plain `NativeWindowHandle`.

## Motivation

Tools, scripts and MCP must be able to control windows with ordinary component writes, and agents must be able
to run everything with no display. SDL must stay replaceable and must not leak into gameplay or the RHI.

## Requirements

- MUST keep SDL types inside `engine/platform/src/sdl_platform.cpp`; SDL is linked PRIVATE to `veach_platform`.
- MUST create, update and destroy OS windows to match `Window` components (title, size, mode). Changes are
  applied in the Input phase, and at init for the primary window.
- MUST write OS-initiated resizes back into `Window` without echoing them to the OS.
- MUST quit the engine when the primary window is closed or the OS asks to quit. Other windows close by
  despawning their entity.
- MUST translate key events into the `Input` singleton (down, pressed, released) and `InputAction` entities.
- MUST provide a `NativeWindowHandle` (a CAMetalLayer on macOS, an HWND/HINSTANCE on Windows) for the RHI, and log
  at debug level that it was valid when the surface is created.
- MUST NOT initialize SDL at all in headless mode.

## API sketch

```cpp
world.entity("primary_window").set<veach::Window>({.title = "Game", .width = 1920, .height = 1080})
     .add<veach::PrimaryWindow>();                      // the engine does this from veach.project.json

veach::bind_action(world, "jump", veach::Key::space, veach::Key::w);
if (veach::action_pressed(world, "jump")) { ... }
```

```lua
-- Luau: the same component
local Window = veach.components.Window
```

## Acceptance criteria

- **platform.AC1** — Setting or changing `Window` creates or updates the OS window; unchanged components cause no
  OS calls; deleting the entity destroys the window.
- **platform.AC2** — Closing the primary window quits; OS resizes update `Window` without an echo.
- **platform.AC3** — Key events produce pressed, down and released for bound actions, for exactly the right frames.
- **platform.AC4** — Manual check, macOS (no display in CI): `veach-lab --window --log-level debug` opens a
  1280×720 "Veach Lab" window and logs `native window handle is valid: CAMetalLayer=...`. Verified 2026-11-24
  on an Apple M5 Max, also with `--rhi metal`. A 214 s windowed run stopped by SIGINT shut down cleanly with
  exit 0, after 12,870 fixed steps (60.0 Hz). Clicking the title-bar close button logs
  `primary window closed, quitting`, runs the Shutdown phase, shuts down in reverse order and exits 0.
  Verified the same day with desktop automation. SDL also sends its quit event when the last window closes,
  which is harmless: both paths call `world.quit()`.
- **platform.AC5** — `--headless` runs use the headless backend (`init platform (headless)`) and never call SDL.

## Out of scope

- Gamepads (VCH-036), exclusive fullscreen and DPI fields (VCH-037), Luau input API (VCH-006), file watching for
  hot reload (VCH-004). Windows verification (hosted CI only, ADR 0030) is VCH-002.

## Open questions

None.

## Links

- ADRs: [0008 platform](../../adr/0008-platform-sdl3.md), [0009 RHI](../../adr/0009-rhi-design.md)
- Code: `engine/platform/`
