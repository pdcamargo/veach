# Candela: real-time global illumination

**Status:** On hold (step 1c failed its gates: surfels or DDGI with the owner)
**Area:** render
**Owner:** dev lead (product-affecting: the product owner approves)

## Summary

Candela is the real-time renderer's indirect light: light that reaches a surface after bouncing off other surfaces
or arriving from the sky, from every light and emissive surface, in dynamic scenes with nothing baked. It replaces
the ambient stopgap (sky light darkened by short-range AO) and fits the Candela slice of the frame budget
(ADR 0049): 4.5 ms in the Performance tier, 10 ms in Quality.

## Motivation

Without it, everything the sun and lamps do not reach directly is lit only by the stopgap: street canyons glow as
if they saw the whole sky, interiors are black where the sky cannot reach, and colored walls never tint their
neighbors. The path tracer shows what is missing: Sponza's arcades, Bistro's shaded facades, the Cornell box's red
and green bleed. Roadmap: VISION's rendering pillar (Candela, "our Lumen"); backlog VCH-017.

## Requirements

- Candela MUST estimate diffuse indirect light at every visible surface: one bounce exactly in expectation, further
  bounces through a cache, the sky (ADR 0050) for rays that leave the scene, all light types and emissive surfaces
  at the bounce points (through the same light sampling as direct light, ADR 0041).
- It MUST need no precomputation or baking and nothing authored per scene (no hand-placed probes or volumes,
  special meshes or UVs): moving objects, lights, the time of day and emissive materials change the indirect light
  within a bounded number of frames.
- It MUST feed the denoiser's diffuse signal (ADR 0042) rather than add a denoiser of its own, and reuse the
  temporal foundation's surfaces, motion and disocclusion (ADR 0040).
- Its passes MUST fit the Candela slice of ADR 0049 on the reference machine: 4.5 ms (Performance) and 10 ms
  (Quality) on Bistro at 1440p output.
- Its image MUST be comparable to the path tracer: a path-tracer mode computes exactly what Candela estimates, and
  the real-time image stays within ADR 0047's tolerance of it, baselines recorded per scene.
- It MUST run on Metal and DX12 through the RHI and the render graph, with ray queries in compute (no hit shaders).
- Its cache MUST NOT leak light through thin walls: cells are keyed by the surface normal and finer near the camera.
- Its cache MUST be reusable by reflections (VCH-018): a query by position and normal from any pass.
- The ambient stopgap MUST remain only as the explicit "GI off" mode (`gi = "off"`), never as a silent fallback.
- Its GPU memory MUST fit the GI slice of ADR 0049's memory budget and show in the budget report.
- Indirect specular (reflections) is out of scope: VCH-018 will reuse Candela's cache.

## API sketch

```luau
camera:set(c.RealtimeLighting, { gi = "candela" })  -- or "off" (the ambient stopgap), "direct_only"
camera:set(c.Candela, {         -- Candela's settings; absent: these defaults
	ray_rate = 0.25,            -- final-gather rays per render pixel (Performance 0.25, Quality 0.5)
	spatial_samples = 2,        -- neighbors' reservoirs reused per pixel
	cache_bounces = 2,          -- bounces after the first in a cache update path, then the cache itself
})
camera:set(c.PathTracer, { mode = 4 })  -- reference: direct light plus diffuse indirect light, what Candela estimates
```

## Acceptance criteria

- **candela.AC1**: The Cornell box: the real-time image against the path tracer's mode 4 within ADR 0047 of its
  baseline; the red and green walls tint the floor and ceiling (measured: the floor's red/green ratio near each
  wall within 10% of the reference's).
- **candela.AC2**: Sponza, Bistro (noon and sunset): mean luminance within 3% of the path tracer's mode 4; mean
  FLIP and the per-pixel figures (99.9th percentile, pixels above 0.5) recorded here, within ADR 0047 of their
  baselines.
- **candela.AC3**: Cost on the M5 Max, Bistro at 1440p output: Candela's passes in the frame budget report
  (ADR 0049) within 4.5 ms at the Performance settings and 10 ms at Quality.
- **candela.AC4**: Stability: with a still camera and still lighting, the mean FLIP between consecutive final
  frames is at most 0.005 on the Cornell box, Sponza and Bistro (the owner's absolute target, replacing "direct light
  plus 20%").
- **candela.AC5**: Response: switching off Bistro's sun (or a Cornell box light) drops the indirect light below 10%
  of its old value within 10 frames; switching it on reaches 90% within 10 frames.
- **candela.AC6**: No ghosting beyond the temporal foundation's: the ghosting test (temporal.AC10) with Candela on
  stays within 10% of its value with the stopgap.
- **candela.AC7**: No leaks: behind one of Bistro's thin walls, unlit from its side, Candela's indirect light is
  within ADR 0047's tolerance of mode 4's, and not brighter than it by more than 10%.
- **candela.AC8**: Opening a door (an occluder removed) brings the new indirect light to 90% within 10 frames.

## Step 1 results (2026-11-26)

The prototype: `RealtimeLighting.gi = "candela"`, off by default. A radiance cache of 2^21 slots
(`veach/radiance_cache.slang`: cells 5 cm at level 0, about 1/80 of their distance to the camera, six normal
directions, an average over 3 frames, eviction after 32 idle frames), fed by one diffuse path per 5x5 pixels with two
bounces past the visible surface; a gather of one cosine ray per 4 pixels with temporal reuse, the sample's radiance
read again from its cell each frame; spatial reuse off (biased, VCH-066). The path tracer's mode 4 is the reference:
direct light, then the first hit's diffuse lobe followed through Lambertian bounces. Measured on the M5 Max with
`tools/bench/candela_step1.py` and `compare_direct.py --candela`.

**Cost**, Performance settings at 1440p output (0.5 scale), median ms:

| Scene | Cache update (est. 0.4-0.6) | Cache resolve | Gather (est. 0.3-1.2) | Spatial | Candela | GI off (stopgap) |
|---|---|---|---|---|---|---|
| Cornell box | 0.14 | 0.04 | 0.33 | 0.13 | 0.64 | 0.12 |
| Sponza | 0.54 | 0.13 | 0.95 | 0.18 | 1.79 | 1.30 |
| Bistro noon | **1.91** | 0.15 | **2.29** | 0.18 | **4.52** | 2.34 |
| Bistro, alpha tests skipped | 0.57 | 0.14 | 0.88 | 0.17 | 1.77 | |

GPU memory: 140 MiB (cache 56, reservoirs 84) of the 160 MiB GI slice.

**Accuracy** against mode 4 (`compare_direct.py --candela --final`, 640x360, 256 frames of warm-up; baselines in
`tests/tolerance.json`): Cornell box 0.0382 FLIP, -2.68%; Bistro awning 0.0109, -5.15%; Sponza 0.1436, +9.65%; Bistro
noon 0.1287, -8.36%. Without the denoiser, Cornell's average over 512 frames is -0.65%; the cache alone (every pixel
gathering, no reuse) -1.1%. Candela's own indirect light settles within 8 frames; the denoised image takes about
128 (the awning: -21% after 64 frames).

**Leaks** (candela.AC7): under Bistro's café awning, Candela is 5% darker than the reference, never brighter.

**Flicker** (candela.AC4, mean FLIP between consecutive frames of a still camera, 640x360): direct light only
0.0042 / 0.0009 / 0.0007 (Cornell, Sponza, Bistro), with Candela 0.0057 / 0.0097 / 0.0058: 1.4x, 10.8x, 8.3x. The same
at 1, 0.5 and 0.25 gather rays per pixel, and with the cache averaged over 8 frames instead of 3 (0.0056).

**Response** (candela.AC5, AC8: frames until Candela's indirect light, linear and before the denoiser, has gone 90% of
the way): a light switched off, 6 (Cornell's fill light), 11 (Sponza's sun), more than 20 (Bistro's sun and sky); a
door opened (Cornell's tall box removed, a lid over Sponza or Bistro moved away), 1, more than 20, more than 20.

**Step 1 verdict:** step 1 hit ADR 0051's stop conditions on Bistro: the gather (2.29 ms) and the cache update (1.91 ms) are
past twice their estimates, and flicker and response fail. Three causes, measured (VCH-067): closest-hit rays through
foliage re-trace once per transparent layer (without alpha tests Bistro's Candela is 1.77 ms, inside the estimates);
gather rays landing in cells no update path reached read black (Bistro dark, Sponza speckled); and flicker that comes
from the cache, not the gather rate. The owner decides how to go on.

## Step 1b results (2026-11-26)

The owner approved step 1b with gates on the Cornell box, Sponza and Bistro: gather and cache update each within
twice their estimates, flicker at most 0.005 FLIP between consecutive frames, luminance within 3% of mode 4, light
off settled within 10 frames, the leak test passing; and the same stop rule (Bistro failing cost or flicker: a
review packet recommending screen probes).

What changed:
- **Foliage:** Candela's rays use `trace_closest()`, one traversal that commits covered alpha candidates (ADR 0048's
  approach for the closest hit). It is correct on Metal (Sponza, every pixel gathering, no reuse: +2.30% against
  +2.73% with the layered trace()).
- **Misses:** every estimate goes into its cell and the two coarser cells containing it, so an empty fine cell reads
  its parent; when every level is empty, a pass of its own (`candela: misses`) lights the hit point and adds it.
- **Reuse:** temporal reuse in its own pass; a reused sample keeps the radiance it was chosen with and is dropped when
  its cell now reads more than 2x off. Replacing it with the cell's current value each frame (step 1) brightened
  Sponza by 12.7%: a hit first lit directly gains its bounced light once its cell fills, and the sample's weight
  was computed for the darker value. Spatial reuse is still off: pairwise MIS in area measure exploded (+400% on
  Sponza, from weights and geometry terms computed with different normals), and with the reconnection Jacobian it
  left fireflies on the Cornell box (516 pixels above 0.5).

**Cache statistics** (per frame, Performance at 1440p, 90 frames, `candela cache:` in the log):

| | Cells held (of 2.1M) | New cells | Evictions | Dropped (slots taken) | Lookups: own / parent / missed |
|---|---|---|---|---|---|
| Cornell box | 18.7k | 237 | 24 | 0 | 99.8 / 0.2 / 0.0% |
| Sponza | 643k | 11.8k | 4.1k | 27 | 92.7 / 7.1 / 0.3% |
| Bistro noon | 908k | 19.5k | 8.5k | 275 | 80.7 / 18.4 / 0.8% |

Collisions are rare (the table is under half full) and churn is 1-2% of the cells per frame. The flicker comes from
sparse updates instead: Bistro adds about 242k estimates a frame into 908k cells, so most cells get one every three
or four frames, and each moves its value.

**Gates:**

| Gate | Cornell box | Sponza | Bistro noon |
|---|---|---|---|
| Gather, ms (estimate 0.3-1.2, limit 2.4) | 0.26 | 0.74 | 1.17 |
| Cache update, ms (estimate 0.4-0.6, limit 1.2) | 0.15 | 0.60 | 1.15 |
| Candela in all, ms (slice 4.5) | 0.81 | 2.15 | 3.16 |
| Flicker, FLIP (limit 0.005) | 0.0048 | **0.0058** | **0.0071** |
| Luminance against mode 4 (limit 3%) | -1.6% | **+3.6%** | -1.3% |
| FLIP against mode 4 (99.9th percentile, pixels above 0.5) | 0.031 (0.23, 1) | 0.068 (0.44, 64) | 0.052 (0.28, 5) |
| Light off, frames to 90% (limit 10) | **>20** | **>20** | **>20** |
| Door opened, frames to 90% | 1 | >20 | >20 |

Leak test (candela.AC7): the awning's underside is 6.8% brighter than mode 4, inside the 10% bound. With step 1's
per-frame refresh the Cornell box's light-off case settled in 6 frames, at the cost of Sponza's +12.7%: the reservoir
can keep the light it was chosen for, or follow the cache, not both.

**Verdict:** Bistro still fails flicker, so by the owner's rule step 1b ends with a review packet recommending screen
probes; light-off response fails everywhere as well. Cost, accuracy (bar Sponza's +3.6%) and leaks pass.

## Diagnostic: the cache alone (2026-11-26)

The owner approved screen probes on one condition: first show that the cache is steady by itself, read at the
visible surface with no rays and no reservoirs (`Candela.gather = "surface_cache"`: update paths also add, at their
visible vertex, the indirect light arriving there, into cells of their own kind). At most 0.005 FLIP between frames
meant probes; more meant the cache is the source and probes would inherit it.

| Cache alone, static camera and lights | Cornell box | Sponza | Bistro noon |
|---|---|---|---|
| Flicker, FLIP between frames (limit 0.005) | 0.0107 | 0.0074 | 0.0138 |
| Light off, frames to 90% | 8 | 17 | 20 |

**The cache is the source: no probe code.** Levers measured on Bistro in this mode:

| Change | Flicker | Light off | Cache update |
|---|---|---|---|
| None (cells 1/80 of their distance, 3-frame average, 1 path per 5x5 pixels) | 0.0138 | 20 | 1.15 ms |
| Coarser cells (1/40) | 0.0158 | 10 | 1.17 ms |
| 2.8x the update paths (1 per 3x3) | 0.0133 | 11 | 2.38 ms |
| No eviction | 0.0132 | >20 | 1.19 ms |
| Cells used only after 3 frames | 0.0132 | >20 | 1.20 ms |
| A 32-frame average | 0.0080 | >20 | 1.20 ms |

The cache's raw output (linear, before the denoiser) changes by 20% of its value from one frame to the next with
the 3-frame average, 35% with 2.8x the paths (every cell then updates every frame, each update a third of the way to
a noisy mean), and 4% with a 32-frame average. A long average is most of the fix but loses the response; the
0.008 left with it is not explained by the raw change (cell edges are the suspect). The decision is with the owner.

## Step 1c results (2026-11-26)

The owner approved fixing the cache before any gather work, gated in the cache-alone mode: flicker at most 0.005,
light off within 10 frames, the cache update within 1.2 ms on Bistro, luminance within 3% of mode 4. Tried, in this
order (commit 99589dd, reverted by the next):

- Each frame's samples already summed per cell before blending; the blend now weighed them by their count, over a
  capped history (64 samples).
- A short average watching for change, restarting the history when it left it by more than the noise allowed.
  First version: restarts on noise, every frame (Bistro flicker 0.0192). Fixed to trust only long histories and keep
  each cell's noise across restarts: then no restart fired, and light off took over 20 frames everywhere.
- Lookups trusting a cell by its sample count and letting its parent share the rest, and eviction after 255 idle
  frames instead of 32: the cache churned (Bistro: 7.2k new cells and 3.8k evictions a frame), each new cell switching
  pixels from their parent's value to its own. Flicker 0.0055 / 0.0085 / 0.0092, light off still over 20.
- A history that fades with time (the sample count decays each frame), so thinly sampled cells defer to their
  better-sampled parents; and a deterministic, normal-aware trilinear blend across neighboring cells. The fade sets
  the trade between the two gates:

| History | Cornell flicker | Sponza flicker | Bistro flicker | Light off (Cornell, Sponza, Bistro) |
|---|---|---|---|---|
| About 5 frames | 0.0077 | 0.0168 | 0.0196 | 6, >20, 2 |
| About 10 frames (kept for the gates below) | 0.0066 | 0.0102 | 0.0110 | 11, 5, 6 |
| About 20 frames | | 0.0091 | 0.0096 | 17, 16 (Sponza, Bistro) |
| 64 samples, no fade | 0.0055 | 0.0085 | 0.0092 | >20 |

The neighbor blend changed flicker by under 10% either way (Bistro 0.0192 with, 0.0207 without, at the first
setting). With a 10-frame history the gates read:

| Gate, cache alone | Cornell box | Sponza | Bistro noon |
|---|---|---|---|
| Flicker (limit 0.005) | **0.0066** | **0.0102** | **0.0110** |
| Light off, frames (limit 10) | **11** | 5 | 6 |
| Cache update, ms (limit 1.2) | 0.25 | 0.80 | **1.30** |
| Luminance against mode 4 (limit 3%) | +0.2% | **+11.5%** | **+5.2%** |

The cache's raw output still changed 14-17% a frame (outgoing cells 13.6%, incoming 16.5%). In Candela's ReSTIR mode
the same cache made things worse: Bistro's gather 1.17 -> 2.13 ms and temporal reuse 0.29 -> 1.09 ms (eight cells read
per level), Cornell -2.03%, and the awning leak test +52.7%.

**Verdict:** step 1c fails; the cache is back to step 1b's, and the owner chooses between surfels and DDGI. A
regression gate now holds the cache-alone mode at its measured values: `candela: the cache alone stays as steady and
quick` on the Cornell box (flicker 0.013, light off 13 frames) and Bistro (0.016, 25).

## Out of scope

- Indirect specular and reflections (VCH-018; they will read Candela's cache).
- Volumetric fog's indirect light (VCH-020).
- A software (non-RT) fallback: RT hardware is the baseline (VISION).

## Open questions

- Settled: the approach ([ADR 0051](../../adr/0051-candela-approach.md), accepted with conditions).

## Links

- ADRs: [0051 approach (proposed)](../../adr/0051-candela-approach.md), [0049 budget](../../adr/0049-frame-budget-and-quality-tiers.md),
  [0047 tolerance](../../adr/0047-image-tolerance-policy.md), [0041 ReSTIR DI](../../adr/0041-restir-di.md),
  [0042 denoiser](../../adr/0042-denoiser.md), [0050 sky](../../adr/0050-physical-atmosphere.md)
- Backlog: VCH-017
