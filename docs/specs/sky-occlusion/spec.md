# Large-scale sky occlusion in the real-time renderer

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

The sky's ambient light on a surface, less the sky that the scene around it hides: the buildings across a street, a
cave's ceiling, the walls of a dug hole, an awning. Screen-space ambient occlusion (ADR 0096) reaches 1.5 m and sees
only what is on screen; this reaches about 180 m from each surface and 200 m from the camera, by direction, from a
height map of the scene drawn from above. It also takes the open, sunlit ground out of the ambient table's light
where the surroundings are in shade. Backlog VCH-141.

## Motivation

The ambient table (ADR 0096) is the open sky over open, sunlit ground. So Bistro's facade at sunset, in the opposite
buildings' shadow, still saw the bright sky around the sun those buildings hide, and stayed warm; walls in a street got
the open field's ground bounce; a cave's walls got sunlit ground from below; the dug hole's walls were too bright. The
camera-wide `SkyVisibility` (ADR 0092) is one number for every surface near the camera. ADR 0039 rules out ray tracing.

## Requirements

- The sky's diffuse and specular ambient light MUST be occluded by direction: the sky hidden behind a building across a
  street, under a roof or a ceiling, and in a hole, and the ground's bounce where the ground is in shade.
- A facade in shadow at sunset MUST NOT receive the bright sky the buildings in front of it hide.
- What hides the sky MUST bounce light in its place by whether the sun reaches it (the sun shadow maps), with the sky's
  light where it does not.
- It MUST reach from the camera to a few hundred meters and fade to the open sky beyond, at s = 0.01 and s = 1.
- It MUST NOT need ray tracing and MUST NOT need TAA; a still camera MUST render the same image every frame, whatever
  frame the scene was last drawn from above in.
- The shading MUST still reach the ambient light through one call (`ambient_light`); the occlusion through one handle
  and one flag in the shade pass's parameters.
- Within its reach it MUST replace the camera's `SkyVisibility`; beyond it, `SkyVisibility` still applies.
- A camera MAY tune or turn it off; without a component it MUST be on.
- Its steady cost MUST stay under 0.5 ms at 1440p on the M5 Max.

## API sketch

```cpp
// On a camera (Scriptable); sky_distance 0 turns the large-scale occlusion off, enabled = false both.
world.entity("camera").set(veach::AmbientOcclusion{.sky_distance = 200.0f});
```

```lua
camera:set(c.AmbientOcclusion, { enabled = true, radius = 1.5, intensity = 1, sky_distance = 200 })
```

`VEACH_DISABLE_SKY_OCCLUSION=1` turns it off; `VEACH_SHOW_SKY_OCCLUSION=1` draws the ambient light alone on white
surfaces.

## Acceptance criteria

- **skyocc.AC1** — Lit by the sky alone, the foot of a wall in a street 6 m wide between blocks 16 m tall is darker
  with the occlusion than without; a block's wall facing 37 m of open ground is not.
- **skyocc.AC2** — The top of a box under a roof 4 m up loses the sky the roof hides; the box's side facing the sky
  under the roof's edge stays brighter than its top.
- **skyocc.AC3** — The back wall of a closed chamber 30 m deep is under half as bright with the occlusion as without.
- **skyocc.AC4** — A street 400 m from the camera (beyond the reach) renders as without the occlusion.
- **skyocc.AC5** — Two frames of a still camera are identical, and so is the frame after the camera went away and came
  back (the height map drawn again).
- **skyocc.AC6** — The Bistro goldens are regenerated and inspected; the facade at sunset reads as shadowed.

## Out of scope

- Occlusion beyond the reach (a valley seen from a mountain: a far-shadow and horizon term from coarse terrain
  heights, VCH-143).
- Light from the surroundings beyond one bounce of the sun or sky (GI), and local reflection probes (ADR 0062, VCH-072).
- What a height map cannot show: air under an overhang that overlaps a wall below it, several floors, a tunnel under a
  hill whose ceiling is not the hill's surface.

## Open questions

None.

## Links

- ADRs: [0101](../../adr/0101-large-scale-sky-occlusion.md), [0096](../../adr/0096-ambient-light-and-occlusion.md),
  [0092](../../adr/0092-aerial-perspective-follows-the-air.md), [0095](../../adr/0095-sun-shadows.md),
  [0039](../../adr/0039-realtime-renderer-is-raster-pbr.md)
- Backlog: VCH-141
- Measurements and screenshots: [docs/research/sky-occlusion](../../research/sky-occlusion/README.md)
- Code: `engine/render/src/sky_occlusion.cpp`, `engine/render/shaders/sky_occlusion.slang`,
  `engine/render/shaders/veach/sky_occlusion.slang`, `sky_ambient_occluded` in `engine/render/shaders/veach/ambient.slang`
