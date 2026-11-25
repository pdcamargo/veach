# 0049: Frame budget and quality tiers

Date: 2026-11-25

## Status

Proposed (the owner decides: review packet in the 2026-11-25 summary).

## Context

Bistro's exterior renders in 14.5 ms of GPU time at 1440p output on the M5 Max, and direct lighting alone (ReSTIR,
the shading pass with its shadow rays, the denoiser) is 10.1 ms of it. Candela (GI), reflections and the sky still
have to fit. Without a budget per subsystem, each feature takes what is left and the last one has nothing. The M5
Max is top-end hardware: a budget measured only there says little about a mid-range machine.

Measured on the M5 Max, Bistro, 1440p output, MetalFX, release build (`tools/bench/frame_times.py`):

| Render scale, candidates, neighbors | Frame | Visibility + surface | Direct lighting | Denoiser part of it | Upscaled FLIP (640x360) |
|---|---|---|---|---|---|
| 0.75, 12, 2 (today's default) | 14.5 ms | 2.4 ms | 10.1 ms | 3.3 ms | 0.087 |
| 0.6, 8, 1 | 9.8 ms | 2.0 ms | 6.5 ms | 2.1 ms | |
| 0.5, 12, 2 | 8.1 ms | 1.9 ms | 4.9 ms | 1.7 ms | 0.100 |
| 0.5, 8, 1 | 7.7 ms | 1.9 ms | 4.5 ms | 1.7 ms | |

## Decision (proposed)

Two tiers, each a frame budget split by subsystem, at 1440p output on the reference machine:

| Subsystem | Performance (60 fps, 16.6 ms) | Quality (30 fps, 33.3 ms) |
|---|---|---|
| Visibility, surfaces, TLAS | 2.5 ms | 3.0 ms |
| Direct lighting (ReSTIR, shadows, denoising) | 4.5 ms | 10.0 ms |
| Candela (GI) | 4.5 ms | 10.0 ms |
| Reflections | 2.0 ms | 5.0 ms |
| Sky, atmosphere, fog | 0.5 ms | 1.0 ms |
| Post, upscale, display | 1.5 ms | 2.0 ms |
| Headroom | 1.1 ms | 2.3 ms |

Tier settings for direct lighting: **Performance** renders at 0.5 scale with 8 candidates and 1 spatial neighbor
(Bistro: 4.5 ms); **Quality** at 0.75 with 12 and 2 (10.1 ms). Further Performance options if the slice tightens:
a half-rate sun ray (temporal accumulation fills the other frames), 3 a-trous iterations instead of 5.

**Budget report.** A budget file maps each render-graph pass to a subsystem and holds both tiers' slices. An MCP
tool (`frame_budget`) returns, per subsystem, the measured GPU time against its slice. CI (null and WARP, whose
timings mean nothing) checks that every pass maps to a subsystem and that the report runs; the numbers are checked
on the reference machine by `tools/bench/budget.py` before a milestone closes, and recorded in its spec.

**Mid-range measurement.** Budgets are set for the M5 Max now. A second reference machine, a mid-range Apple
Silicon Mac, gets the same report before each milestone; its tier is the one that fits there.

## Consequences

- Candela and reflections get a number before they are designed (Candela's design must fit 4.5 ms Performance).
- Performance at 0.5 scale costs about 15% more upscaled FLIP than 0.75 on Bistro.
- The budget file becomes something every rendering change keeps in step.
