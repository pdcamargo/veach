# Planet atmosphere: aerial perspective from orbit to a cave

**Status:** Implemented
**Area:** render, physics, space
**Owner:** dev lead

## Summary

A planet's air in front of every surface, from orbit to the ground and into a cave: aerial perspective in a camera
froxel volume whose depth follows the body, occluded where the camera's surroundings hide the sky, with eclipses
evaluated per sample and the medium's tables cached per body. It replaces SP-10's interim atlas (32 km deep) and
completes the atmosphere decisions of [ADR 0063](../../adr/0063-astronomical-rendering.md),
[0076](../../adr/0076-amendments-from-sp-10.md) and [0082](../../adr/0082-amendments-from-sp-14.md).

## Motivation

From orbit a real planet's whole sunlit disk sits under a blue veil, strongest toward the limb. The first slice
(SP-15) drew only the thin blue rim, because the ground was drawn without the air in front of it: the atlas stopped
32 km from the camera, and from orbit the air starts thousands of km away. SP-14 found the opposite failure inside a
cave: unoccluded aerial perspective filled the chamber with haze brighter than its walls.

## Requirements

- Aerial perspective MUST be a compute pass that writes a 3D texture (ADR 0090) and a function the shade pass calls
  per surface (`aerial_perspective` in `veach/atmosphere.slang`), so the real-time renderer of ADR 0060 can call it
  from either of its paths. Photo mode MUST call the same function on camera hits.
- The volume's depth range MUST follow the body: each ray's slices span its own path through the atmosphere, from
  where it enters the shell to the ground, so the same volume covers hundreds of km from orbit at s = 1 and the whole
  10 km atmosphere at s = 0.01.
- Air and surfaces within reach of the camera's surroundings MUST receive the sky's light times their sky visibility
  (ADR 0082 rule 5): no haze inside a cave or an interior, the haze back beyond them. The visibility comes from a
  `SkyVisibility` component on the camera; without one the camera is in the open.
- The engine MUST offer a way to measure sky visibility from static geometry (`physics::sky_visibility`).
- Transmittance and multiple-scattering tables MUST be built only when a medium is new or was evicted from a small
  per-medium cache.
- The sky-view table MUST wrap in azimuth.
- Every sphere test in the atmosphere MUST go through one numerically stable helper with a CPU twin.
- Up to four occluder spheres MUST eclipse every sun per sample in the sky and in the volume.
- The existing goldens MUST pass unchanged, or be regenerated on purpose and inspected.

## API sketch

```cpp
// On a camera (Scriptable): how open its surroundings are to the sky.
camera.set(veach::SkyVisibility{.visibility = 0.0f, .reach = 12.0f});   // a cabin: no haze within 12 m
// Measured from static bodies and terrain colliders (32 cosine-weighted rays):
physics::SkyOpenness o = physics::sky_visibility(physics, scene, eye_in_scene, up);
// A module's sky (ResolvedSky::planet): occluders eclipse the suns per sample.
sky.occluders[0] = {.center = moon_km, .radius = 1737.0f};
sky.occluder_count = 1;
```

```luau
camera:set(c.SkyVisibility, { visibility = 0, reach = 10 })
```

## Acceptance criteria

- **atmosphere.AC1** — The ray-sphere helper keeps both roots within 2 m at 1e4 km from the center in float
  kilometers, keeps a 1.3 m altitude's near root within 1e-4 where `|o|^2 - R^2` loses it, never shadows a ground
  point rounded under the surface toward a sun above its horizon, and the shader's helper matches its CPU twin on the
  GPU.
- **atmosphere.AC2** — The volume's slice mapping is invertible and monotonic; with 32 slices over a 1,130 km span the
  first slice is under 1 m from the camera, the second under 50 m, and the last within 0.5 % of the span's end.
- **atmosphere.AC3** — A medium's tables are built once while it is in use; returning to a cached medium builds
  nothing; more media than slots evict the least recently used.
- **atmosphere.AC4** — `physics::sky_visibility` reports an open sky on open ground and under a dynamic body, and a
  mostly closed one under a static roof with a reach of a few meters; a camera's `SkyVisibility` reaches its
  `RenderView`, and a camera without one is in the open.
- **atmosphere.AC5** — By eye, in `veach-slice` at s = 0.01 and s = 1 (screenshots in
  [findings](../../research/spikes/vch-135-atmosphere/findings.md)): the sunlit disk from orbit under a veil that
  strengthens toward the limb, with no seam or band at the limb; haze on distant terrain from 200 m; no haze inside the
  dug hole; a sunset. In SP-14's cave, no haze in the chamber and the haze back through its mouth.
- **atmosphere.AC6** — The real-time and photo-mode goldens pass unchanged.

## Out of scope

- Human-scale haze (ADR 0063, 0049): physical coefficients near the camera at compressed scale. VCH-109 owns the
  creator controls; the band itself is a follow-up in this item's findings.
- Per-froxel occlusion from geometry (a sky-visibility map from above, volumetric sun shadows): with shadows
  ([ADR 0062](../../adr/0062-shadows-ambient-reflections.md)) in the renderer of ADR 0060.
- Other bodies' atmospheres seen from a planet (a neighbor's limb): one atmosphere per frame for now.
- Photo mode's planet sky (the path tracer's sky call is near Metal's compiler limit, ADR 0076).

## Open questions

None.

## Links

- ADRs: [0063](../../adr/0063-astronomical-rendering.md), [0076](../../adr/0076-amendments-from-sp-10.md),
  [0082](../../adr/0082-amendments-from-sp-14.md), [0090](../../adr/0090-rhi-texture-kinds-updates-and-raster-state.md),
  [0092](../../adr/0092-aerial-perspective-follows-the-air.md)
- Backlog: VCH-135
- Guides: [rendering](../../guides/rendering.md), [space realization](../../guides/space-realization.md),
  [Luau API](../../guides/luau-api.md)
