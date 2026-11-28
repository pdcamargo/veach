# Ambient light and occlusion in the real-time renderer

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

The sky's light on surfaces that the sun does not reach, done properly for a raster renderer with no ray tracing: the
diffuse and specular light of the sky around each shaded point (not around the camera), as a function of the surface's
normal and the air it sits in, darkened by screen-space ambient occlusion in creases, holes and against walls. Item 6
of the renderer pass; backlog VCH-140.

## Motivation

Until now the real-time renderer (ADR 0040's model, kept by ADR 0093) lit every surface with one number: the camera's
sky, averaged over an upward hemisphere, on the diffuse lobe only, unoccluded. So creases, the slice's dug hole and
cave walls read flat; walls and upward faces got the same blue light; metals reflected nothing and looked black; and a
night side seen from high in the day side's air was lit by the day sky, because the light came from the camera's sky
(ADR 0092's consequences). ADR 0062 asked for SH sky ambient and GTAO; ADR 0039 rules out real-time GI and ray tracing.

## Requirements

- The sky's diffuse ambient light MUST depend on the shading normal: a wall MUST see the ground's half of the sky, an
  upward face the zenith.
- It MUST be the sky at the shaded point: its altitude and each sun's angle there, so a point where the sun has set MUST
  get the night's light whatever the camera's sky is; every sun MUST count, with the fraction of its disk the occluders
  leave at that point.
- The sky's specular ambient light MUST come from the same source, with a roughness-aware lobe and the split-sum
  environment BRDF, so metals and grazing dielectrics reflect the sky.
- The data MUST be built per medium (and its ground albedo) when it changes, not every frame, and cached with the
  medium's other tables (ADR 0092's cache).
- Screen-space ambient occlusion MUST darken the ambient light (diffuse and specular, with interreflection) and MUST
  NOT touch direct light. It MUST work from the visibility pass's depth alone (no normals target), MUST NOT need TAA
  or any temporal history, and a still camera MUST render the same image every frame.
- It MUST NOT darken open surfaces and MUST NOT blur across depth edges.
- The camera's sky visibility (ADR 0092) MUST still scale the ambient light near the camera.
- The shading MUST reach all of it through one call (`ambient_light`), and the occlusion through one texture and one
  flag in the shade pass's parameters.
- A camera MAY tune or turn off the occlusion with a component; without it the occlusion MUST be on.
- Nothing MAY need ray tracing.

## API sketch

```cpp
// On a camera (Scriptable); without it: on, 1.5 m, intensity 1.
world.entity("camera").set(veach::AmbientOcclusion{.enabled = true, .radius = 1.5f, .intensity = 1.0f});
// RenderView::ambient_occlusion carries it to the renderer; the frame blackboard's "ambient_occlusion" holds the
// texture (r16_float, the visible fraction) for features recorded later.
```

```slang
// veach/ambient.slang: the shading's one call.
AmbientSurface a;  // position, normal, view, diffuse, f0, roughness, occlusion, distance
light += ambient_light(sky, camera_position, a);
```

```lua
-- Luau
camera:set(c.AmbientOcclusion, { enabled = true, radius = 2.0, intensity = 1.0 })
```

`VEACH_SHOW_AO=1` draws the occlusion in grey instead of the lit surfaces (debugging).

## Acceptance criteria

- **ambient.AC1** — Lit by the sky alone, the roof of a grey box is bluer than its walls, and the walls are lit.
- **ambient.AC2** — Under a camera whose sun is 5 degrees up, a box 1,500 km away where the sun is 8 degrees down is
  under a tenth as bright as one 1,500 km toward the sun.
- **ambient.AC3** — Lit by the sky alone, a smooth metal box's roof and walls are not black.
- **ambient.AC4** — Ambient occlusion darkens the wall next to and above a box standing against it, leaves the open wall
  unchanged, and `AmbientOcclusion { enabled = false }` turns it off.
- **ambient.AC5** — With no sky and a sun, the image with occlusion equals the image without; two frames of a still
  camera are identical.
- **ambient.AC6** — Every real-time golden is regenerated with the new ambient light and inspected; the flight helmet
  renders the same with ray tracing disabled.

## Out of scope

- Temporal accumulation of the occlusion (more slices for free once TAA's history exists, ADR 0061) and the trace's
  cost on M1-class GPUs: VCH-141.
- Local reflection probes, a prefiltered sky cube map and screen-space reflections (ADR 0062): VCH-141.
- Shadows (cascades are a parallel item), planetshine and moonlight, emissive surfaces lighting others.

## Open questions

None.

## Links

- ADRs: [0095](../../adr/0095-ambient-light-and-occlusion.md), [0062](../../adr/0062-shadows-ambient-reflections.md),
  [0092](../../adr/0092-aerial-perspective-follows-the-air.md), [0093](../../adr/0093-gpu-driven-core.md),
  [0039](../../adr/0039-realtime-renderer-is-raster-pbr.md)
- Backlog: VCH-140, VCH-141
- Measurements and screenshots: [docs/research/ambient-light](../../research/ambient-light/README.md)
- Code: `engine/render/shaders/veach/ambient.slang`, `engine/render/shaders/ambient_occlusion.slang`,
  `engine/render/src/ambient_occlusion.cpp`, `sky_ambient_lut` in `engine/render/shaders/atmosphere.slang`
