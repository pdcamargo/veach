# Candela: real-time global illumination

**Status:** Draft (design only: the owner reviews the approach in [ADR 0051](../../adr/0051-candela-approach.md)
before any code)
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
- Indirect specular (reflections) is out of scope: VCH-018 will reuse Candela's cache.

## API sketch

```luau
-- On the camera, next to RealtimeLighting. Absent: the ambient stopgap, as today.
camera:set(c.Candela, {
	ray_rate = 0.25,        -- final-gather rays per render pixel (Performance 0.25, Quality 0.5)
	spatial_samples = 2,    -- neighbors' reservoirs reused per pixel
	cache = true,           -- multiple bounces through the world-space radiance cache
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

## Out of scope

- Indirect specular and reflections (VCH-018; they will read Candela's cache).
- Volumetric fog's indirect light (VCH-020).
- A software (non-RT) fallback: RT hardware is the baseline (VISION).

## Open questions

- The approach: [ADR 0051](../../adr/0051-candela-approach.md), awaiting the owner.

## Links

- ADRs: [0051 approach (proposed)](../../adr/0051-candela-approach.md), [0049 budget](../../adr/0049-frame-budget-and-quality-tiers.md),
  [0047 tolerance](../../adr/0047-image-tolerance-policy.md), [0041 ReSTIR DI](../../adr/0041-restir-di.md),
  [0042 denoiser](../../adr/0042-denoiser.md), [0050 sky](../../adr/0050-physical-atmosphere.md)
- Backlog: VCH-017
