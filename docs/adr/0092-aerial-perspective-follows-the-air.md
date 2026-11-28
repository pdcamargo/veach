# 0092: Aerial perspective follows each ray through the air; caves are found by the camera's sky visibility

Date: 2026-11-28

## Status

Accepted. Amends [0063](0063-astronomical-rendering.md), [0076](0076-amendments-from-sp-10.md) (decisions 2 to 6) and
[0082](0082-amendments-from-sp-14.md) (rule 5). Built as VCH-135 ([spec](../specs/planet-atmosphere/spec.md),
[findings](../research/spikes/vch-135-atmosphere.md)).

## Context

0076 asked for an aerial-perspective volume whose depth follows the body, now that 3D textures exist
([0090](0090-rhi-texture-kinds-updates-and-raster-state.md)), and 0082 for occlusion so that caves and interiors get no
haze. The slice (SP-15) showed why: from orbit the ground had no veil, because the interim atlas ended 32 km from the
camera and the air begins thousands of km away. A froxel volume with one depth range per frame cannot serve both ends:
from orbit the air under a nadir pixel is 100 km thick at 15,000 km, so a frame-wide range puts it in one slice;
from the ground at s = 1 a horizontal ray crosses 1,130 km of air, and a cave wall sits 10 m away. The renderer core
is being settled in parallel (SP-16, [0060](0060-gpu-driven-visibility-buffer.md)), so the atmosphere must not depend
on how surfaces are drawn.

## Decision

1. **Compute passes producing textures, and two functions the shade pass calls.** The volume is a `d3` `rgba16_float`
   texture of one cell per 16 × 16 pixels and 32 slices (160 × 90 × 32 at 1440p), written by `aerial_volume.slang`:
   one thread per cell marches its ray once, one sample per slice, and writes every slice (in-scattering per lux in
   rgb, mean transmittance in a). Surfaces call `aerial_perspective(sky, direction, uv, distance)` and
   `sky_ambient_radiance(sky, distance)` (`veach/atmosphere.slang`); photo mode calls the first on camera hits. Either
   path of 0060 can use them.
2. **Each ray's slices span its own path through the air.** From where the ray enters the shell (the camera, inside)
   to where it meets the analytic ground, plus 2 % and 2 % of the shell's thickness for terrain below the ground radius.
   A ray that misses the ground ends at its lowest point from outside the atmosphere (so terrain standing above the
   ground radius at the limb lands at the same dense end as its neighbors), and from inside slides from that lowest
   point (at the ground's tangent) to the shell's exit (at the horizontal), so neighboring rays never jump. Slices are
   placed at x = ((1 − cos πw) / 2)², which goes as w⁴ near the camera (the first slice under a meter, the second under
   50 m, at s = 1) and crowds at the ground's end as well; the last slice holds the whole path, for what lies beyond.
   A surface reads the volume at its own fraction of its own ray's span (one trilinear sample), so cells agree on "how
   far toward the ground" where a grazing view makes neighboring rays meet the ground kilometers apart.
3. **Sky visibility is a property of the camera's surroundings, supplied as a component.** `veach::SkyVisibility
   {visibility, reach}` on a camera (Scriptable, visual only) says what fraction of the sky its surroundings leave open
   and how far they extend. Air and surfaces nearer than `reach` receive the sky's light times `visibility` (both the
   in-scattering marched into the volume and the sky's ambient light), rising to all of it at twice the reach: the
   chamber of a cave gets no haze and the haze comes back through the mouth. Gameplay writes it; the engine measures
   it with `physics::sky_visibility` (32 fixed cosine-weighted rays against static bodies and terrain colliders, the
   reach where three quarters of the blocked rays have hit, `RayFilter::static_only` so a craft or a character never
   counts); an interior may set it outright. The slice measures its camera against the surface scene each frame.
4. **Transmittance and multiple-scattering tables are cached per medium**: four slots, least recently used first
   (`render::AtmosphereLutCache`). A planet and its moon each keep theirs.
5. **The sky-view table wraps in azimuth** with a repeat sampler whose elevation coordinate is clamped to texel
   centers, so one sampler (0090 has one address mode per sampler) wraps one axis only.
6. **One sphere helper**: `sphere_span` (the stable quadratic, c from the altitude where the origin is the camera),
   `ray_sphere_at`, and `sphere_blocks` (the inward-ray shadow test) in `veach/atmosphere.slang`, with CPU twins in
   `<veach/render/atmosphere.hpp>`, tested at 1e4 km on the CPU and on the GPU. The transmittance and
   multiple-scattering tables use it too.
7. **Eclipses per sample, up to four occluders.** `PlanetSky::occluders` (center, radius, albedo, whether the sky draws
   it) reach every sample of the sky, the per-pixel march and the volume; a trig-free test rejects samples far from
   any eclipse. The space extractor passes the four bodies of largest angular size seen from the camera (never the
   star or the atmosphere's own body) and dims the sun's directional light by its visible fraction at the camera.
8. **The march from space defaults to 16 steps** (0076 decision 1: indistinguishable from 32 in every comparison).

## Consequences

- From orbit the sunlit disk sits under a veil that strengthens to the limb, at s = 0.01 and s = 1; from the ground
  distant terrain hazes into the horizon; a cave or the dug hole stays clear; one code path, no per-scale switch.
- Cost on the M5 Max at 2560 × 1440: the volume 0.05 to 0.11 ms (two thirds of SP-10's sky-view cost per pixel,
  far fewer pixels); applying it adds about 0.04 ms to the shade pass. The 32-slice volume differs from 64 slices by
  FLIP 0.001 to 0.003 in every slice view.
- Sky visibility is one value for the camera's surroundings, not per froxel: a cave seen from outside is hazed like
  open air (its air is a few meters of the view), and direct sunlight inside a cave still needs shadows (0062). Per-froxel
  visibility from a view of the scene above the camera arrives with shadows in the 0060 renderer, behind the same two
  functions.
- `physics::sky_visibility` sees colliders only: away from a physics scene (in flight, far terrain) the camera is in the
  open, which is right everywhere a player can be enclosed.
- The ambient light on distant surfaces is still the camera's sky irradiance (the interim renderer's), so a night side
  seen from the edge of the atmosphere is lit by the day sky; probes or SH ambient (0062) replace it.
- Human-scale haze (physical coefficients near the camera at compressed scale, 0063) is still unbuilt.

## Alternatives considered

- **Froxels at fixed distances (UE's default, 32 slices over ~100 km):** no depth resolution from orbit; the veil
  would be one slice.
- **One frame-wide depth range from the nearest to the farthest air:** about 100 km per slice from orbit at s = 1,
  the whole veil in one interpolation.
- **Reading each neighboring cell at the surface's absolute distance along that cell's ray** (classic froxels): tried;
  on grazing views neighboring rows meet the ground kilometers apart, and the haze banded in 16-pixel stripes.
- **A per-pixel march for every surface:** 1.46 ms per sun full screen at 1080p (SP-10).
- **Sky visibility from the screen's depth:** a camera looking at a cave's back wall cannot see the ceiling that hides
  the sky.
- **Sky visibility from rays through the terrain's density field:** misses the player's edits and buildings, and the
  procgen interpreter is slow at -O0; colliders already hold the edited ground near the player.
- **A sky-visibility map rendered from above the camera, per froxel:** the right long-term signal, but it is a
  geometry pass in the renderer core that 0060 and shadows (0062) define; this ADR's functions take it without change.
