# Bloom: findings (VCH-148)

What bloom does to the scenes it was built for, the settings it ended with and what it costs.
[Spec](../../specs/bloom/spec.md), [ADR 0099](../../adr/0099-bloom.md). Apple M5 Max, Metal, release builds,
2026-11-28. "Before" is `main` at `5e68174` (temporal AA, ambient light, sun shadows); "after" is this branch with its
defaults (`Bloom { intensity = 0.04, radius = 0.12 }`, a highlight pull of 2 stops).

## Before and after

| View | Before | After |
|---|---|---|
| SP-14's cave, looking out at the sunlit ground (auto-exposure, 1920 x 1080) | ![](cave_mouth_before.png) | ![](cave_mouth_after.png) |
| The slice at s = 1, 300 m up at sunset, the sun 2 degrees up (2560 x 1440) | ![](sunset_before.png) | ![](sunset_after.png) |
| The slice at s = 0.01 from orbit (2560 x 1440) | ![](orbit_before.png) | ![](orbit_after.png) |
| SP-14's sun from 400 km, deep-space exposure adapting (1920 x 1080) | ![](sun_from_orbit_before.png) | ![](sun_from_orbit_after.png) |
| Bistro at noon, EV100 14 (1920 x 1080) | ![](bistro_noon_before.png) | ![](bistro_noon_after.png) |
| Deep space, EV100 −4 (`scripts/space_backdrop.luau`, 1920 x 1080) | ![](deep_space_before.png) | ![](deep_space_after.png) |

Mean FLIP after against before: cave mouth 0.50 (the exposure moved two stops, below), sunset 0.059, orbit
0.012, the sun from orbit 0.10, Bistro at noon 0.026 (0.027 at 1440p), Bistro at sunset 0.023, deep space 0.018, SP-14's
sunlit ground 0.008.

- **The cave's opening reads as light.** Before, a white hole with hard edges in a black frame; after, the opening
  glows over the walls, floor and ceiling around it, and the interior is two stops brighter (the highlight pull,
  below), so the chamber's walls show.
- **The sun's disk at sunset** is a glowing sun instead of a white dot, with the sky and the ground under it veiled
  by its glare; **from orbit** the sun, clamped at the fp16 maximum in a frame exposed for stars, glows over a wide disk around it.
- **The lit limb from orbit** glows softly into space. Under auto-exposure a sunlit planet is a mid tone, so the glow
  is faint: bloom is physical and shows only what is far brighter than its surroundings.
- **Bistro at noon barely changes**: nothing in it is far brighter than the rest. Highlights soften by a pixel.
- **Deep space keeps its stars** (crop: [stars_crop.png](stars_crop.png), before | after). Each star gives 4 % of its
  light to a glow too wide to see; the brightest get a faint halo on top of their own glare (ADR 0091). The backdrop
  therefore takes part in bloom like everything else on screen (ADR 0099 rule 4).
- SP-14's cave has a seam at the horizon line on its walls (a thin bright line, there before bloom); bloom makes it
  glow a little. It is the spike's mesh, not the renderer.

## Intensity and radius

![](sweep_cave.png)

*The cave mouth at the old 4-stop pull. Rows: intensity 0.01, 0.02, 0.04. Columns: radius 0.06, 0.12, 0.25 of the
image height.* At 0.25 the glow washes the whole chamber grey; at 0.06 it is a tight halo that does not light the
walls; 0.12 spreads it over the walls around the opening. 0.04 is the strongest that keeps the far walls dark. It is
also the order of real glare: a lens scatters 1 to 3 % of its light, and the eye's Stiles-Holladay glare (10 E / θ²,
θ in degrees) puts about 6 % between 1 and 30 degrees. With one level per octave and equal weights the glow falls as
1 / r², the same shape. Defaults: **intensity 0.04, radius 0.12**.

## The exposure finish

![](pull.png)

*The cave mouth. Top: no bloom; bottom: bloom. Columns: highlight pull 0, 2, 3 and 4 stops.*

- **The −1 stop compensation stays.** Bloom moves light around without adding any, so the metered band and the grey
  it lands on do not change; metering reads the image before bloom anyway.
- **The highlight pull drops from 4 to 2 stops.** ADR 0082 capped it at 4 so the interior stayed readable while the
  opening went white. With bloom the opening reads as light at any pull, so the last two stops only darkened the
  interior: at 2 the chamber's walls show and the opening glows; at 0 the glow floods the frame. The pull binds only
  in high-contrast frames: of the slice's tour (22 views) only the sunset view changes between 4 and 2 (mean FLIP
  0.004); in SP-14's timeline only the cave mouth (EV100 7.4 to 5.4).

## Resolution and stability

- **Screen-relative** (`[bloom.AC1]`, `[bloom.AC3]`): the chain's weights by screen size match at 1080p, 1440p and
  2160p (weighted mean scale within 0.1 octave); a bright cube's glow measured at the same fractions of the height
  matches within two 8-bit levels at 640 x 360 and 960 x 540, once the levels a small image cannot have leave their
  share in the image (`coverage`; without it the smaller image glowed up to 2.6 levels brighter).
- **No added flicker under TAA:** Bistro at sunset, frame 40 against 41 with temporal AA on (the jitter moves):
  mean FLIP 0.0124 with bloom, 0.0128 without. The input is TAA's resolved image, and Karis's average in the first
  downsample keeps a lone bright pixel from pulsing the glow.
- **Deterministic:** the goldens rendered twice are byte-identical.

## GPU cost

Render-graph timestamps, release, 100 frames after 120 of warm-up, the minimum over three rounds. Other engineers'
GPU work shared the machine; the minimum matches temporal AA's clean cost (0.42 ms in Bistro at 1440p, ADR 0097).

| Scene | Size | Bloom chain (ms) | Display with / without (ms) | Total added (ms) |
|---|---|---|---|---|
| Bistro at noon | 1920 x 1080 | 0.142 | 0.054 / 0.051 | 0.146 |
| Bistro at noon | 2560 x 1440 | 0.200 | 0.093 / 0.079 | 0.214 |
| Deep space | 1920 x 1080 | 0.140 | 0.054 / 0.046 | 0.148 |
| Deep space | 2560 x 1440 | 0.219 | 0.102 / 0.092 | 0.229 |

At 1440p in Bistro: `bloom: down 0` (full resolution into half, 13 taps, Karis) 0.080, `down 1` 0.029, `down 2`
0.009, `up 0` 0.032, `up 1` 0.011, and each of the ten passes on smaller levels about 0.004 (0.04 together, mostly
per-pass overhead). Under the 0.3 ms target at 1440p.

## Goldens

Bloom is on in goldens. Mean FLIP of each regenerated golden against `main`'s: bistro noon 0.022, bistro sunset
0.018, cornell box (real time) 0.044 (the ceiling light's glow over the ceiling), flight helmet 0.009, runtime mesh
0.017, thin triangles 0.012, space backdrop 0.012, planet twilight 0.010 (s = 1) and 0.008 (s = 0.01), temporal AA in
motion 0.017. Inspected side by side: a soft glow around the brightest areas and nothing else. Photo-mode goldens
are untouched.

## Reproduce

Scratch scripts drove the shots; the same views from the command line:

```sh
VEACH_SP14=exposure VEACH_SP14_FROM=950 VEACH_SP14_HOLD=1 VEACH_SP14_SIZE=1920x1080 \
  build/release/apps/lab/veach-lab --rhi metal --headless --frames 90 --screenshot cave.png      # from apps/lab
VEACH_SP14_FROM=1430 ...                                                                         # the sun from orbit
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out build/release/apps/slice/veach-slice \
  --script scripts/slice_s1.luau --rhi metal --headless --frames 30000                           # from apps/slice
build/release/apps/slice/veach-slice --script scripts/slice_s0.01.luau --rhi metal --headless --frames 60 \
  --screenshot orbit.png
VEACH_DISABLE_BLOOM=1 ...                                                                        # without bloom
python3 docs/research/gpu-driven/scripts/timings.py --app build/release/apps/lab/veach-lab \
  --script scripts/bistro_noon.luau --size 2560x1440                                             # GPU ms per pass
build/debug/tests/veach-tests -tc="*bloom.AC*"
```
