# Twilight: the banded sky

[ADR 0093](../../../adr/0093-twilight-precision.md), [spec](../../../specs/planet-atmosphere/spec.md) atmosphere.AC7.
From the sun a few degrees below the horizon to the end of twilight, the slice drew the planet's sky in hard
concentric bands with stepped edges (VCH-136, first seen in [VCH-137's screenshot](../space-backdrop/day_s1_twilight_sun_-10.png)).
Metal on the M5 Max, 2560 x 1440, release build, slice autopilot with `VEACH_SLICE_SHOTS`, whose tour now ends with
the sun 18 degrees down.

## The evening, before (top) and after (bottom)

Sun 2 degrees up, then 3, 6, 10 and 18 down. Sunset looks toward the sun along the horizon; the others look away from
it, 30 degrees up.

s = 1: ![](slice_s1_evening.png)

s = 0.01: ![](slice_s0.01_evening.png)

| View | s = 1 | s = 0.01 |
|---|---|---|
| Sun 2 degrees up | [before](s1_sun_2_before.png), [after](s1_sun_2_after.png) | [before](s0.01_sun_2_before.png), [after](s0.01_sun_2_after.png) |
| 3 down | [before](s1_sun_-3_before.png), [after](s1_sun_-3_after.png) | [before](s0.01_sun_-3_before.png), [after](s0.01_sun_-3_after.png) |
| 6 down | [before](s1_sun_-6_before.png), [after](s1_sun_-6_after.png) | [before](s0.01_sun_-6_before.png), [after](s0.01_sun_-6_after.png) |
| 10 down | [before](s1_sun_-10_before.png), [after](s1_sun_-10_after.png) | [before](s0.01_sun_-10_before.png), [after](s0.01_sun_-10_after.png) |
| 18 down | [before](s1_sun_-18_before.png), [after](s1_sun_-18_after.png) | [before](s0.01_sun_-18_before.png), [after](s0.01_sun_-18_after.png) |

- **s = 1, 10 down:** five flat bands, blue and purple by turns, become one gradient from deep blue overhead to a
  pale horizon. At 3 and 6 down the faint stripes in the upper sky are gone. At 18 down the sky was exactly zero;
  now it is a faint purple ([lab view at EV100 −11](lab_s1_sun_-18_ev-11.png), before and after), still black next
  to the stars at the slice's exposure, as a real sky is.
- **s = 0.01, 6 to 18 down:** the planet's shadow rising opposite the sun (the dark blue band) had a stepped edge with
  arcs above it; now its edge is soft and the air above it smooth.

## Two causes

Each was confirmed by fixing it alone (SP-10's lab view, `VEACH_SP10_*`, the twilight goldens' setup):

1. **Half floats.** The sky-view and multiple-scattering tables hold luminance per lux of sun. Opposite the sun 10
   degrees down that is a few 1e-7, all multiple scattering (the air in view is in the planet's shadow), under
   rgba16_float's smallest normal (6.1e-5): its denormals step by 6e-8, five or six levels per channel. Sky view in
   float32 alone removed the bands; multiple scattering in float32 too changed the image by a further mean FLIP of
   0.045; transmittance in float32 by 0.002, so it stays half.
2. **Whole steps lit or not.** The march took the sun's light at each step's middle. At s = 0.01 the shadow's edge
   crosses lit air near the camera, and each of the 16 steps switched as a whole: arcs, gone at 128 steps. Lit by the
   part of the step outside the shadow, 16 steps look like 128, and at 128 steps it is within 0.002 of 512 where the
   old test was 0.012 off.

## Nothing else moved

Mean FLIP between the tours before and after: 0.0003 to 0.003 for every view from orbit to the dug hole at both scales
(the hole at s = 1: 0.0097, its dark walls), and the sunset 0.002. SP-10's eclipse views match the
[VCH-135 screenshots](../vch-135-atmosphere.md) (0.00004 from the ground, 0.0004 from orbit), SP-14's cave its back
wall (0.005). The Bistro and helmet goldens pass.

## GPU cost

Release, 1920 x 1080, SP-10's lab, median of three rounds of 40 frames, ms:

| View | Pass | Before | After |
|---|---|---|---|
| Ground, noon | sky view | 0.026 | 0.033 |
| | aerial perspective | 0.028 | 0.036 |
| | shade | 0.092 | 0.098 |
| Ground, twilight (10 down) | sky view | 0.015 | 0.017 |
| | aerial perspective | 0.027 | 0.029 |
| | shade | 0.100 | 0.109 |
| Orbit (every pixel marches) | sky view | 0.015 | 0.018 |
| | aerial perspective | 0.038 | 0.046 |
| | shade | 0.205 | 0.254 |

The float32 volume and tables cost about 0.01 ms; the rest is the lit fraction in every march step. Variants that
sampled the sun at the step's middle, or kept the shadow spans in an array, were no cheaper.

## Reproduce

```sh
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out build/release/apps/slice/veach-slice \
  --script scripts/slice_s1.luau --rhi metal --headless --frames 30000   # also slice_s0.01.luau; from apps/slice
ctest --preset debug -R "planet sky at twilight"
```
