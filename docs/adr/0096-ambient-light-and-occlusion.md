# 0096: The sky's ambient light per shaded point from an SH table per medium; GTAO at half resolution without history

Date: 2026-11-28

## Status

Accepted. Amends [0062](0062-shadows-ambient-reflections.md) (its ambient-light decisions) and replaces the ambient
model of [0040](0040-interim-realtime-path-visibility-buffer.md) that [0093](0093-gpu-driven-core.md) kept. Built as
VCH-140 ([spec](../specs/ambient-light/spec.md), [measurements and screenshots](../research/ambient-light/README.md)).

## Context

The real-time renderer lit every surface with the camera's sky averaged over an upward hemisphere: one texel, on the
diffuse lobe, unoccluded. Walls and roofs got the same blue, metals reflected nothing, creases and the slice's dug hole
read flat, and a night side seen from the day side's air was lit by the day sky (ADR 0092's consequences). ADR 0062
chose SH sky ambient projected from the camera's sky-view LUT every frame, and GTAO at half resolution with temporal
reuse. Two things changed since: the night-side case shows that any ambient taken from the camera's sky is wrong far
from the camera, and TAA (ADR 0061's history) is being built in parallel, so nothing here may depend on it. No real-time
GI and no ray tracing (ADR 0039).

## Decision

1. **The ambient table, per medium.** The sky's radiance around a point depends only on the medium, the point's
   altitude and the sun's zenith angle there, and is symmetric about the sun's vertical plane. `sky_ambient_lut`
   (`shaders/atmosphere.slang`) stores its L2 spherical harmonics in that point's frame (up, the sun's horizontal
   direction, their perpendicular): the 6 of 9 coefficients that are even across the sun's plane, rgb, for 64 sun
   zenith cosines by 16 altitudes (t² of the shell's thickness), in one `rgba32_float` texture of 384 × 16 (ADR 0094:
   twilight needs float32). Each entry is projected by a group of 64 threads from 512 directions through
   `planet_march` (single and multiple scattering, the planet's shadow, the analytic ground lit below the horizon),
   per lux of a sun of color 1. It is built with the medium's transmittance and multiple-scattering tables in
   `AtmosphereLutCache`'s slot, and again when a planet's ground albedo changes; never per frame. It replaces the
   1 × 1 irradiance texel and its per-frame pass.
2. **Evaluated per shaded point, per sun.** `ambient_light(sky, camera, AmbientSurface)` (`veach/ambient.slang`) finds
   the point's altitude and up from the planet's center, and for each sun its zenith cosine there, the table's entry,
   the sun's color and the fraction of its disk the occluders leave at that point. Diffuse: the SH convolved with the
   cosine lobe on the shading normal. Specular: the SH convolved with a Gaussian lobe as wide as the GGX lobe (variance
   4α², capped at the cosine lobe) toward its dominant direction, times the split-sum environment BRDF (Karis's fit);
   the diffuse term is scaled by one minus that specular albedo. The flat-world `Sky` uses the same path (its planet
   block is a sphere of 6,360 km below the camera); a constant sky is uniform radiance; no sky, no ambient light. The
   camera's sky visibility (ADR 0092) still scales the result near the camera.
3. **GTAO from depth alone, at half resolution, without history** (`shaders/ambient_occlusion.slang`,
   `src/ambient_occlusion.cpp`), recorded after the visibility pass and before shading:
   - a pyramid of view distance (5 mips, XeGTAO's depth-weighted filter), read at each sample's texel center;
   - the trace at half resolution (the full-resolution pixel at twice its coordinates): normals rebuilt from the
     depths 2 pixels away, 2 slices of 3 samples each way, a fixed 4 × 4 Bayer pattern of slice angles; the
     visibility is stored unclamped, and a side with nothing above its tangent plane takes the tangent exactly;
   - two edge-aware 3 × 3 blurs at half resolution (XeGTAO's depth edges, made symmetric);
   - an upsample to full resolution by depth (the 4 nearest, weighted by depth within 2 % of the distance), then the
     clamp, the power and a floor of 0.03.
   The pattern never changes, so a still camera's image never changes; temporal reuse waits for TAA's history.
4. **Applied to ambient light only.** Diffuse ambient times Jimenez's multi-bounce of the occlusion over the base
   color; specular ambient times Lagarde's specular occlusion with multi-bounce over F0. Direct light never sees it.
5. **One seam.** The shade pass calls `ambient_light` once per surface pixel; the occlusion reaches it as one texture
   handle and a flag (`shade_ambient_occlusion`) in `ShadeParams`; the frame blackboard holds it as
   `ambient_occlusion`. `veach::AmbientOcclusion { enabled, radius = 1.5 m, intensity = 1 }` on a camera tunes it
   (Scriptable; on without it). `VEACH_SHOW_AO=1` draws the occlusion instead of the image.

## Consequences

- Walls see the lit ground and look neutral beside blue roofs; metals reflect the sky; creases, window recesses, the
  dug hole and a cave's corners darken; a night side gets the night's ambient light (its twilight, then nothing)
  whatever the camera's sky is. Every real-time golden that has a sky changed and was regenerated.
- The table costs nothing per frame; its build, once per medium, took 1.9 ms on a cold first frame on the M5 Max.
- GTAO costs 0.34 to 0.68 ms at 1080p and 0.53 to 1.20 ms at 1440p on the M5 Max (Bistro, the SP-16 field, the slice;
  the trace about 60 %), measured while other work shared the GPU. Shading plus the removed irradiance pass (one
  thread, 0.11 to 0.25 ms) moves by -0.05 to +0.15 ms. [Measurements](../research/ambient-light/README.md). That is
  well above ADR 0062's estimate (0.1 ms at 1080p with temporal reuse); the levers (history, deinterleaving, half
  floats, a lighter pyramid) are VCH-141, and M1-class numbers (VCH-097) decide how far they must go.
- No large-scale sky occlusion: the table is the open sky and the analytic ground, and GTAO reaches 1.5 m. In a
  street the walls get the open ground's bounce, and with sun shadows (0095) a facade shadowed at sunset still gets
  the bright sky around the sun that the street's buildings hide (VCH-141).
- Screen-space limits: no occlusion from what is off screen or behind the front layer; thin foliage gets its
  neighbors' occlusion at half resolution; a cave is lit by the outdoor sky's SH times the camera's sky visibility, so
  its walls get "ground" light from below: local probes (ADR 0062) remain the answer.
- Above the atmosphere the table clamps to the shell's top: a ship in high orbit sees the planet as from the top of
  the air (VCH-141).
- Revisit if: TAA's history lands (rotate the pattern per frame, more slices for the same cost), M1-class measurements
  (VCH-097) put the trace over budget, or the SH misses sharp sky features that matter (a prefiltered cube map then
  takes the specular term, VCH-072).

## Alternatives considered

- **SH from the camera's sky-view LUT each frame (ADR 0062 as written):** right near the camera only; the night side
  seen from the day air is the case it gets wrong.
- **A small irradiance cube per frame:** same flaw (one sky for the whole screen), and needs cube maps (VCH-072).
- **Evaluating the sky per pixel by marching:** dozens of directions per pixel; the table gives the same answer once.
- **GTAO at full resolution:** 2.2 ms at 1440p for 3 slices × 4 steps on the M5 Max in the first measurement; half
  resolution with a depth-guided upsample costs a quarter and looked the same in Bistro and the field.
- **Temporal reuse now, without TAA:** a history buffer, reprojection and rejection of its own; TAA will provide them.
- **Normals from the visibility buffer for GTAO:** exact (normal maps aside) but a vertex fetch and interpolation per
  pixel in a pass that otherwise reads depth only; depth normals are what XeGTAO uses.
- **Clamping and the power per pixel before the blur (XeGTAO's order):** with two slices a pixel's estimate of an open
  plane seen at an angle ranges around 1; clamping first darkened open walls by their angle to the view.
