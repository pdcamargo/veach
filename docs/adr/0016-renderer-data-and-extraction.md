# 0016: Renderer data and extraction

Date: 2026-11-24

## Status

Accepted

## Context

Renderer data has different lifetimes, layouts and threading needs from gameplay data. Examples are BVHs, GPU buffers, light structures, ReSTIR reservoirs and denoiser history. Storing it in ECS components would expose GPU handles to gameplay, couple the renderer to ECS iteration, and block running the renderer on another thread later.

## Decision

- **Renderer data lives outside the ECS**, in `render::RenderWorld`, owned by `render::Renderer` (`engine/render`). In v0, `RenderWorld` holds only `frame` and `window_count`, as placeholders until views and cameras exist.
- **Extraction is the only crossing point.** During the Extract phase, `Renderer::extract(world)` clears `RenderWorld` and runs every registered extractor in registration order. An extractor reads components and writes `RenderWorld`.
- The renderer never reads the ECS outside extraction. Gameplay never sees GPU handles.
- **Extension:** modules add extractors with `Renderer::add_extractor(name, fn)` (through `services(world).get<render::Renderer>()`) rather than reaching into the renderer.
- `RenderModule` registers the `Extract` and `Render` phase systems. `Renderer::render()` is a no-op until the render graph lands (VCH-010).
- Gameplay will interact with rendering only through reflected components such as `Mesh`, `Material`, `Light` and `Camera`, none of which exist yet. They describe intent, not GPU state.

## Consequences

- The ECS stays a pure description of the game, fully reflected and inspectable.
- Pipelining the renderer one frame behind the simulation is possible later: only extraction needs to synchronize.
- Each feature needs an extractor that copies data. That cost is per frame. Change detection is a backlog optimization, to be measured with Tracy first.
- Debugging "why isn't X rendered" means looking at extraction. A tool exposing `RenderWorld` is a natural MCP addition.

## Alternatives considered

- **GPU handles in components.** Convenient, but it couples gameplay to the RHI and breaks the "gameplay never touches renderer internals" rule. Rejected.
- **The renderer querying the ECS directly during Render.** No clear boundary, and threading becomes impossible.
- **A fixed extraction list in the renderer.** Not extensible by modules; violates the extensibility values in [VISION.md](../VISION.md).
