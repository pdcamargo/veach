# Physical sky and atmosphere

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

Every scene gets a physically based sky by default: an Earth-like atmosphere (Rayleigh and Mie scattering, ozone)
rendered from lookup tables after Hillaire 2020, in photometric units. The sun's direction comes from a time of day
and its illuminance from the atmosphere's transmittance; the sky lights the real-time ambient stopgap and is the
path tracer's environment, so the two renderers stay comparable. Time of day and the atmosphere's parameters are a
component, settable from Luau and MCP.

## Motivation

A scene without a sky rendered its shadows black and its background a flat color (Bistro, 2026-11-25). The sky is
also the main light source Candela (VCH-017) will gather, and aerial perspective (haze with distance) is part of how
outdoor scenes read. Roadmap: VISION's rendering pillar; backlog VCH-019.

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
- Both renderers MUST use the same sky: the real-time renderer's background and ambient stopgap, and the path
  tracer's escaped rays (camera rays also see the sun's disk); aerial perspective MUST apply to both.
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
- **sky.AC3**: Bistro at noon and at sunset: the real-time renderer's image against the path tracer's (direct light
  plus sky light at the first hit, `PathTracer.mode = 3`) within the tolerance of ADR 0047, baselines recorded here.
- **sky.AC4**: Bistro at noon and at sunset have goldens (real-time, Metal).
- **sky.AC5**: `Sky` is settable over MCP and from Luau: changing `time_of_day` moves the sun in the next frame.
- **sky.AC6**: A scene with lights and `Sky.mode = "none"` logs `render: the scene has lights but no sky` once.

## Results

| Criterion | Test | Result |
|---|---|---|
| sky.AC1 | `unit: [sky.AC1] ...` | zenith at 12:00, horizon at 18:00 within 1 degree, west is -X, `north` turns the frame |
| sky.AC2 | `unit: [sky.AC2] ...` (x2), `mcp: sky set over MCP` | GPU and CPU transmittance within 1% at 24 points, two atmospheres; sunset sun red/blue > 3; noon sky mean blue 2.8x red |
| sky.AC3 | `sky.AC3: bistro_noon/bistro_sunset final image within ADR 0047` | see below |
| sky.AC4 | `golden: bistro at noon/sunset (realtime)` | `tests/golden/bistro_noon.png`, `bistro_sunset.png` (640x360, 32 frames) |
| sky.AC5 | `mcp: sky set over MCP` | `time_of_day` 12:30 -> 23:00 over MCP: the next frame's zenith goes from 0.245 to 0.042 (what is left is the street's wires and lamps) |
| sky.AC6 | `lab: a scene with lights and no sky warns` | Cornell box (`Sky.mode = "none"`) |

**sky.AC3 baselines** (ADR 0047, kept in `tests/tolerance.json` per ADR 0052; `compare_direct.py --sky --final`, 640x360, 1024 spp reference, 64 real-time
frames). The real-time side runs the ambient stopgap with unlimited range, which is what mode 3 computes; its
default 1 m range lights street canyons as if they saw the whole sky, a stopgap trade until Candela.

| Scene | Mean FLIP | Limit | Luminance | Limit |
|---|---|---|---|---|
| bistro_noon | 0.0312 | 0.0328 | -2.37% | 3% (exception, VCH-064) |
| bistro_sunset | 0.0406 | 0.0426 | -8.28% | 9% (exception, VCH-064) |

The luminance exceptions are the trees' string-light bulbs: the path tracer shows them sky-lit, the real-time
image does not (VCH-064): about 2% of the pixels, nearly all of those above 0.5 FLIP. Before the stopgap saw the sky per
direction, it applied the upward irradiance to every normal: +35% and +214%.

## Out of scope

- Clouds, volumetric fog (VCH-020), a planet seen from space, night sky and moon.
- Sky reflections in specular surfaces (the reflections milestone, VCH-018).

## Open questions

None.

## Links

- ADRs: [0050 the atmosphere](../../adr/0050-physical-atmosphere.md), [0034 lights](../../adr/0034-lights-and-reference-path-tracer.md)
- Backlog: VCH-019. Reference: S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
  Technique", EGSR 2020.
