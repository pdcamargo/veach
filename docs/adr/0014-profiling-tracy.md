# 0014: Profiling with Tracy

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0011).

## Context

Performance claims, from humans or agents, must be checked against measurements. Agents work headlessly, so
captures must be possible without a GUI and readable as text. Instrumentation must cost nothing when off. Guide:
[profiling](../guides/profiling.md).

## Decision

**Tracy v0.15.0**, client only, fetched only when `VEACH_TRACY=ON` (the `profile` preset: RelWithDebInfo plus Tracy).

- Built with `TRACY_ENABLE`, `TRACY_STATIC`, `TRACY_ONLY_LOCALHOST` and `TRACY_NO_BROADCAST`, so it never listens on
  external interfaces.
- `veach/core/profile.hpp` defines `VEACH_ZONE(name)`, `VEACH_ZONE_TEXT(name, text, size)` and `VEACH_FRAME_MARK()`,
  mapping to Tracy under `VEACH_TRACY` and to nothing otherwise. Engine code never includes Tracy directly.

**Zones:** `Engine::init`, each init step, `Engine::start`, `Engine::shutdown`; `frame` and `fixed step` plus a frame
mark; every phase run (`phase`, name as text); `script.execute` and `script.system`; `AssetServer::load`,
`Renderer::extract` (and each extractor), `Renderer::render`; `PollPlatform` and `SyncWindows`.

**Headless capture:**

1. Start the app with `--wait-for-profiler`: in profile builds it blocks in init (up to 30 s) until a capture
   connects, and lingers 500 ms at exit so the server can fetch zone names.
2. Run `tracy-capture -o trace.tracy -f` (it only connects to a client already listening).
3. Run `tracy-csvexport trace.tracy` for per-zone statistics as CSV.

The tools must be the same Tracy version as the client.

**GPU time** comes from the RHI's per-pass timestamps (`Device::last_frame_timings()`, MCP `gpu_timings`), not Tracy.

**Rule:** a performance claim in an ADR, spec or commit message cites a trace or the numbers from one.

## Consequences

- Default builds carry zero profiling overhead and don't fetch Tracy.
- Agents can verify "this got faster" from CSV output.
- Zones with runtime names copy the string per zone; watch it in hot loops.

## Alternatives considered

- **Superluminal / Instruments / PIX only:** platform-specific and GUI-centric, no headless text export. Fine as
  complements.
- **Optick:** less maintained, weaker on macOS.
- **Our own instrumentation:** we would rebuild Tracy's viewer, protocol and capture tooling.
