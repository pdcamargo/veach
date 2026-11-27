# 0038: The physical sky: Hillaire LUTs shared by both renderers, one Sky component

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0050).

## Context

Both renderers need an environment: the real-time renderer behind geometry and as ambient light, photo mode for
escaped rays. It must be cheap per frame, physically based enough to serve as photo mode's environment, in the same
units as the lights ([ADR 0036](0036-lights-in-photometric-units.md)), and parameterized by time of day.
Spec: [physical-sky](../specs/physical-sky/spec.md).

## Decision

**Model.** Hillaire 2020 with the paper's Earth defaults: planet radius 6,360 km, atmosphere top 6,460 km; Rayleigh
scattering (5.802, 13.558, 33.1) × 1e-6 per meter, scale height 8 km; Mie scattering 3.996e-6 and absorption
4.40e-7 per meter, scale height 1.2 km, phase g = 0.8; ozone absorption (0.650, 1.881, 0.085) × 1e-6 per meter in a
tent centered at 25 km, 30 km wide; ground albedo 0.3.

**The `Sky` component** (`scene.hpp`, Scriptable; on any one entity, none means the defaults): `mode` (`physical`,
`constant` with `color` in nits, or `none`), `drives_sun`, `time_of_day` (hours, local solar), `latitude`
(degrees), `day_of_year`, `north` (radians: north is −Z turned about +Y), `sun_illuminance` (lux at the top of the
atmosphere, default 128,000), `ground_albedo`, density multipliers `rayleigh`, `mie`, `ozone`, `mie_g`, and
`aerial_perspective`.

**Units.** The LUTs hold luminance per lux of sun illuminance at the top of the atmosphere; times
`sun_illuminance` they are nits.

**LUTs** (`engine/render/shaders/atmosphere.slang`, compute, rgba16f, built by `AtmosphereRenderer`):

| LUT | Size | When | Holds |
|---|---|---|---|
| transmittance | 256 × 64 | parameters change | transmittance to space by altitude and zenith angle |
| multiple scattering | 32 × 32 | parameters change | Hillaire's isotropic multiple-scattering term |
| sky view | 192 × 108 | every frame | in-scattered luminance around the camera by direction, latitude-mapped toward the horizon |
| aerial perspective | 32 × 32 × 32, as a 1024 × 32 atlas (the RHI has no 3D textures) | every frame | in-scattering and transmittance along camera rays, to 32 km |
| sky irradiance | 1 × 1 | every frame | the sky's cosine-weighted average for an upward normal |

Passes read them through one `SkyFrame` structured-buffer element (`veach/atmosphere.slang`).

**The sun** is computed on the CPU (`veach/render/atmosphere.hpp`: solar declination and hour angle from
`time_of_day`, `latitude`, `day_of_year`); its ground illuminance is `sun_illuminance` times the transmittance along
it, integrated with the LUT's integrator so light and sky agree. With `drives_sun` it replaces the scene's
directional lights (angular diameter 0.53°). Without a `Sky` entity the default sky aligns with the scene's
brightest directional light and scales its sun so the ground illuminance matches that light. A lit scene with
`mode = none` (or a black constant sky) logs one warning.

**Use.**

- Real-time renderer: pixels without geometry show the sky view plus the sun's disk; the sky-irradiance texel is the
  ambient term on diffuse surfaces; aerial perspective applies to the lit image.
- Photo mode: escaped camera rays see the sky view and the disk; escaped bounce rays see the sky view only (the sun
  is the directional light, sampled directly, so it is not counted twice); aerial perspective applies to camera hits.

## Consequences

- Outdoor scenes get sky light and haze by default; indoor test scenes set `Sky.mode = "none"` or `"constant"`.
- The sky view is at the camera's altitude: bounce rays far from the camera see the camera's sky (fine near the
  ground, wrong at flight altitude).
- **This is an Earth atmosphere seen from the ground.** Atmospheres driven by a planet's composition, their absence,
  and planets seen from orbit or space are a research-phase question (ARCHITECTURE.md, open question 7); this model
  is the starting point, not the answer.

## Alternatives considered

- **Bruneton 2008 (precomputed 4D scattering):** more accurate multiple scattering, much larger tables, a slow
  precomputation on every parameter change.
- **Analytic sky models (Preetham, Hosek-Wilkie):** no aerial perspective, no altitude, fit to fixed conditions.
- **Ray marching the atmosphere per pixel:** too costly per frame.
