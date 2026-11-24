# 0011: Profiling with Tracy

Date: 2026-11-24

## Status

Accepted

## Context

Performance is a feature ([VISION.md](../VISION.md)). Claims about performance, from humans or agents, must be checked against measurements, not intuition. Agents work headlessly, so captures must be possible without a GUI and readable as text. Instrumentation must cost nothing when profiling is off.

## Decision

**Tracy v0.15.0**, client only, fetched only when `VEACH_TRACY=ON` (the `profile` preset: RelWithDebInfo plus Tracy).

- The client is built with `TRACY_ENABLE`, `TRACY_STATIC`, `TRACY_ONLY_LOCALHOST` and `TRACY_NO_BROADCAST`, so it never listens on external interfaces.
- `veach/core/profile.hpp` defines `VEACH_ZONE(name)`, `VEACH_ZONE_TEXT(name, text, size)` and `VEACH_FRAME_MARK()`. They map to Tracy when `VEACH_TRACY` is defined and to nothing otherwise. Engine code never includes Tracy directly.

**Zones in place from day one:**

- `Engine::init`, each init step (named), `Engine::start`, `Engine::shutdown`;
- `frame` and `fixed step`, plus a frame mark per frame;
- every phase run (`phase`, with the phase name as text);
- script execution (`script.execute`, the chunk name) and every script system run (`script.system`, the system name);
- `AssetServer::load`, `Renderer::extract` (and each extractor), `Renderer::render`;
- `PollPlatform` and `SyncWindows`.

**Headless capture**, detailed in [guides/profiling.md](../guides/profiling.md):

1. Start `tracy-capture -o trace.tracy` (it waits for a client).
2. Run the app with `TRACY_NO_EXIT=1` so a short `--frames N` run flushes its data before exiting.
3. Run `tracy-csvexport trace.tracy` to get per-zone statistics as CSV text.

The tools must be the same Tracy version as the client (0.15.0).

**Rule:** a performance claim in a PR, ADR, spec or commit message cites a trace, or the numbers from one.

## Consequences

- Default builds carry zero profiling overhead and don't fetch Tracy.
- Agents can verify "this got faster" from CSV output.
- Zones with runtime names use `ZoneText`, which copies the string per zone. Acceptable at current zone counts; watch it in hot loops.
- GPU zones come with the real RHI (backlog).

## Alternatives considered

- **Superluminal / Instruments / PIX only.** Platform-specific and GUI-centric, with no headless text export usable by agents. Still fine as complements.
- **Optick.** Less maintained, weaker macOS support.
- **Our own instrumentation.** We would rebuild Tracy's viewer, protocol and capture tooling.
