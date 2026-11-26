# 0049: Frame budget and quality tiers

Date: 2026-11-25

## Status

Proposed. The owner approved the two tiers, Performance as the default and the budget report, with additions
(review packet replies, 2026-11-25); it becomes Accepted once the owner has judged the Performance tier's 0.5 render
scale from the upscaling comparison. Until then the renderer's defaults stay at Quality's settings.

## Context

Bistro's exterior rendered in 14.5 ms of GPU time at 1440p output on the M5 Max, and direct lighting alone
(ReSTIR, the shading pass with its shadow rays, the denoiser) was 10.1 ms of it. Candela (GI), reflections, fog and
transparency still have to fit. Without a budget per subsystem, each feature takes what is left and the last one
has nothing.

The M5 Max (32 GPU cores, 36 GB) is the only machine we can measure on. Mid-range Macs have a third of its GPU and
often 16 GB of memory shared between CPU and GPU, so every budget here is optimistic for them.

Measured on the M5 Max, Bistro, 1440p output, MetalFX, release build (`tools/bench/frame_times.py`, before the sky):

| Render scale, candidates, neighbors | Frame | Visibility + surface | Direct lighting | Denoiser part of it | Upscaled FLIP (640x360) |
|---|---|---|---|---|---|
| 0.75, 12, 2 (today's default) | 14.5 ms | 2.4 ms | 10.1 ms | 3.3 ms | 0.087 |
| 0.6, 8, 1 | 9.8 ms | 2.0 ms | 6.5 ms | 2.1 ms | |
| 0.5, 12, 2 | 8.1 ms | 1.9 ms | 4.9 ms | 1.7 ms | 0.100 |
| 0.5, 8, 1 | 7.7 ms | 1.9 ms | 4.5 ms | 1.7 ms | |

## Decision

**Two tiers**, each a GPU-time budget and a GPU-memory budget split by subsystem, at 1440p output on the reference
machine. **Performance is the default** at startup.

| Subsystem | Performance (60 fps, 16.67 ms) | Quality (30 fps, 33.33 ms) |
|---|---|---|
| Visibility, surfaces, TLAS | 2.5 ms | 3.0 ms |
| Direct lighting (ReSTIR, shadows, denoising) | 4.5 ms | 10.0 ms |
| GI (Candela; the ambient stopgap while GI is off) | 4.5 ms | 10.0 ms |
| Reflections | 2.0 ms | 5.0 ms |
| Sky (the atmosphere's lookup tables) | 0.25 ms | 0.5 ms |
| Volumetric fog | 0.5 ms | 1.0 ms |
| Transparency and particles | 0.5 ms | 1.0 ms |
| Post: upscaling, display, readbacks | 1.5 ms | 2.0 ms |
| Headroom | 0.42 ms | 0.83 ms |

Fog and transparency (VISION) take their slices from the headroom and the sky's old 0.5 ms, which the sky measured
under 0.25 ms. Headroom is now thin in Performance: the GI slice is where to find more if a later subsystem needs it
(Candela is estimated at 1.3-2.6 ms of its 4.5, ADR 0051).

Tier settings for direct lighting: **Performance** renders at 0.5 scale with 8 candidates and 1 spatial neighbor;
**Quality** at 0.75 with 12 and 2. Further Performance options if the slice tightens: a half-rate sun ray (temporal
accumulation fills the other frames), 3 a-trous iterations instead of 5.

**GPU memory**, in MiB, sized so the engine stays near 4-5 GiB: under half of what a 16 GB Mac can give the GPU,
with room for the game's own data and the OS.

| Kind | Performance | Quality | Bistro today (Performance / Quality) |
|---|---|---|---|
| Geometry: vertices, indices, meshlets, records | 512 | 512 | 214 |
| Acceleration structures with scratch | 768 | 768 | 469 |
| Material textures | 1536 | 1536 | 301 |
| Render targets, history, reservoirs, readbacks | 640 | 1280 | 448 / 910 |
| GI: Candela's cache and reservoirs | 160 | 256 | 0 (64 MB cache approved, ADR 0051) |
| Sky | 8 | 8 | 0.6 |
| Other | 64 | 64 | 0 |
| **Total (the rest is headroom)** | **4096** | **5120** | 1434 / 1896 |

Counted per resource as the driver sized it. The driver's own total (Metal's `currentAllocatedSize`) is about 0.6 GiB
more on Bistro (pipelines, MetalFX, the drawable pool) and is reported next to it.

**Budget report.** `engine/render/src/frame_budget.cpp` holds both tiers and maps every render-graph pass and GPU
resource to a subsystem by name. MCP `frame_budget { tier }` renders a few frames and returns each subsystem's median
GPU time and its passes against the slice, memory by kind against its slice, the GI mode (Candela, the ambient
stopgap as "GI off", direct only, or the path tracer), whether the camera's settings are the tier's, and `ok`. GPU
work no pass timestamp sees (MetalFX) counts in post. CI (WARP) checks that every pass and resource has a subsystem
and that the report runs; the numbers are checked on the reference machine before a milestone closes and recorded in
its spec.

Bistro at noon today (GI off, the ambient stopgap with 4 rays), from the report:

| Tier | Frame | Visibility | Direct | GI (stopgap) | Sky | Post |
|---|---|---|---|---|---|---|
| Performance | 9.84 ms | 2.17 | 4.13 | 2.32 | 0.06 | 1.16 |
| Quality | 18.80 ms | 2.70 | 9.85 | 4.96 | 0.06 | 1.22 |

**Mid-range hardware is estimated, not measured.** A base M3 or M4 has a 10-core GPU against the M5 Max's 32.
Scaling the frames above by core count alone:

| Estimate, base M3/M4 (10 GPU cores) | Performance | Quality |
|---|---|---|
| Bistro frame, x3.2 | about 31 ms (32 fps) | about 60 ms (17 fps) |
| Render scale that would hold the tier's frame time | about 0.35 | about 0.5 |

This is a lower bound on the gap: the M5's GPU cores are faster per core than the M3's and M4's, and memory
bandwidth (about 120 GB/s against about 400) falls by a similar ratio, so the real frames are likely 20-50% longer.
The render-scale row assumes the pixel-bound passes scale with pixel count, and visibility and the TLAS only partly
do.

**For hardware we cannot measure, dynamic resolution is the plan** (VCH-065): the render scale adapts each frame
to hold the tier's frame time, between a floor (about 0.33) and the tier's own scale, with the temporal upscaler
absorbing the change. A tier then fixes the frame rate and the feature set, and resolution is what gives. Until it
exists, an unmeasured Mac runs the tier's settings and misses its frame rate.

## Consequences

- Candela, reflections, fog and transparency each have a number before they are designed (Candela's design must
  fit 4.5 ms in Performance).
- Performance at 0.5 scale costs about 15% more upscaled FLIP than 0.75 on Bistro; the owner judges it by eye.
- The budget file is something every rendering change keeps in step: a new pass or resource name fails the
  mapping test until it has a subsystem.
- The GI slice holds the ambient stopgap while GI is off (2.3 ms of 4.5 in Performance), so the report shows what
  Candela replaces.
- Every budget is a top-end number. Until dynamic resolution lands, mid-range Macs are expected to miss them.

## Alternatives considered

- **One 60 fps tier at today's settings:** direct lighting alone took 10.1 ms of the 16.67, leaving nothing that
  fits Candela and reflections.
- **One 30 fps tier:** easy to fit, but VISION targets 60 fps at 1440p.
- **Tiers per machine class instead of per frame rate:** needs machines we do not have; dynamic resolution covers
  the same ground with one measured machine.
