# 0012: Platform layer on SDL3

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0008).

## Context

We need windows, input and OS events on macOS and Windows without writing Cocoa and Win32 code, and without letting a
windowing library's types spread through the engine. Tools and MCP must control windows through ordinary component
writes. CI and agents need a backend with no display. Specs: [platform-and-windows](../specs/platform-and-windows/spec.md),
[input](../specs/input/spec.md).

## Decision

**SDL confinement.**

- SDL 3.4.20, built static, with the audio, gpu, render and camera subsystems disabled.
- Linked **PRIVATE** to `veach_platform`. Only `engine/platform/src/sdl_platform.cpp` includes SDL headers;
  `sdl_platform.hpp` is SDL-free.
- We own `main()`: `SDL_MAIN_HANDLED` plus `SDL_SetMainReady()`.

**Backends.** `Platform` (`veach/platform/platform.hpp`) is created by `create_platform(PlatformBackend::sdl |
headless)`. The headless backend touches no SDL: windows get ids, have no native handle, and produce no events.

**Native handle.** The RHI receives only `NativeWindowHandle` (`kind`, `metal_layer`, `hwnd`, `hinstance`, pixel
size). On Mac the window is created with `SDL_WINDOW_METAL`, and `SDL_Metal_CreateView` / `SDL_Metal_GetLayer` give a
`CAMetalLayer*`; on Windows `HWND` and `HINSTANCE` come from the window properties.

**Windows are entities.**

- `veach::Window { title, width, height, mode (windowed | fullscreen), vsync }`, reflected and Scriptable.
- `SyncWindows` (Input phase) creates OS windows for new `Window` entities and applies changes by diffing against
  `WindowPlatformState.applied`. OS resizes are written back to `Window` and `applied`, so they are not echoed.
- Removing `Window` or deleting the entity closes the OS window (observer `CloseOsWindow`). Closing the
  `PrimaryWindow` calls `world.quit()`; closing any other deletes its entity. An OS quit calls `world.quit()`.
- `sync_windows(world)` runs the system on demand; the engine uses it so the primary window exists before the RHI
  init step.

**Input.**

- `PollPlatform` (Input phase) translates events. Keys are a reflected `veach::Key` enum mapped from SDL
  **scancodes** (layout-independent).
- The `Input` singleton holds `down`, `pressed` and `released` bitsets.
- Named actions are `InputAction` entities under `veach::actions` (`bind_action`, `action_down`, `action_pressed`,
  `action_released`), bound from `veach.project.json`.
- Gameplay and scripts never see SDL events.

## Consequences

- Swapping or adding a backend touches one translation unit.
- Window control works the same from C++, scripts, tools and MCP.
- Fullscreen means borderless desktop fullscreen only.
- Gamepads, mouse look and text input are not covered yet.
- SDL's GPU API is not used; why is in [0004](0004-own-rhi-not-sdl-gpu.md).

## Alternatives considered

- **GLFW.** Smaller, but weaker input (no robust gamepad/HID) and less active.
- **Native Cocoa/Win32.** Most control, double the code for no current benefit.
- **SDL types in engine headers.** Couples everything to SDL; the private link prevents it.
