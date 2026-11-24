# 0014: Engine lifecycle and phases

Date: 2026-11-24

## Status

Accepted

## Context

The engine owns the main loop. User code never calls `progress()`, never creates a world, and never creates pipelines or threads ([ARCHITECTURE.md](../ARCHITECTURE.md)). Subsystems depend on each other in a fixed order and must be torn down in reverse. Simulation must be deterministic. Headless runs must verify work in a fixed number of frames. Spec: [engine-lifecycle](../specs/engine-lifecycle/spec.md), [ecs-and-phases](../specs/ecs-and-phases/spec.md).

## Decision

**Config before init.** `resolve_config` parses the flags first, then `veach.project.json`, then applies CLI overrides; CLI wins. This only reads files. The project file is parsed with a temporary reflection world (`register_config_types`), so config errors have the same did-you-mean quality as any data file.

**Init order** (`Engine::init`): core → platform → ecs → assets → script → rhi → render → modules.

| Step | Does |
|---|---|
| core | Log configuration, error counter |
| platform | Platform backend |
| ecs | World, `CoreModule`, phases, `PlatformModule`, `Time`/`Random` singletons, primary window entity plus `sync_windows` |
| assets | `AssetServer` |
| script | `ScriptRuntime` and the `luau-script` loader |
| rhi | Device, plus a surface if the primary window has a native handle |
| render | Renderer and `RenderModule` |
| modules | User modules in `import<T>()` call order, then `check_reflection` |

`render` is a step of its own, after `rhi`; that refines the original sketch. Each step logs `init <step> (<detail>)`. After init, `start()` loads the startup script and runs the Startup phase.

**Shutdown** is the exact reverse. Each completed step pushes its teardown onto a stack, and `shutdown()` pops it, logging `shutdown <step>`. A failed init step unwinds only the steps that completed. The Shutdown phase runs before teardown. `~Engine` calls `shutdown()`, so cleanup is RAII.

**Phases:**

| Phase | Cadence |
|---|---|
| Startup | once, before the first frame |
| Input | per frame |
| Simulation → PostSimulation → TransformPropagation | per fixed step (1/60 s) |
| PreExtract → Extract → Render | per frame |
| Shutdown | once, before teardown |

- Phase tags are `veach::phase::*` and are deliberately **not** `flecs::Phase` entities, so Flecs's builtin pipeline never runs user systems.
- `PhaseRunner` creates one Flecs pipeline per phase (`System` plus the phase tag, ordered by declaration).
- Each frame the engine calls `frame_begin` → `run_pipeline` per phase → `frame_end`. With `--rest` it also runs the builtin pipeline.

**Time and determinism.**

- With `headless` and `frames > 0`, every frame runs exactly one fixed step, unpaced.
- Windowed runs use `FixedTimestep`: an accumulator of real time, at most 8 steps per frame, and the backlog dropped after a stall. The loop sleeps to 60 Hz until a swapchain provides pacing.
- The `Time` singleton holds `frame` (1-based), `fixed_step`, `delta` and `elapsed`. `delta` is the fixed step in simulation phases.
- `Random` is seeded from `--seed`. Simulation never reads wall-clock time.

**Exit:**

- The run stops on `--frames N`, `world.quit()` (primary window closed, OS quit) or SIGINT, always with a clean shutdown.
- **Exit code 0 if and only if nothing was logged at error level.** Init and start failures return 1.

## Consequences

- Agents verify work with `--headless --frames N` and the exit code.
- Every subsystem has one owner and a defined lifetime. Services are revoked in reverse order ([0015](0015-veach-modules-and-services.md)).
- Splitting the frame into per-phase pipelines costs a little overhead per phase. That is negligible next to the explicitness, and it gives one Tracy zone per phase.
- Render interpolation (`FixedTimestep::alpha`) exists but is not used yet.
- Flecs threading must later be hooked into our own job system (backlog).

## Alternatives considered

- **`world.progress()` with Flecs phases.** Can't run fixed-step phases a variable number of times per frame, and puts the loop in Flecs's hands.
- **One custom pipeline with phase grouping.** Needs Flecs pipeline tricks to split fixed-step and per-frame phases; per-phase pipelines are simpler.
- **Variable timestep simulation.** Breaks determinism and replay.
- **Exit code only on fatal errors.** Hides script errors from CI and agents.
