# Candela: real-time global illumination

**Status:** In progress (step 1 measured: stop conditions hit, the owner decides; see Step 1 results)
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
- **candela.AC2**: Sponza, Bistro (noon and sunset): within ADR 0047 of their baselines, recorded here.
- **candela.AC3**: Cost on the M5 Max, Bistro at 1440p output: Candela's passes in the frame budget report
  (ADR 0049) within 4.5 ms at the Performance settings and 10 ms at Quality.
- **candela.AC4**: Stability: a still camera's frame-to-frame FLIP over 64 frames no worse than direct lighting
  alone plus 20% (the temporal foundation's stability measure).
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

**Verdict:** step 1 hit ADR 0051's stop conditions on Bistro: the gather (2.29 ms) and the cache update (1.91 ms) are
past twice their estimates, and flicker and response fail. Three causes, measured (VCH-067): closest-hit rays through
foliage re-trace once per transparent layer (without alpha tests Bistro's Candela is 1.77 ms, inside the estimates);
gather rays landing in cells no update path reached read black (Bistro dark, Sponza speckled); and flicker that comes
from the cache, not the gather rate. The owner decides how to go on.

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
