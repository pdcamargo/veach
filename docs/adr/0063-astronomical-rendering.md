# 0063: Astronomical rendering: per-body atmospheres, N suns, distant bodies, starfield, depth and exposure

Date: 2026-11-27

## Status

Accepted. Supersedes the scope of [0038](0038-physical-sky.md): Hillaire's model is kept and generalized to any
planet; the `Sky` component stays as the flat-world mode.

## Context

ADR 0038 is one Earth atmosphere seen from the ground: a fixed radius, altitude read as world Y, an Earth sun, LUTs that
break above the top. The toolbox needs planets from the ground, from orbit and from other bodies, atmospheres from
composition or none, several suns, eclipses, rings, gas giants, a generated starfield, depth from a 0.05 m near plane to
1e8 m, and exposure from starlight (about 1e-6 lux per star) to 100,000 lux, at any scale.
Research: [08-renderer](../research/08-renderer.md).

## Decision

**Per-body atmospheres: Hillaire generalized.**

- Each body with an atmosphere has an `AtmosphereRecord`: center (its body frame), ground radius, top, a density
  profile per component, coefficients, phase g and ground albedo, shared with drag
  ([0059](0059-gravity-flight-travel.md)). `space/` fills it from composition (Rayleigh from refractive index and number
  density, absorbers from cross-sections, Mie from an aerosol table); the renderer only consumes it.
- Local up is the direction from the body's center. Per body: transmittance (256 × 64) and multiple-scattering
  (32 × 32) LUTs, rebuilt when the record changes.
- For the camera's body, per frame: a sky-view LUT parameterized in **absolute azimuth around local up**, with all suns
  summed into one table, and an aerial-perspective volume that sums all suns as it marches (a 2D atlas until 3D
  textures, VCH-072).
- **From space** and for other bodies' atmospheres: a per-pixel ray march with the same LUTs, so the switch has no seam.
- **Scale** ([0049](0049-scale-model.md)): top, scale heights and cloud-layer heights follow s_a = √s_p, and
  coefficients by 1/s_a, so vertical optical depth (the sky's color) is unchanged. Human-scale haze: aerial
  perspective toward nearby geometry uses physical coefficients out to a distance tied to the scale height, then blends
  to the scaled ones. Its default is in the owner's packet (working default: on).

**N suns.** Sun directions, angular radii and per-sun visible fractions (eclipses) come from the system's queries at
extraction ([0048](0048-bodies-on-rails.md)); time of day falls out of geometry. An extra sun costs one more phase and
transmittance evaluation per sample, not a LUT pass. `Sky`'s `time_of_day`, `latitude` and `day_of_year` drive the sun
only in a scene without bodies.

**Distant bodies**, in three tiers chosen by projected size in pixels, which is scale-free:

1. **Point** below about 2 px, with the body's phase-dependent illuminance.
2. **Analytic sphere impostor:** a screen quad that ray-intersects the sphere, with maps baked from coarse terrain,
   the atmosphere ray march and rings, writing exact reverse-Z depth.
3. **Rasterized terrain** ([0053](0053-terrain-density-field.md)), using the same baked maps for color, with a
   dithered cross-fade over a band of projected sizes.

Gas giants are analytic spheres with banded albedo under the generic atmosphere; rings are an analytic annulus with a 1D
radial texture. Baked maps are caches keyed by generator version and seed.

**Starfield and deep sky.** The galaxy query ([0051](0051-staged-generation-pipeline.md)) returns near stars (direction,
distance, absolute magnitude, temperature). Each draws as an energy-conserving point whose integral is its illuminance,
E = E₀·10^(−0.4m) with E₀ ≈ 2.5e-6 lux. The rest of the galaxy is baked into a background cubemap on system change,
continuous across sectors. Visibility in daylight comes from a star boost in grading, never from faked exposure.

**Depth.** Reverse-Z, infinite far plane, float32 ([0013](0013-conventions-and-math.md)): about 0.01 m precision at
100 km and 12 m at 1e8 m. Everything the GPU sees is camera-relative
([0045](0045-large-world-positions-and-reference-frames.md)); far bodies are analytic. No log depth, no multi-frustum.

**Exposure.** A compute histogram, 128 bins over about 30 EV, percentile clipping, adaptation in stops per second. It
stays on the GPU; MCP reads it a frame late ([0030](0030-frame-and-sync-model.md)), so VCH-073 is not needed.
**Pre-exposure:** shading multiplies radiance by last frame's exposure before writing `rgba16_float`; the sun's disk
(about 1.6e9 nits) overflows fp16 otherwise. The physical camera of [0037](0037-exposure-and-agx.md) stays; a manual
`ev100` overrides, and an `AutoExposure` component sets range, speeds and compensation.

## Consequences

- One atmosphere technique from ground to orbit, any body, any scale, near today's cost.
- At strong compression limbs look thicker than real ones; the stylized target accepts it.
- Hillaire drifts for very dense atmospheres (Venus- or Titan-like); a known limit.
- Costs: atmosphere 0.17 to 0.25 ms measured on the M5 Max at 1080p; estimated about 1.0 ms on an M1-class GPU and 0.6
  ms on M3 Pro-class at 1440p; the ray march from space adds 0.1, 0.5 and 0.35 ms; the far layer 0.05, 0.3 and 0.2 ms.
- **Validation:** VCH-086 (SP-10): composition atmosphere, ground to orbit without a seam at s = 1 and 0.01, two suns
  and an eclipse, under 0.05 ms per extra sun on the M5 Max. VCH-090 (SP-14): auto-exposure from sunlit ground
  to a cave to space without clipping, and a cockpit, gas giant and moon with no depth fighting. VCH-079 (SP-03):
  clean screenshots at 1 km, 1 AU and 30 AU. VCH-080 (SP-04): correct sun noon elevations at s = 1 and 0.01.
- **Reopen if** Hillaire looks wrong next to photo mode's sky for atmospheres creators want (Bruneton), the s_a rule
  or haze band fails the eye test in VCH-086, or VCH-090 finds a depth or exposure case this does not cover.

## Alternatives considered

- **Bruneton precomputed scattering.** More accurate, but about 250 ms to rebuild and 8 MB per body.
- **Logarithmic depth.** Fragment depth disables early-z and defeats hidden-surface removal on TBDR.
- **Exposure read back to the CPU.** Stalls or lags; nothing on the CPU needs it.
