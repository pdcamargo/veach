# 0076: Amendments from SP-10: the march's cost, eclipses per sample, a body-sized aerial-perspective volume

Date: 2026-11-27

## Status

Accepted. Amends [0063](0063-astronomical-rendering.md). Validated by SP-10 (VCH-086,
[findings](../research/spikes/sp-10-atmosphere.md)). The scale rule of [0049](0049-scale-model.md) is not changed
here: its sunset question is with the owner (the findings' review packet).

## Context

SP-10 rendered a per-body atmosphere from a composition record (`space::AtmosphereRecord`) around any center, behind
an optional `ResolvedSky::planet`, from the ground to orbit, Earth- and Mars-like, at s = 1 and 0.01, with two suns
and an eclipse. The sky-view table inside the atmosphere and the per-pixel march outside it meet without a visible
seam (mean FLIP 0.004 to 0.009). On the ground the atmosphere costs 0.13 ms on the M5 Max at 1080p, and each extra
sun 0.019 ms; the multiple-scattering table (0.19 ms) is rebuilt only when a medium changes. Three of 0063's
assumptions did not hold.

## Decision

1. **The march from space is budgeted at 0.2 to 0.6 ms, not 0.1**, on the M5 Max at 32 steps (0.6 ms just above the
   top of the atmosphere). It ships with an adaptive step count (16 steps looked the same in every comparison) and
   may drop to half resolution when a limb fills the screen.
2. **Eclipses are evaluated per sample.** Occluders reach the sky shader as spheres (center and radius per sun), and
   each scattering sample tests them. One visible fraction per sun, from extraction, remains the input for lighting
   surfaces near the camera; it is wrong for the sky, where the far air outside the umbra stays lit and from orbit
   the umbra is a spot.
3. **Aerial perspective lives in a 3D froxel volume whose depth follows the body** (hundreds of km from orbit, the
   whole 10 km atmosphere at s = 0.01), replacing the atlas's fixed 32 km. This needs 3D textures (VCH-072), which
   clouds need anyway.
4. **Transmittance and multiple-scattering tables are cached per body** and rebuilt only when its medium changes.
5. **The sky-view table wraps in azimuth.** Its azimuth is absolute, so its two edges are the same direction; it uses
   a wrapping sampler, not a clamp.
6. **Ray-sphere tests share one helper** using the numerically stable quadratic and the inward-ray shadow test, tested
   at 10⁴ km; float kilometers fail from orbit at s = 1 without both.

## Consequences

- The atmosphere from orbit costs more of the 1440p60 budget on the M3 Pro than planned; SP-15 measures it there.
- Photo mode cannot render a planet's sky until the path tracer's sky call is split from its kernel, which is near
  Metal's compiler limit; tracked with photo mode's planet support.
- The analytic ground and far-terrain impostors need skylight (Mars and Titan grounds render too dark without it).
- The human-scale haze band of 0063 and 0049 is still unbuilt and untested.

## Alternatives considered

- **A sky-view table from space too:** it cannot represent the limb's thin shell at orbit resolution; the march is
  what removes the seam.
- **One dimming factor per sun for the sky:** cheap, and visibly wrong for partial and orbital views of an eclipse.
- **A larger fixed atlas depth:** wastes resolution at small s and still runs out from orbit.
