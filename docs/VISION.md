# Veach vision

Veach is an **AI-native C++ game engine, and the foundation of a reusable toolbox for space games**. It is named
after Eric Veach, whose thesis underlies modern path tracing.

Changing anything in this file is a product decision: the product owner signs off. The current direction was set at
the reset ([ADR 0001](adr/0001-reset.md)); the raytracing-first plan before it is preserved in the `pathtracer-era`
git tag.

## What Veach is

- **The engine under a space-game toolbox.** The toolbox (in `space/`, on top of the engine) is in the spirit of No
  Man's Sky: procedural planets and solar systems with orbits; editable volumetric terrain with caves; underground and
  aquatic ecosystems; earth-like and alien planets whose atmosphere (or lack of one) drives the look; volumetric
  clouds and weather; oceans, lakes and rivers; volcanic activity; procedural vegetation, and eventually a nature
  editor; fauna and flora; NPC and faction simulation; base building and light terraforming (flatten, smooth, raise,
  lower); ship building; multiplayer.
- **A toolbox for other people.** Others build their own games with it and extend it: new features as modules, and
  procgen extended through Luau hooks and scripts. The engine never depends on the toolbox; whatever the toolbox needs
  that is generic becomes part of the engine ([ADR 0042](adr/0042-space-toolbox-layering.md)).
- **AI-native.** Agents are first-class users. They write the code, drive the running engine, inspect it and verify
  their own work. Humans and agents use **the same API**; there is no separate AI layer. Everything that makes the
  engine good for agents (text formats, reflection, strict types, actionable errors, headless runs, structured logs,
  measurable performance) also makes it pleasant for humans.
- **Pleasant to write by hand.** One obvious way to do each thing, explicit over magic, consistent verbs
  (`spawn/despawn`, `get/set/has/remove`, `on/emit`), strict types by default.
- **Customizable and extensible to the core.** Engine features are modules, and so is the game. Users extend the parts
  they care about instead of forking. Materials are Slang modules implementing interfaces with default
  implementations, so a user overrides exactly `evalBaseColor` or `evalEmission` and nothing else. Render features,
  extraction and components are extensible in the same spirit.

## AI-native principles

1. **Text everywhere.** Scenes and prefabs are Flecs Script; configs, metadata and materials are JSON with schemas
   generated from reflection. Logs are JSON lines for machines and readable lines for humans.
2. **Everything is inspectable.** Every component is reflected, so it is printable, serializable, and visible to
   tools, scripts, schemas and the MCP server.
3. **Strict by default.** Luau `--!strict`, with types generated from the engine's reflection; JSON validated against
   generated schemas.
4. **Errors tell you the fix.** Stable codes, a concrete message and a hint (`Unknown component 'Tranform' — did you
   mean 'Transform'?`).
5. **Headless first, deterministic.** Every app runs with no window and no GPU for a fixed number of frames,
   deterministically. That is how agents and CI verify work, and what record/replay and procgen from seeds rest on.
6. **Measured, not claimed.** Tracy and per-pass GPU timings are in from day one; performance claims cite a capture.
7. **Transactional by default.** Script and tool writes go through a command buffer and apply all-or-nothing.

## Rendering goals

- **The real-time renderer is a well-made PBR rasterizer** with a stylized-but-not-cartoony look (reference: No Man's
  Sky): physically based materials, photometric lights and exposure, with artistic grading on top. Planet-scale
  features (atmosphere driven by composition, terrain, vegetation, oceans, clouds) plug into it as render features
  ([ADR 0039](adr/0039-realtime-renderer-is-raster-pbr.md)).
- **No real-time GI and no real-time path tracing.** Real-time features never require hardware ray tracing.
- **Photo mode:** the progressive reference path tracer, accumulating a path-traced image while the camera is still.
  No denoiser, no temporal tricks. It is also the engine's ground truth for debugging materials, lights and the sky
  ([ADR 0041](adr/0041-photo-mode.md)).
- **Hardware:** every Apple silicon Mac and DX12 GPUs with Shader Model 6.6; ray tracing optional, for photo mode.
  Design target: 60 fps at 1080p on a base Apple M1-class GPU ([ADR 0003](adr/0003-platforms-and-hardware-target.md)).

## Roadmap

In order (details and ids in [backlog/BACKLOG.md](backlog/BACKLOG.md)):

1. **Foundation (done):** lifecycle, ECS and phases, Luau API v0 with script components and transactional writes,
   assets with stable ids, the platform layer, input actions, transforms, the RHI seam, the extraction boundary, CI on
   macOS and Windows.
2. **First light (done):** Metal and DX12 backends (DX12 verified on WARP), bindless resources, the render graph and
   its barrier model, frames in flight, the Slang pipeline, golden-image tests with FLIP.
3. **Agents in the loop (done):** the MCP server (query and edit the world, step frames, screenshots, render-graph
   plan, per-pass GPU timings, sandboxed transactional `eval`), hot reload, record/replay.
4. **Content (done):** glTF import with cooked meshes, meshlets and BC7/KTX2 textures, extendable Slang materials,
   photometric lights, the physical sky.
5. **Renderers for now (done):** photo mode, and an interim real-time path (a visibility buffer with basic shading)
   so every scene can be looked at in real time.
6. **Research phase (done):** the toolbox's architecture, decided in ADRs 0043-0065 and summarized in
   [ARCHITECTURE.md](ARCHITECTURE.md#toolbox-architecture-decided-in-the-research-phase) and [research/00-synthesis.md](research/00-synthesis.md): large-world
   coordinates and frames, time and orbits, the scale model, universe generation and procgen, terrain, persistence,
   networking, simulation LOD, physics, flight and travel, the real-time renderer, determinism.
7. **Spikes toward the first slice (next):** throwaway prototypes that validate those decisions (VCH-077 to VCH-090),
   then the first slice (VCH-091): a seed-generated star system with a rotating, tilted planet seen from orbit, an
   atmosphere from its composition and the right local time; fly down with no loading screen, land, dig a hole. The
   same code at 1/100 scale and real scale.

## Non-goals

- Real-time global illumination, real-time path tracing, and offline or film rendering as a product.
- Hardware ray tracing as a requirement for anything but photo mode.
- Platforms beyond macOS (Apple silicon) and Windows for now: no Linux, mobile, web or consoles.
- A "Bevy clone" plugin/app model. Veach is built on Flecs modules and Flecs idioms.
- Multiple scripting languages, or a custom shader language. Luau and Slang, directly.
- An AI layer separate from the human API.
- A toolbox the engine depends on: the engine stays usable without it.
