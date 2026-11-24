# Engine lifecycle

**Status:** Implemented
**Area:** runtime
**Owner:** dev lead

## Summary

`veach::Engine` owns the process: it resolves configuration, initializes subsystems in a fixed order, runs the
main loop and shuts everything down in exact reverse order. Every app, test and tool goes through it.

## Motivation

Agents and humans must be able to run any app the same way, headless or windowed, for a fixed number of frames,
and trust that a clean exit means a clean run. A single owner of the loop is also what makes determinism,
record/replay (VCH-014) and the MCP server (VCH-003) possible.

## Requirements

- MUST initialize in this order: core (log) → platform → ecs → assets → script → rhi → render → modules.
- MUST shut down in exact reverse order of the steps that completed, including after a failed init step.
- MUST log every init and shutdown step (category `engine`, `init <step> (<detail>)` / `shutdown <step>`).
- MUST resolve configuration before init: CLI flags override `veach.project.json`, which overrides defaults.
- MUST accept the core flags in every app: `--headless`, `--window`, `--frames N`, `--seed N`,
  `--log-format human|json`, `--project <path>`, plus `--log-level`, `--script`, `--rhi`, `--rest`, `--help`.
- MUST reject unknown flags with a suggestion, and invalid or conflicting values with the accepted ones.
- MUST run exactly one fixed simulation step per frame in headless runs, unpaced, so runs are deterministic.
- MUST run the Startup phase once before the first frame and the Shutdown phase once before teardown.
- MUST exit 0 if and only if nothing was logged at error level during the run (init and script errors included).
- MUST shut down cleanly on window close, `--frames` reached, `world.quit()` and SIGINT.
- SHOULD finish init in well under 100 ms headless (it is ~10 ms today) so agent loops stay fast.
- MUST NOT let user code create worlds, call `progress()`, or create pipelines or threads.

## API sketch

```cpp
int main(int argc, char** argv) {
    veach::Engine engine({.name = "my-game", .default_project = "path/to/project"}, argc, argv);
    engine.import<MyGameModule>();   // imported during the 'modules' step
    return engine.run();             // init + start + frames + shutdown; returns the exit code
}

// Tests and tools:
veach::Engine engine(config);        // EngineConfig, no argv
engine.init(); engine.start(); while (engine.frame()) {} engine.shutdown();
```

```json
// veach.project.json — schema: schemas/veach.project.schema.json (generated)
{ "name": "veach-lab", "script": "scripts/hello.luau", "window": { "title": "Veach Lab", "width": 1280, "height": 720 } }
```

Frame: `Input` → N × (`Simulation` → `PostSimulation` → `TransformPropagation`) → `PreExtract` → `Extract` →
`Render`. N is 1 headless, and the fixed-timestep accumulator (60 Hz, max 8) otherwise.

## Acceptance criteria

- **lifecycle.AC1** — Init steps run in the documented order and shutdown steps in exact reverse; Startup and
  Shutdown phases each run once.
- **lifecycle.AC2** — When an init step fails, the run exits 1 with `engine.init_failed` and only the completed
  steps are shut down, in reverse.
- **lifecycle.AC3** — A headless run of N frames performs exactly N fixed steps; `Time.elapsed == N/60`.
- **lifecycle.AC4** — CLI flags override the project file, which overrides defaults.
- **lifecycle.AC5** — Unknown flags, invalid values, conflicting flags and unknown project-file fields produce
  errors with stable codes and a fix (did-you-mean where applicable).
- **lifecycle.AC6** — `veach-lab --script scripts/hello.luau --frames 3 --headless` prints
  `Hello from Luau — frame 1..3`, shuts down cleanly and exits 0 (CTest: `lab: hello world ...`).
- **lifecycle.AC7** — `--log-format json` prints one JSON object per line (CTest: `lab: json logs`).

## Out of scope

- Record/replay (VCH-014), threads and a job system (VCH-015), hot reload (VCH-004).
- Frame pacing by a real swapchain: until VCH-010, windowed runs sleep to 60 Hz.

## Open questions

None.

## Links

- ADRs: [0014 lifecycle and phases](../../adr/0014-engine-lifecycle-and-phases.md),
  [0015 modules and services](../../adr/0015-veach-modules-and-services.md),
  [0021 logging and errors](../../adr/0021-structured-logging-and-errors.md)
- Backlog: VCH-001 (done), VCH-014, VCH-015
- Code: `engine/runtime/`
