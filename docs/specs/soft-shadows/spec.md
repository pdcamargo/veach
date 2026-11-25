# Soft shadows from lights with a size

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

Point and spot lights get a radius and directional lights an angular diameter, so their shadows have penumbrae.
Both renderers sample the light's extent with the same shared code: the path tracer stays the reference, and the
real-time renderer gets the penumbrae through ReSTIR's light samples and the denoiser, with no extra rays.

## Motivation

Emissive surfaces already cast soft shadows. Punctual lights cast hard ones, which read as artificial on anything
larger than a bulb at a distance: a window-sized spot, a lamp shade, the sun's 0.53 degrees. ReSTIR DI and the
denoiser ([temporal spec](../temporal-direct-lighting/spec.md)) now make sampling a light's extent cheap.
Backlog: VCH-021.

## Requirements

- `PointLight.radius` and `SpotLight.radius` (meters, default 0) make the light a sphere of that radius emitting
  uniformly, with the same intensity in candela: its radiance is intensity / (pi r^2), so lights at a distance look
  the same whatever their radius.
- `DirectionalLight.angle` (radians, the angular diameter, default 0) makes it a disk of that apparent size with
  the same illuminance.
- A radius or angle of 0 MUST give exactly the hard shadows of before (goldens unchanged).
- Both renderers MUST sample a light's extent through shared functions (`veach/lights.slang`); the spheres and disks
  are not geometry (camera and BSDF rays do not hit them).
- The real-time renderer MUST keep one shadow ray per pixel: a sized light's sample is a point (sphere) or direction
  (disk) in ReSTIR's reservoir, like an emissive triangle's point.

## API sketch

```luau
lamp:set(c.PointLight, { color = vector.one, intensity = 25, radius = 0.15 })
sun:set(c.DirectionalLight, { color = vector.one, illuminance = 100000, angle = math.rad(0.53) })
```

## Acceptance criteria

- **shadows.AC1**: A point light, a spot light and a directional light with a size, each over the Cornell box: the
  real-time renderer's direct light (256 frames averaged, no reuse) matches the path tracer's direct-light reference
  to a mean FLIP of at most 0.04, as the hard-shadow comparisons (rt.AC4) do.
- **shadows.AC2**: The penumbra is there: the same light with and without a size differ (mean FLIP at least 0.005)
  in both renderers.
- **shadows.AC3**: Size 0 changes nothing: every existing golden and comparison passes unchanged.

## Results

`tests/soft_shadows.py` (Metal, 320x180; sizes exaggerated to be measurable: sun 0.2 rad, point 0.2 m, spot 0.15 m):

| Light | Real time against the path tracer (AC1) | Size changes the image: real time / reference (AC2) |
|---|---|---|
| Directional | 0.0331 | 0.0522 / 0.0291 |
| Point | 0.0229 | 0.0267 / 0.0345 |
| Spot | 0.0182 | 0.0126 / 0.0107 |

Every golden and comparison is unchanged at size 0 (AC3). No extra rays: a sized light's sample is a point or a
direction in ReSTIR's reservoir, and the path tracer's NEE draws it with two more random numbers only when the light
has a size.

## Out of scope

- Lights you can see (spheres and disks drawn in the image): they stay invisible as punctual lights are.
- Rectangle and tube lights: emissive meshes cover them.

## Open questions

None.

## Links

- Backlog: VCH-021. ADRs: [0034 lights](../../adr/0034-lights-and-reference-path-tracer.md),
  [0041 ReSTIR DI](../../adr/0041-restir-di.md)
