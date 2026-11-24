# Veach vision

Veach is an **AI-native C++ game engine with a raytracing-first renderer for real-time games**. It is named
after Eric Veach, whose thesis underlies modern path tracing.

Changing anything in this file is a product decision: the product owner signs off.

## What Veach is

- **A game engine, not a renderer for film.** Global illumination, raytraced reflections, screen-space
  reflections as a hybrid or fallback, volumetric fog, physical sky and atmosphere, soft shadows from area
  lights, denoising and upscaling all have to look as good as possible *within real-time frame budgets*.
  Performance is a feature. Offline or reference-quality rendering exists only as a debugging tool.
- **AI-native.** Agents are first-class users. They write the code, drive the running engine, inspect it and
  verify their own work. Humans and agents use **the same API**; there is no separate AI layer. Everything
  that makes the engine good for agents (text formats, reflection, strict types, actionable errors, headless
  runs, structured logs, measurable performance) also makes it pleasant for humans.
- **Pleasant to write by hand.** One obvious way to do each thing, explicit over magic, consistent verbs
  (`spawn/despawn`, `get/set/has/remove`, `on/emit`), strict types by default.
- **Customizable and extensible to the core.** Engine features are modules, and so is the game. Users extend
  the parts they care about instead of forking. The model is Bevy's `ExtendedMaterial`: in Veach, materials are
  Slang modules implementing interfaces with default implementations, so a user overrides exactly
  `evalAlbedo`, `evalEmission` or `displaceVertex` and nothing else. Render passes, extraction and components
  are extensible in the same spirit.

## AI-native principles

1. **Text everywhere.** Scenes and prefabs are Flecs Script; configs, metadata and materials are JSON with
   schemas generated from reflection. Logs are JSON lines for machines and readable lines for humans.
2. **Everything is inspectable.** Every component is reflected, so it is printable, serializable, and visible
   to tools, scripts, schemas and the MCP server.
3. **Strict by default.** Luau `--!strict`, with types generated from the engine's reflection; JSON validated
   against generated schemas.
4. **Errors tell you the fix.** Stable codes, a concrete message and a hint (`Unknown component 'Tranform' —
   did you mean 'Transform'?`).
5. **Headless first.** Every app runs with no window and no GPU for a fixed number of frames, deterministically.
   That is how agents and CI verify work.
6. **Measured, not claimed.** Tracy is in from day one; performance claims cite a capture.
7. **Transactional by default.** Script and tool writes go through a command buffer and apply all-or-nothing.

## Rendering goals

- Hardware raytracing is the baseline, not an add-on: DXR 1.1-class GPUs on Windows, Apple M3 or newer on
  Mac. M1/M2 may run but are not optimized for.
- Initial target: **60 fps at 1440p output** (with upscaling) on current mid/high-end RT GPUs. Refined with
  real measurements.
- **Tessera**: virtualized geometry (our Nanite). Cluster hierarchies, a visibility buffer, streaming.
- **Candela**: real-time global illumination (our Lumen). Raytraced and denoised.
- Plus RT reflections with SSR fallback, physical sky and atmosphere, volumetric fog, area-light soft shadows,
  a shared denoiser and a temporal upscaler.

## Roadmap

In rough order (details and ids in [backlog/BACKLOG.md](backlog/BACKLOG.md)):

1. Foundation (done): lifecycle, ECS and phases, Luau API v0, assets with stable ids, platform layer, RHI seam,
   extraction boundary, CI on macOS and Windows.
2. First light: Metal RHI resources and swapchain, render graph, Slang pipeline, golden-image tests, cameras.
3. Agents in the loop: MCP server (`query_entities`, `get/set_component`, `step_frames`, `screenshot`,
   `gpu_timings`, `load_scene`, sandboxed transactional `eval`), hot reload, record/replay.
4. Content: glTF (fastgltf, meshoptimizer), KTX2/BC7 textures, Flecs Script scenes and prefabs, extendable
   materials.
5. The renderer: Tessera, Candela, RT reflections, sky and atmosphere, volumetric fog, area lights, denoiser,
   upscaler. DX12 backend alongside Metal.
6. The rest of a game engine: physics with Box3D (deterministic, MIT), audio, UI, an editor built on the same
   reflection and MCP machinery.

## Non-goals

- Offline, film or video rendering as a product. A reference path tracer may exist for debugging.
- Non-RT hardware as a target. No legacy rendering paths.
- Platforms beyond macOS (Apple Silicon) and Windows for now: no Linux, mobile, web or consoles.
- A "Bevy clone" plugin/app model. Veach is built on Flecs modules and Flecs idioms.
- Multiple scripting languages, or a custom shader language. Luau and Slang, directly.
- An AI layer separate from the human API.
