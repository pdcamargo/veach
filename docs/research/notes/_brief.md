# Research brief (shared by every researcher)

This is the owner's brief for the Veach research phase, verbatim, followed by the repository context every
researcher must read. Repository: /Users/patrickdiascamargo/dev/cpp/veach. Date: 2026-11-27.

## Repository context to read before researching

- docs/VISION.md, docs/ARCHITECTURE.md (non-negotiables; "Open questions (research phase)")
- docs/adr/README.md and especially: 0003 (hardware target), 0004 (our RHI), 0005-0007 (Flecs v4, barrel, Luau),
  0013 (conventions: right-handed, +Y up, meters, float32 glm), 0018 (lifecycle, fixed 60 Hz phases, determinism),
  0019 (modules and services), 0020 (extraction boundary), 0029-0030 (render graph sync, frames in flight),
  0035-0038 (materials, lights, exposure, physical sky: Hillaire LUTs, Earth-at-ground-level today),
  0039 (raster PBR, no real-time GI/path tracing), 0040 (interim visibility-buffer path), 0041 (photo mode),
  0042 (space/ toolbox layering: engine never knows the toolbox)
- docs/reference/rhi-capabilities.md (what the GPU layer can and cannot do) and docs/backlog/BACKLOG.md
  (VCH-070 research phase; VCH-071-076 RHI gaps: indirect dispatch/draw, 3D textures/arrays, non-blocking readback,
  async compute, wider atomics, texture updates after creation)
- Known flat-world assumptions in today's code (inventory from the dev lead): the sky's camera altitude is world Y
  (engine/render/src/atmosphere.cpp), zenith is always world +Y, the sun comes from Earth astronomy (time_of_day,
  latitude, day_of_year), the planet radius is fixed Earth values, one Sky per scene, the sky LUTs break above the
  atmosphere top (no planet-from-orbit view); Transform positions are float32; `world_up` is +Y; the sun is one
  directional light; exposure is a fixed manual EV100 (no auto-exposure).

## The owner's brief (verbatim)

# Research phase: the space toolbox architecture

## Goal
Answer the open questions in docs/ARCHITECTURE.md (extended below) well enough to write the toolbox's founding ADRs and plan the first spikes. This is research and design, not implementation: no engine or toolbox code in this task. You are dev lead and architect. Form opinions, make the calls, and record them.

## Inputs you must ground everything in
- docs/VISION.md, docs/ARCHITECTURE.md, the ADRs (especially 0003 hardware target, 0004 RHI, 0039–0042), docs/reference/rhi-capabilities.md and backlog items VCH-071 to VCH-076.
- Veach's existing constraints: Flecs v4 through the barrel, fixed 60 Hz deterministic simulation with veach::Random, the extraction boundary, the render graph, bindless Slang, strict Luau with command-buffer transactions, headless-first, MCP, record/replay, Windows verified only in CI, and the M5 Max as the only machine we can measure on.
- Hardware target: treat ADR 0003's number (60 fps at 1080p on a base M1-class GPU) as unmeasured. For every question, note how the answer changes between a base M1-class GPU and an M3 Pro-class GPU, so the owner can set the real target with that in front of them.

## Product scope (as the owner described it so far)
A reusable, extensible toolbox for space games in the spirit of No Man's Sky, which other people can build games on and extend, including extending procgen through Luau hooks and scripts. Everything in the universe is derived from a seed. So far the owner has described:
- A seed-generated galaxy, including its central supermassive black hole, quasars, nebulae and other deep-space objects, visible and coherent from anywhere in it.
- Solar systems with real-feeling celestial mechanics: orbits, planet rotation, axial tilt and seasons. Day length, year length, time of day and day of the year on each body all follow from its rotation and orbit. Gravity follows from the body's physical properties.
- Planets: earth-like and alien, editable volumetric terrain with caves, underground and aquatic ecosystems, atmospheres (or none) whose composition drives the look, volumetric skies and clouds, weather, oceans, lakes and rivers, volcanic activity including eruptions.
- Procedural vegetation (and eventually a nature editor), fauna and flora, NPC and faction simulation.
- Base building with light terraforming (flatten, smooth, raise, lower), ship building, and multiplayer.
- A universe scale chosen by the creator (see "Scale is a creator setting" below).
- Rendering: well-made PBR, stylized but not cartoony (reference: No Man's Sky). No real-time GI or path tracing. Photo mode already exists.

## This scope is not complete, and it will grow
The list above is how the owner described the idea, not a finished analysis. The owner will want more over time, and some of it nobody has thought of yet. Two consequences:
1. Before answering the questions, do your own gap analysis (see "Scope gap analysis" below) and find what's missing.
2. The architecture must be built to grow. For every major system, name its extension points: where a new kind of celestial body, biome, phenomenon, simulated entity or render feature plugs in, and whether that can happen from Luau, from a C++ module, or only by changing the toolbox itself. Prefer designs where new content kinds are data plus registered generators over designs with hardcoded enums of things.

## Scale is a creator setting, not a constant
Creators must be able to choose the scale of their universe, from real-life scale to a compressed, No Man's Sky-style scale. The owner's working idea, to validate or improve on: a scale factor s. Every body is generated in real physical units (Earth's radius is 6,371 km at s = 1), and s scales celestial lengths only. At s = 0.1, an Earth-like planet has a tenth of the radius, but the generation pipeline still runs on the unscaled physical parameters, so the same seed gives the same planet (composition, climate, biomes, life) at any scale. Surface gravity and other gameplay-relevant quantities can stay at their physical values. Kerbal Space Program is a precedent: about a tenth of Earth's radius with full Earth gravity.
Hold this design to a precise standard. A scale factor does not keep all the math the same; it forces a choice of which quantities stay fixed, and everything else changes as a consequence. Answer explicitly:
- What s applies to: celestial lengths (radii, orbits, atmosphere height, large terrain features) vs human-scale things that must never scale (players, creatures, buildings, props, caves, edit brushes). This implies splitting terrain generation into planetary-scale features that follow s and detail features that always stay in meters.
- Which derived quantities stay fixed and which follow from the scaled sizes: surface gravity, orbital speed and period (which on-rails orbits can decouple), day length, atmosphere scale height and sky appearance from the ground and from orbit, horizon distance and curvature, travel and transition speeds. Should these be per-quantity creator settings with defaults?
- Planet scale vs system scale (orbit distances, star sizes): one factor or separate ones.
Everything must hold up and look good across the whole range: precision and jitter at the surface, in orbit and between bodies; terrain LOD depth, streaming and memory from orbit down to centimeter-level edits; atmosphere, clouds and fog; vegetation and fauna density; and networking and save formats, which must not assume a scale. Treat scale as a parameter every question must answer to. Where a design only works at one scale, say so, and say what it would cost to support the full range.

## Scope gap analysis (do this first)
Compare the scope with No Man's Sky, Elite Dangerous, Star Citizen, Space Engineers, Kerbal Space Program, Outer Wilds, Astroneer and Space Engine, and with real astronomy and planetary science where it matters. List what's missing. At minimum, consider:
- Celestial bodies: star types and spectral classes driving light color and intensity, binary and multiple star systems, gas and ice giants (no landable surface), moons, rings, asteroid belts and fields, comets, super-earths, tidally locked planets, rogue planets, the galactic center black hole, quasars, nebulae, star clusters.
- Physically coherent generation: seed → physical parameters (mass, radius, composition, orbit, rotation, tilt) → derived properties (gravity, escape velocity, atmosphere retention, temperature from stellar luminosity, distance, albedo and greenhouse effect) → climate → biomes → life. A staged pipeline, not independent noise layers.
- Time: a universal simulation clock, per-body local time and calendars, seasons from axial tilt and orbit, eclipses, whether time compression or warp exists, and how all of that stays deterministic and cheap to sync.
- Reference frames: planets that rotate and orbit, surface bases riding on them, and which frame physics, networking and rendering each work in.
- Gravity and flight: spherical gravity and player orientation, transitions between gravity wells, the ship flight model (Newtonian vs assisted), atmospheric entry.
- Determinism across platforms: ARM vs x64, CPU vs GPU, across GPU vendors, and what that means for regenerating from seeds in multiplayer.
- Generator versioning: saves and server worlds surviving changes to procgen algorithms.
- Simulation LOD: off-screen catch-up when a place is revisited, ecosystem population dynamics, economy and trade, NPC ship traffic, faction territory over time, automated bases running while nobody is there, interest management when players are in different systems at once.
- Rendering at astronomical scale: a starfield and deep-sky objects generated from the actual galaxy, impostors for distant bodies, depth precision from orbit to caves, auto-exposure from deep space to underground, eclipses and ring shadows, underwater rendering, gas giant atmospheres.
- Later content: procedural creatures and their animation, procedural audio, ruins and structures, names and lore.
- Anything else you find.
Classify every item as (a) constrains a founding decision now, so fold it into the relevant question; (b) must be possible later, so name the extension point the architecture must leave open; or (c) out of scope for now, with a reason. Write the classified list to `docs/research/00-scope.md`. Everything after this must respect it.

## Questions
1. Large-world coordinates and reference frames: floating origin, double or int64 positions, planet-local vs system vs galaxy frames for rotating and orbiting bodies, how positions flow through ECS, physics, extraction and rendering, and the precision budget across the full scale range (from compressed to real scale), including galaxy-scale distances.
2. Celestial mechanics and time: on-rails orbits (Keplerian, patched conics) vs n-body, the universal clock, per-body rotation, tilt, seasons, day and year length, local time and calendars, eclipses, whether time compression exists, how all of it stays deterministic and cheap to sync, and how orbital periods, day length and gravity relate to the chosen scale (physical vs gameplay values).
3. The universe generation pipeline: the seed hierarchy (galaxy → sector → system → body → biome → chunk → object), the staged physically coherent pipeline from physical parameters to life, deep-space objects (galactic center, quasars, nebulae), cross-platform determinism, generator versioning, how player edits are stored on top of generated content, and scale as an input to the pipeline, so the same seed produces a coherent universe at any scale.
4. Representation and LOD of editable volumetric terrain: SDF/density, meshing and seams, chunk streaming, caves, bodies with no solid surface, asteroids as small editable bodies, how GPU-meshed chunks reach CPU physics, and how LOD depth, chunk sizes and streaming adapt to radii that differ by orders of magnitude while edits and detail stay in meters.
5. The procgen graph model: what runs in C++ or GPU compute, what is Luau, how users extend it with new nodes, biomes, body types and phenomena, how scale-dependent and scale-independent features are expressed, and how it becomes the foundation for a nature editor.
6. The networking model: authority, edits as deltas, what gets synced vs regenerated, interest management across systems, how it fits our fixed-step determinism and record/replay, and keeping wire and save formats independent of scale.
7. Simulation LOD: fauna, flora and ecosystems, NPCs and factions, economy and traffic, automated bases, weather and geological events (eruptions) at planet scale vs local, and off-screen catch-up.
8. The real-time PBR renderer on the existing render graph (forward+/clustered vs the current visibility buffer), and how astronomical-scale features plug in as RenderFeatures: composition-driven atmospheres, volumetric skies and clouds, terrain, vegetation, ocean and underwater, rings, gas giants, the starfield and deep-sky objects, multiple suns, depth precision and exposure. All of it must stay correct as the radius and atmosphere height change by orders of magnitude.
9. Physics at planetary scale (Box3D vs Jolt with double precision): colliders for streamed, editable chunks, gravity per body, physics in rotating frames, and stability at every scale.
If the gap analysis shows a question is missing or should be split, add or split it, and say why.

## Method
- Use real sources: GDC and SIGGRAPH talks, papers, engine docs, and postmortems or tech talks from shipped games and tools that solved the same problem. Prefer primary sources. Cite them with links. Mark anything you couldn't verify as unverified.
- You may run the questions in parallel with subagents. The scope gap analysis comes first, and the cross-question synthesis is yours, after all of them land.
- Evaluate every option against Veach as it is: cost given our RHI capabilities and gaps, effect on determinism, Luau extensibility, headless/CI testability and MCP inspection, behavior across the scale range, and whether it can be verified on the M5 Max.

## Output
In docs/research/:
- `00-scope.md`: the classified gap analysis.
- One file per question (`01-coordinates.md` … `09-physics.md`, plus any you added), each with: the problem in Veach's terms; the options with real-world precedents; tradeoffs (complexity, performance, determinism, extensibility, testability, scale range); its extension points; how it interacts with the other questions; your recommendation and what would change your mind; and what's expensive to undo.
- `00-synthesis.md`: one coherent proposed architecture that resolves the cross-question conflicts. Include a diagram of how the pieces fit on top of the current engine layers; the extension-point map (what can be added from Luau, from a C++ module, or only in the toolbox core); the scale model (what s applies to, which quantities stay fixed, which are creator settings and their defaults); the list of ADRs to write; the RHI gaps that must close first (mapped to VCH-071–076); and a spike plan.
- Spike plan: each spike is the smallest throwaway prototype that validates a decision, with a visible success criterion (something you can see in a screenshot or measure with Tracy). Size spikes by scope, never by time. Order them toward the first real slice: a seed-generated star system with a rotating, tilted planet seen from orbit, with an atmosphere driven by its composition and the correct time of day for where you are; flying down to the surface with no loading screen, landing, and digging a hole in the terrain. The slice must work at both 1/100 scale and real scale with the same code, switched by configuration.

Then write the ADRs that are engineering decisions as Accepted, update ARCHITECTURE.md (replace the open questions with the decisions and links), and update the backlog (spikes as items with ids). Anything that is a product decision goes into a single review packet at the end, with your recommendation for each: the hardware target; the default scale and the supported scale range; which scale-dependent quantities are creator settings and their defaults; multiplayer scope (player count, dedicated vs listen server); realism vs gameplay tradeoffs (for example real orbital periods vs compressed ones); and which features are in or out of the first slice. Don't start spikes in this task.
