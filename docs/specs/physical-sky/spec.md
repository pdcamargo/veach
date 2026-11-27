# Physical sky and atmosphere

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

Every scene gets a physically based sky by default: an Earth-like atmosphere (Rayleigh and Mie scattering, ozone)
rendered from lookup tables after Hillaire 2020, in photometric units. The sun's direction comes from a time of day
and its illuminance from the atmosphere's transmittance. The real-time renderer and photo mode read the same sky,
so the two stay comparable. Time of day and the atmosphere's parameters are a
component, settable from Luau and MCP.

## Motivation

A scene without a sky rendered its shadows black and its background a flat color. The sky is the main light
source outdoors, and aerial perspective (haze with distance) is part of how outdoor scenes read. This is an
Earth-at-ground-level model; atmospheres driven by a planet's composition, and seen from orbit, are a
research-phase question ([ARCHITECTURE.md](../../ARCHITECTURE.md#open-questions-research-phase), VCH-070).

## Requirements

- A `Sky` component (on any one entity; none means the defaults) MUST choose the environment: `physical` (default),
  `constant` (a radiance in nits) or `none` (black).
- The physical sky MUST follow Hillaire 2020: a transmittance LUT, a multiple-scattering LUT, a sky-view LUT at the
  camera's altitude and an aerial-perspective volume, with Rayleigh, Mie and ozone coefficients in physical units
  and luminance in nits for a sun illuminance in lux at the top of the atmosphere.
- The sun MUST come from the sky: with a `Sky` (and `drives_sun`, default true) its direction follows `time_of_day`,
  `latitude`, `day_of_year` and `north`, its illuminance is the top-of-atmosphere illuminance times the transmittance
  toward it, and it replaces the scene's directional lights. Without a `Sky`, the default sky follows the scene's
  brightest directional light (its direction and ground illuminance).
- Both renderers MUST read the same sky: the real-time renderer for its background (with the sun's disk), its
  ambient term (the sky-irradiance texel) and aerial perspective; photo mode for its escaped rays (camera rays also
  see the sun's disk) and aerial perspective on camera hits.
- A scene with lights and no sky (`none`, or a black constant) MUST log a warning once: nothing then lights what
  the lights do not reach.
- Time of day and atmosphere parameters MUST be settable from Luau and MCP (the component is Scriptable).

## API sketch

```luau
veach.spawn("sky"):set(c.Sky, { time_of_day = 18.5, latitude = 48.9, rayleigh = 1.0, mie = 1.5 })
camera:set(c.Sky, { mode = "none" })  -- indoor test scenes
```

## Acceptance criteria

- **sky.AC1**: The sun's position: at latitude 0 on an equinox it is at the zenith at 12:00 and on the horizon at
  18:00 (within 1 degree); north is -Z rotated by `north`.
- **sky.AC2**: The atmosphere's transmittance computed on the CPU (for the sun's illuminance) and on the GPU (the
  LUT) agree within 1%; the noon sky is blue (blue over red at the zenith) and the sun at sunset is red (its
  transmittance red over blue by at least 3x).
- **sky.AC3**: Removed at the reset ([ADR 0001](../../adr/0001-reset.md)): it compared the old real-time lighting
  with the path tracer under an image-tolerance policy that no longer exists.
- **sky.AC4**: Bistro at noon and at sunset have goldens (real-time, Metal).
- **sky.AC5**: `Sky` is settable over MCP and from Luau: changing `time_of_day` moves the sun in the next frame.
- **sky.AC6**: A scene with lights and `Sky.mode = "none"` logs `render: the scene has lights but no sky` once.

## Results

| Criterion | Test | Result |
|---|---|---|
| sky.AC1 | `unit: [sky.AC1] ...` | zenith at 12:00, horizon at 18:00 within 1 degree, west is -X, `north` turns the frame |
| sky.AC2 | `unit: [sky.AC2] ...` (x2), `mcp: sky set over MCP` | GPU and CPU transmittance within 1% at 24 points, two atmospheres; sunset sun red/blue > 3; noon sky mean blue 2.8x red |
| sky.AC4 | `golden: bistro at noon/sunset (realtime)` | `tests/golden/bistro_noon.png`, `bistro_sunset.png` (the full test set) |
| sky.AC5 | `mcp: sky set over MCP` | `time_of_day` 12:30 -> 23:00 over MCP: the next frame's zenith goes from 0.245 to 0.042 (what is left is the street's wires and lamps; measured before the reset) |
| sky.AC6 | `lab: a scene with lights and no sky warns` | Cornell box (`Sky.mode = "none"`) |

## Out of scope

- Clouds, volumetric fog, a planet seen from space, atmospheres of other compositions, night sky and moon: the
  research phase (VCH-070).
- Sky reflections in specular surfaces.

## Open questions

None.

## Links

- ADRs: [0038 the atmosphere](../../adr/0038-physical-sky.md), [0036 lights](../../adr/0036-lights-in-photometric-units.md), [0041 photo mode](../../adr/0041-photo-mode.md)
- Backlog: VCH-019. Reference: S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
  Technique", EGSR 2020.
