# 0001: Reset: from a raytracing-first engine to the foundation of a space-game toolbox

Date: 2026-11-27

## Status

Accepted. A product decision by the owner (2026-11-27); this ADR records it and how the repository was reset.

## Context

Until now Veach was an AI-native engine whose product was a raytracing-first real-time renderer: hardware ray
tracing as the baseline, real-time global illumination, a denoiser, a temporal upscaler, ReSTIR-style direct lighting
and RT reflections, with a reference path tracer as ground truth. That plan is now 54 ADRs and the specs, code, tests
and tools that served it.

The owner changed what Veach is for. Veach is the engine foundation of a reusable, extensible toolbox for space games
in the spirit of No Man's Sky: procedural planets and solar systems, editable volumetric terrain, atmospheres, oceans,
vegetation, fauna and flora, NPCs and factions, base and ship building, and multiplayer, extended by other people
through C++ modules and Luau. Its real-time renderer is a stylized-but-not-cartoony PBR rasterizer. Real-time GI and
real-time path tracing are out of scope. The AI-native identity (text everywhere, reflection everywhere, strict Luau,
errors that tell you the fix, headless and deterministic, measured not claimed, transactional writes, MCP, one API for
agents and humans) is unchanged.

## Decision

- **The raytracing-first era ends.** It is preserved in the annotated git tag `pathtracer-era` (on `8c272b5`). Agents
  consult it only when the owner asks, or when reusing photo-mode internals.
- **The ADR series restarts at 0001.** The old ADR files are deleted from the tree (`git show
  pathtracer-era:docs/adr/<file>` reads them). Every decision that survives is rewritten concisely as one of 0002 to
  0038, stating the current decision. The reset's own decisions are 0003 (hardware target), 0004 (keep our RHI),
  0039 (raster PBR), 0040 (the interim real-time path), 0041 (photo mode) and 0042 (the toolbox layering).
- **Code that served only the ended plan is deleted**: the real-time GI, the denoiser, the temporal upscalers (ours
  and MetalFX), ReSTIR, the temporal history and motion vectors, quality tiers and the frame budget report, the image
  tolerance policy, and their shaders, scripts, goldens, tests and tools. What stays is listed in the appendix.
- **The reference path tracer becomes photo mode** (ADR 0041); **the real-time path is a basic visibility-buffer
  rasterizer** until the research phase decides the real renderer (ADR 0040).
- **The next milestone is the research phase** (VCH-070): the open questions in
  [ARCHITECTURE.md](../ARCHITECTURE.md#open-questions-research-phase), answered by ADRs and spikes before any toolbox
  feature is built.

## Consequences

- The tree describes one product. Old ADR numbers no longer mean anything outside the tag; every link was moved to
  the new numbers.
- Real-time image quality drops for now: no shadows, no GI, no reflections in real time. Photo mode keeps the
  reference images exactly (its goldens are unchanged).
- Hardware ray tracing stops being a requirement (ADR 0003), which widens the hardware the engine runs on.
- Work that existed only in the tag (the GI cache, the denoiser) is not maintained. Reviving any of it needs a new
  ADR that fits the new direction.

## Alternatives considered

- **Supersede the old ADRs one by one.** Keeps history in the tree, but leaves 54 records of which most are
  superseded, numbers that mean the old plan, and a reader who must replay them to learn the current state. The tag
  keeps the history at no cost.
- **Keep the raytracing renderer as an optional path.** Every renderer change would have to keep it working, for a
  feature the product no longer wants.
- **Start a new repository.** Loses the history, CI, LFS assets, tooling and the agent setup that all still apply.

## Appendix: triage

Keep = unchanged in substance; rewrite = kept with the ended plan removed or renamed; delete = removed (in the tag).

| Item | Verdict | Why |
|---|---|---|
| Old ADRs 0001, 0003-0008, 0010-0023, 0026, 0029-0033, 0036, 0038 | rewrite as 0002, 0005-0038 | Engine decisions that still hold; renumbered and stated as current |
| Old ADR 0002 (hardware) | rewrite as 0003 | RT is no longer the baseline |
| Old ADR 0009 (RHI) | rewrite as 0004 | Same RHI, now with the alternatives considered at the reset |
| Old ADRs 0024 + 0039, 0027 + 0028 | merged into 0027, 0031 | One decision each once their amendments are folded in |
| Old ADR 0034 (lights, path tracer) | split into 0036 and 0041 | Lights hold for every renderer; the path tracer becomes photo mode |
| Old ADRs 0035, 0048 (alpha in rays) | folded into 0041 | Photo mode's ray tracing details |
| Old ADR 0037 (visibility buffer) | rewrite as 0040 | Kept as the interim real-time path, minus RT shading |
| Old ADR 0044 (Metal residency set) | rewrite as 0028 | Still the Metal residency model |
| Old ADR 0050 (physical sky) | rewrite as 0038 | The sky serves both renderers |
| Old ADRs 0040-0043, 0045-0047, 0049, 0051-0053 | delete | Temporal foundation, ReSTIR, denoiser, upscaling, exact suns beside ReSTIR, image tolerance policy, quality tiers, real-time GI, history resets |
| Specs: asset-system-core, dx12-backend, ecs-and-phases, engine-lifecycle, first-light, hot-reload, input, mcp-server, platform-and-windows, record-replay, render-views, scripting-luau-api-v0, transforms | keep, links updated | Engine capabilities that still hold |
| Specs: first-real-scene, physical-sky | rewrite | Path tracer criteria moved to photo-mode; the sky's ambient stopgap and AO removed |
| Specs: candela, realtime-renderer-v0, temporal-direct-lighting, soft-shadows | delete | The ended renderer; sized lights live on in photo mode |
| Specs: realtime-renderer, photo-mode | new | The two renderers as they are after the reset |
| Backlog | rewrite | Renderer-plan items removed; VCH-070 research phase and VCH-071-076 RHI gaps added |
| Guides | rewrite | ADR links, components and tools brought up to date; rendering.md rewritten |
| `engine/core`, `ecs`, `platform`, `asset`, `script`, `runtime` | keep | The engine foundation (MCP: `path_trace` renamed `photo_mode`, `frame_budget` removed) |
| `engine/rhi` | keep, trimmed | MetalFX temporal scaler removed; ray tracing optional, `VEACH_DISABLE_RAY_TRACING=1` |
| `engine/render`: render graph, extraction, display, sky, materials, scene upload | keep | The foundation of any renderer |
| `engine/render`: path tracer | rewrite as photo mode | Comparison modes and jitter switch removed; images unchanged |
| `engine/render`: real-time renderer | rewrite | Visibility pass kept; one non-RT shading pass replaces the RT/temporal pipeline |
| `engine/render`: Candela, TAAU, ReSTIR, temporal, denoiser shaders; frame budget | delete | The ended plan |
| Components `PathTracer`, `RealtimeLighting`, `Candela`, `Upscaling`, `RenderQuality` and their enums | `PhotoMode` / delete | Renamed, or removed with their features |
| Lab scripts | keep; delete `bistro_awning`, `glow_closeup`, `cornell_box_ao`, `dev/gi_modes` | The deleted ones tested ended features |
| Goldens | keep photo mode and triangles; regenerate the 4 real-time ones; delete `cornell_box_ao` | Real-time output changed on purpose |
| Tests: tolerance, Candela, motion-trail, upscale, soft-shadow, temporal and budget tests | delete | They measured ended features |
| `tools/bench/` | delete | Benchmarks of the ended renderer |
| `tools/metal-repros/` | keep `path-tracer-second-trace`; delete the GI repro | The kept one still constrains photo mode |
| `types/`, `schemas/`, `apps/lab/types/` | regenerate | Components changed |
| CI, `.mcp.json`, githooks, `tools/push.sh` | keep | Only the tolerance file left the push trigger list |
| `space/` | new | The toolbox's home, empty (ADR 0042) |
