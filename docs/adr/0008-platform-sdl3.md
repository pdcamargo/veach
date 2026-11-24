# 0008: Platform layer on SDL3

Date: 2026-11-24

## Status

Accepted

## Context

We need windows, input and OS events on macOS and Windows without writing Cocoa and Win32 code ourselves, and without letting a windowing library's types spread through the engine. Tools and MCP must be able to control windows through ordinary component writes. CI and agents need a backend with no display.

## Decision

**SDL confinement.**

- SDL 3.4.20 is built static, with the audio, gpu, render and camera subsystems disabled.
- It is linked **PRIVATE** to `veach_platform`. Only `engine/platform/src/sdl_platform.cpp` includes SDL headers; `sdl_platform.hpp` is SDL-free.
- We own `main()`: `SDL_MAIN_HANDLED` plus `SDL_SetMainReady()`.

**Backends.**

- `Platform` is the interface (`veach/platform/platform.hpp`), created by `create_platform(PlatformBackend::sdl | headless)`.
- The headless backend touches no SDL at all. Windows get ids, have no native handle, and produce no events.

**Native handle.**

- The RHI receives only `NativeWindowHandle`: `kind`, `metal_layer`, `hwnd`, `hinstance`, and pixel size.
- On Mac, the window is created with `SDL_WINDOW_METAL`, then `SDL_Metal_CreateView` and `SDL_Metal_GetLayer` give a `CAMetalLayer*`.
- On Windows, `HWND` and `HINSTANCE` come from the window properties.

**Windows are entities.**

- The `veach::Window` component holds `title`, `width`, `height`, `mode` (`windowed` | `fullscreen`) and `vsync`. It is reflected and `Scriptable`.
- The `SyncWindows` system (Input phase) creates OS windows for new `Window` entities. It applies changes by diffing against `WindowPlatformState.applied`.
- OS resizes are written back to `Window` and to `applied`, so they are not echoed back to the OS.
- Removing `Window` or deleting the entity closes the OS window (observer `CloseOsWindow`).
- Closing the window tagged `PrimaryWindow` calls `world.quit()`; closing any other window deletes its entity. An OS quit also calls `world.quit()`.
- `sync_windows(world)` runs the system on demand. The engine uses it so the primary window exists before the RHI init step.

**Input.**

- The `PollPlatform` system (Input phase) translates events.
- Keys are a reflected `veach::Key` enum mapped from SDL **scancodes** (layout-independent).
- The `Input` singleton holds `down`, `pressed` and `released` bitsets.
- Named actions are `InputAction` entities under `veach::actions`, managed with `bind_action`, `action_down`, `action_pressed` and `action_released`.
- Gameplay and scripts never see SDL events.

## Consequences

- Swapping or adding a backend touches one translation unit.
- Window control works the same from C++, scripts, tools and (later) MCP.
- `vsync` is stored but not consumed until a swapchain exists.
- Fullscreen means borderless desktop fullscreen only.
- No gamepad, mouse or text input yet, and no data-driven action bindings or Luau input binding (backlog).

## Alternatives considered

- **GLFW.** Smaller, but weaker input (no robust gamepad/HID) and less active. SDL3 covers future needs (gamepads, haptics, dialogs).
- **Native Cocoa/Win32.** Most control, but double the code and maintenance for no current benefit.
- **SDL types in engine headers.** Convenient, but it couples everything to SDL. Rejected; the private link enforces this.
