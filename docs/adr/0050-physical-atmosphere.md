# 0050: The physical atmosphere: Hillaire LUTs shared by both renderers, one Sky component

Date: 2026-11-25

## Status

Accepted. Replaces `RealtimeLighting.sky` and `PathTracer.sky` with the `Sky` component.

## Context

The [physical-sky spec](../specs/physical-sky/spec.md) needs a sky that is cheap enough for the 0.5 ms
sky slice of [0049](0049-frame-budget-and-quality-tiers.md), physically based enough for the path tracer to use as
its reference environment, and parameterized by time of day.

## Decision

**Model.** Hillaire 2020 with the paper's Earth defaults: planet radius 6,360 km, atmosphere top 6,460 km; Rayleigh
scattering (5.802, 13.558, 33.1) x 1e-6 per meter, scale height 8 km; Mie scattering 3.996e-6, absorption 4.40e-7
per meter, scale height 1.2 km, phase g = 0.8; ozone absorption (0.650, 1.881, 0.085) x 1e-6 per meter in a tent
centered at 25 km, 30 km wide; ground albedo 0.3. `Sky` scales Rayleigh, Mie and ozone densities.

**Units.** The LUTs hold luminance per lux of sun illuminance at the top of the atmosphere; times
`Sky.sun_illuminance` (default 128,000 lux) they are nits, like every other light (ADR 0034).

**LUTs** (engine shader library `atmosphere.slang`, compute, rgba16f):

| LUT | Size | When | Holds |
|---|---|---|---|
| transmittance | 256 x 64 | parameters change | transmittance to space by altitude and zenith angle |
| multiple scattering | 32 x 32 | parameters change | Hillaire's isotropic multiple-scattering term |
| sky view | 192 x 108 | every frame | in-scattered luminance around the camera, by direction (latitude-mapped toward the horizon) |
| aerial perspective | 32 x 32 x 32, as a 1024 x 32 atlas (the RHI has no 3D textures) | every frame | in-scattering and transmittance along camera rays, to 32 km |
| sky irradiance | 1 x 1 | every frame | the sky's cosine-weighted average for an upward normal (the ambient stopgap) |

**The sun** is computed on the CPU (solar declination and hour angle from `time_of_day`, `latitude`, `day_of_year`;
north = -Z rotated by `north` about +Y) and its ground illuminance is `sun_illuminance` times the transmittance along
it, integrated on the CPU with the LUT's integrator (so the light and the sky agree). With `drives_sun` it replaces
the scene's directional lights (angular diameter 0.53 degrees, so soft shadows). Without a `Sky` entity the default
sky aligns with the scene's brightest directional light and scales its sun illuminance so the ground illuminance
matches that light.

**Use.**
- Real-time: pixels without geometry show the sky view plus the sun's disk; the ambient stopgap's cosine rays
  each see the sky view in their direction unless blocked within `ao_radius`; aerial perspective applies to the lit
  image (shading and the denoiser's remodulation). The sky-irradiance texel only gates the stopgap for now.
- Path tracer: escaped camera rays see the sky view and the disk; escaped bounce rays see the sky view only (the
  sun is the directional light, sampled by NEE: no double counting); aerial perspective applies to camera hits.
  `PathTracer.mode = 3` is direct light plus the sky on the first hit's diffuse lobe through one cosine ray: with
  an unlimited `ao_radius`, exactly what the real-time stopgap estimates (sky reflections wait for VCH-018).
- Both read the same LUTs from the same scene-renderer passes.

## Consequences

- Outdoor scenes get sky light and haze by default; indoor test scenes set `Sky.mode = "none"` or `"constant"`.
- The sky view is at the camera's altitude: bounce rays far from the camera see the camera's sky (fine near the
  ground, wrong for flight altitudes).
- `RealtimeLighting.sky` and `PathTracer.sky` are gone: scripts move to `Sky { mode = "constant", color = ... }`.

## Alternatives considered

- **Bruneton 2008 (precomputed 4D scattering):** more accurate multiple scattering, much larger tables and a slow
  precomputation on every parameter change.
- **Analytic sky models (Preetham, Hosek-Wilkie):** no aerial perspective, no altitude, fit to fixed conditions.
- **Ray marching the atmosphere per pixel:** too costly for the 0.5 ms slice.
