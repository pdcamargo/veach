# 0018: Engine lifecycle and phases

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0014).

## Context

The engine owns the main loop: user code never calls `progress()`, never creates a world, never creates pipelines or
threads ([ARCHITECTURE.md](../ARCHITECTURE.md)). Subsystems depend on each other in a fixed order and must be torn
down in reverse. Simulation must be deterministic, and headless runs must verify work in a fixed number of frames.
Specs: [engine-lifecycle](../specs/engine-lifecycle/spec.md), [ecs-and-phases](../specs/ecs-and-phases/spec.md).

## Decision

**Config before init.** `resolve_config` parses the flags, then `veach.project.json`, then applies CLI overrides (CLI
wins). It only reads files. The project file is parsed with a temporary reflection world (`register_config_types`),
so config errors have the same did-you-mean quality as any data file.

**Init order** (`Engine::init`): core → platform → ecs → assets → script → rhi → render → modules.

| Step | Does |
|---|---|
| core | Log configuration, error counter |
| platform | Platform backend |
| ecs | World, `CoreModule`, phases, `PlatformModule`, `Time`/`Random`, primary window entity plus `sync_windows` |
| assets | `AssetServer` |
| script | `ScriptRuntime` and the `luau-script` loader |
| rhi | Device, plus a surface if the primary window has a native handle |
| render | Renderer and `RenderModule` |
| modules | User modules in `import<T>()` order, then `check_reflection` |

Each step logs `init <step> (<detail>)`. After init, `start()` loads the startup script and runs Startup.

**Shutdown** is the exact reverse: each completed step pushes its teardown onto a stack, `shutdown()` pops it
(logging `shutdown <step>`), a failed step unwinds only what completed, the Shutdown phase runs before teardown, and
`~Engine` calls `shutdown()`.

**Phases:**

| Phase | Cadence |
|---|---|
| Startup | once, before the first frame |
| Input | per frame |
| Simulation → PostSimulation → TransformPropagation | per fixed step (1/60 s) |
| PreExtract → Extract → Render | per frame |
| Shutdown | once, before teardown |

- Phase tags are `veach::phase::*` and deliberately **not** `flecs::Phase`, so Flecs's builtin pipeline never runs
  user systems.
- `PhaseRunner` creates one Flecs pipeline per phase (`System` plus the tag, ordered by declaration). Each frame the
  engine calls `frame_begin` → `run_pipeline` per phase → `frame_end`; with `--rest` it also runs the builtin pipeline.

**Time and determinism.**

- `--headless --frames N`: every frame runs exactly one fixed step, unpaced.
- Windowed: `FixedTimestep`, an accumulator of real time, at most 8 steps per frame, backlog dropped after a stall;
  the swapchain paces frames, or the loop sleeps to 60 Hz without one.
- The `Time` singleton holds `frame` (1-based), `fixed_step`, `delta`, `elapsed`; `delta` is the fixed step in
  simulation phases. `Random` is seeded from `--seed`. Simulation never reads wall-clock time.
- `--record` / `--replay` repeat a run exactly ([spec](../specs/record-replay/spec.md)).

**Exit:** on `--frames N`, `world.quit()` (primary window closed, OS quit) or SIGINT, always with a clean shutdown.
**Exit code 0 if and only if nothing was logged at error level**; init and start failures return 1.

## Consequences

- Agents verify work with `--headless --frames N` and the exit code.
- Every subsystem has one owner and a defined lifetime; services are revoked in reverse ([0019](0019-veach-modules-and-services.md)).
- One pipeline per phase costs a little per phase and gives one Tracy zone per phase.
- Render interpolation (`FixedTimestep::alpha`) exists but is unused.
- Flecs threading must later be hooked into our own job system.

## Alternatives considered

- **`world.progress()` with Flecs phases:** can't run fixed-step phases a variable number of times per frame.
- **One custom pipeline with phase grouping:** needs Flecs pipeline tricks; per-phase pipelines are simpler.
- **Variable timestep simulation:** breaks determinism and replay.
- **Exit code only on fatal errors:** hides script errors from CI and agents.
