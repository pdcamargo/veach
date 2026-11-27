# 0020: Renderer data and the extraction boundary

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0016).

## Context

Renderer data (GPU buffers, textures, acceleration structures, render targets and their history) has different
lifetimes, layouts and threading needs from gameplay data. Storing it in components would expose GPU handles to
gameplay, couple the renderer to ECS iteration, and block running the renderer on another thread later.

## Decision

- **Renderer data lives outside the ECS**, in `render::RenderWorld`, owned by `render::Renderer` (`engine/render`).
  `RenderWorld` holds the frame's snapshot: the views (one per active `Camera`, [render-views](../specs/render-views/spec.md)),
  the mesh draws, the lights, the resolved sky, the target size, and per-type data that modules extract
  (`RenderWorld::emplace<T>` / `get<T>`).
- **Extraction is the only crossing point.** During the Extract phase, `Renderer::extract(world)` clears `RenderWorld`
  and runs every registered extractor in registration order. An extractor reads components and writes `RenderWorld`.
- The renderer never reads the ECS outside extraction. Gameplay never sees GPU handles or `RenderWorld`.
- **Extension:** modules add extractors with `Renderer::add_extractor(name, fn)` and render features with
  `Renderer::add_feature(...)`, through `services(world).get<render::Renderer>()`, rather than reaching into the
  renderer.
- `RenderModule` registers the Extract and Render phase systems.
- Gameplay interacts with rendering only through reflected components (`MeshInstance`, `Camera`, lights, `Sky`,
  `PhotoMode`, ...). They describe intent, not GPU state.

## Consequences

- The ECS stays a pure description of the game, fully reflected and inspectable.
- Pipelining the renderer a frame behind the simulation stays possible: only extraction synchronizes.
- Each feature needs an extractor that copies data, per frame. Change detection is an optimization to measure first.
- Debugging "why isn't X rendered" means looking at extraction; MCP's `render_graph_plan` shows what the renderer
  made of it.
- Planet-scale features (terrain, vegetation, atmosphere, ocean) plug in the same way: components in the world, an
  extractor, a render feature.

## Alternatives considered

- **GPU handles in components:** couples gameplay to the RHI.
- **The renderer querying the ECS during Render:** no clear boundary, and threading becomes impossible.
- **A fixed extraction list in the renderer:** not extensible by modules.
