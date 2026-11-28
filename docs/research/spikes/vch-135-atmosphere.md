# VCH-135: the atmosphere done properly, from orbit to a cave

Item 5 of the renderer pass ([spec](../../specs/planet-atmosphere/spec.md),
[ADR 0092](../../adr/0092-aerial-perspective-follows-the-air.md)). The owner's observation: from orbit, a real
planet's whole sunlit disk sits under a blue veil, strongest toward the limb; the first slice drew only the thin blue
rim, because the ground was drawn without the air in front of it. All numbers are from the Apple M5 Max at
2560 × 1440, release build, 2026-11-28.

**Verdict.** The veil is there at both scales, strongest at the limb, with no seam or band; distant terrain hazes
into the horizon from 200 m; the dug hole and SP-14's cave stay clear and the haze comes back through the cave's
mouth; sunsets light the terrain gold at s = 1 and pale at s = 0.01 (the known s^¼ effect, ADR 0081). The volume
costs about 0.05 ms at full GPU clock and applying it about 0.04 ms more in the shade pass. Every existing golden
passes unchanged.

## Before and after: orbit

| | before (SP-15) | after |
|---|---|---|
| s = 0.01, 3.4 radii out | ![](sp-15/s0.01_1_orbit.png) | ![](vch-135-atmosphere/s0.01_orbit.png) |
| s = 1, 3.4 radii out | ![](sp-15/s1_1_orbit.png) | ![](vch-135-atmosphere/s1_orbit.png) |
| s = 0.01, half a radius up | ![](sp-15/s0.01_2_descent_high.png) | ![](vch-135-atmosphere/s0.01_high.png) |
| s = 1, half a radius up | ![](sp-15/s1_2_descent_high.png) | ![](vch-135-atmosphere/s1_high.png) |

- **s = 1** reads like Earth from orbit: the disk under a pale blue veil that whitens and thickens toward the sunlit
  limb, and a blue twilight band along the terminator. The air is 100 km over 6,371 km, so the veil is thin over the
  center (a 100 km column) and strong where the view grazes it.
- **s = 0.01** is much hazier: 10 km of air over a 64 km planet (ADR 0049's √s rule), so every column is
  proportionally ten times thicker and the limb fills 15 % of the radius. That is the scale model, not the renderer:
  the same code makes both images.
- Half a radius up, the terrain fades into blue toward the horizon and meets the limb without a line.

## From the ground, in the hole, at sunset

| | s = 0.01 | s = 1 |
|---|---|---|
| Haze from 200 m, looking away from the sun | ![](vch-135-atmosphere/s0.01_haze_200m.png) | ![](vch-135-atmosphere/s1_haze_200m.png) |
| In the atmosphere (30 % of the top) | ![](vch-135-atmosphere/s0.01_in_atmosphere.png) | ![](vch-135-atmosphere/s1_in_atmosphere.png) |
| From the bottom of the dug hole | ![](vch-135-atmosphere/s0.01_hole_eye.png) | ![](vch-135-atmosphere/s1_hole_eye.png) |
| Sunset: the sun 2° up, 300 m over the ground | ![](vch-135-atmosphere/s0.01_sunset.png) | ![](vch-135-atmosphere/s1_sunset.png) |

- **Haze.** At s = 1 the far terrain greys into a pale horizon over tens of km; at s = 0.01 the coefficients are ten
  times larger (×1/s_a), so the same 200 m view is hazy within a few km. Human-scale haze (physical coefficients near
  the camera at small scale, ADR 0063) would thin the near haze at s = 0.01; it is still unbuilt (VCH-136).
- **In the hole** the slice measures the camera's sky visibility against the surface scene's colliders: 0.25 open
  within 2.3 m at s = 0.01, 0.28 within 2.4 m at s = 1. The walls get a quarter of the sky's ambient light and no
  haze; the direct sun still lights them because the interim renderer has no shadows (ADR 0040, 0062).
- **Sunset.** s = 1: a gold glow on the horizon, the terrain lit orange-gold by the reddened sun under a violet-grey
  sky. s = 0.01: a white glow over blue, the paler sunset ADR 0081 accepted (creator control is VCH-109).

## A cave: SP-14's chamber, now with sky visibility

SP-14 turned aerial perspective off by hand inside its cave. Now the spike sets the camera's `SkyVisibility` from the
chamber (the stand-in's open fraction, reaching back to the mouth) and the renderer does the rest.
`VEACH_SP14_OPEN_CAVE=1` leaves it out, which is what SP-14 found: blue haze brighter than the lamp-lit walls.

| | with sky visibility | without (SP-14's failure) |
|---|---|---|
| The back wall, lit by a 600 cd headlamp | ![](vch-135-atmosphere/cave_back_wall.png) | ![](vch-135-atmosphere/cave_back_wall_open.png) |
| Turned to the mouth | ![](vch-135-atmosphere/cave_mouth.png) | ![](vch-135-atmosphere/cave_mouth_open.png) |

With it, the chamber reads (ceiling, walls, the lamp's pool) at the same blue-grey ambient level as SP-14's
reference, and the mouth is white because the sunlit outside is 13 stops brighter (ADR 0082 rule 6); the ceiling
brightens toward the mouth where the reach runs out. Without it, the walls drown in haze and auto-exposure adapts to
the haze.

## Eclipses

SP-10's eclipse views, re-rendered through the occluder array: ![](vch-135-atmosphere/eclipse_ground_ev9.png)
![](vch-135-atmosphere/eclipse_orbit.png) (a total eclipse from the ground at EV 9, the horizon glowing outside the
umbra; the umbra and penumbra on the planet from orbit). In the slice the moon and the planets of largest angular size
are passed as occluders; no eclipse happens during the demo.

## How it works

- **The volume.** One cell per 16 × 16 pixels, 32 slices, `rgba16_float` (160 × 90 × 32 at 1440p). One thread per
  cell marches its ray once, one sample per slice, every sun with its eclipses, and writes each slice.
- **Depth that follows the air, per ray.** A ray's slices span from where it enters the shell to where it meets the
  ground (plus a margin), or else to the shell's exit (from inside) or its lowest point (from outside), blended so
  neighbors never jump. Slices sit at x = ((1 − cos πw) / 2)²: under a meter for the first at s = 1 on the ground,
  crowded at the ground's end for the veil.
- **Reading it.** A surface reads at its own fraction of its own ray's span. Two versions failed on the way:
  - Without the lowest-point rule, terrain standing above the ground radius at the limb missed the analytic sphere,
    read mid-air values, and drew a dark line along the limb.
  - Reading each cell at the surface's absolute distance along that cell's ray (classic froxels) banded the grazing
    view near the terminator in 16-pixel stripes, because neighboring rows meet the ground kilometers apart.
- **Sky visibility** is the camera's: `SkyVisibility { visibility, reach }`, measured by `physics::sky_visibility`
  (32 cosine-weighted rays against static colliders) or set by gameplay. Air and surfaces within `reach` get the
  sky's light times `visibility`, both in the volume and in the ambient term.
- **Per-medium tables** in a four-slot cache; the sky-view table wraps in azimuth; every sphere test goes through one
  stable helper with a CPU twin (`[atmosphere.AC1]` checks it at 1e4 km on the CPU and on Metal).

## GPU cost (2560 × 1440, M5 Max)

The screenshots are from after merging the space backdrop (ADR 0091, the stars and band behind the planet); the
table was taken just before it. After the merge the volume measures the same (0.049 to 0.053 ms in the orbit and
atmosphere frames), and the shade pass is lighter (0.33 ms in orbit) because the stars moved to the backdrop's pass.

From the slice's per-milestone pass timings. Each frame's numbers move with the GPU's clock: the sky view's work is
fixed and it took 0.021 ms in the orbit frames and up to 0.12 ms in frames where the CPU stalled on terrain, so
compare passes within a frame.

| Frame (s = 0.01 / s = 1) | sky view | aerial perspective (volume) | shade (whole pass) |
|---|---|---|---|
| Orbit | 0.021 / 0.022 | 0.052 / 0.053 | 0.548 / 0.439 |
| Half a radius up | 0.023 / 0.024 | 0.051 / 0.052 | 0.898 / 0.496 |
| In the atmosphere | 0.035 / 0.034 | 0.061 / 0.060 | 0.576 / 0.562 |
| Haze from 200 m | 0.060 / 0.051 | 0.107 / 0.089 | 1.125 / 0.880 |
| In the hole | 0.060 / 0.061 | 0.105 / 0.105 | 0.873 / 1.172 |

- **The volume costs 0.05 ms at full clock**, 2.2 to 2.5 times the sky view in every frame. With 64 slices and two
  samples each it cost 0.17 ms; 32 slices and one sample differ from that by mean FLIP 0.0007 to 0.0034 in every
  slice view, so they ship.
- **Applying it** adds about 0.04 ms to the shade pass (orbit: 0.51 → 0.55 ms at s = 0.01, 0.40 → 0.44 ms at s = 1,
  against SP-15).
- The transmittance and multiple-scattering tables are built once per medium (SP-10: 0.006 + 0.19 ms).
- The trig-free eclipse rejection saved 0.007 ms of 0.18 at the time; the cost is in the samples, not the eclipses.

## Follow-ups (VCH-136)

- Human-scale haze: physical coefficients near the camera at small scale (ADR 0063, 0049).
- Per-froxel sky visibility and volumetric sun shadows from a view of the scene, with shadows (ADR 0062) in the 0060
  renderer, behind the same two functions.
- Ambient light per surface (probes or SH, ADR 0062): the night side is lit by the camera's day sky from the edge of
  the atmosphere today.
- Other bodies' atmospheres from a planet (a neighbor's limb), and photo mode's planet sky.
- The sky-irradiance pass is still one thread reading 256 texels (0.07 ms, SP-10): a parallel reduction.

## Reproduce

```sh
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out build/release/apps/slice/veach-slice \
  --script scripts/slice_s0.01.luau --rhi metal --headless --frames 30000   # also slice_s1.luau; from apps/slice
VEACH_SP14=exposure VEACH_SP14_FROM=650 VEACH_SP14_HOLD=1 VEACH_SP14_SIZE=1920x1080 \
  build/release/apps/lab/veach-lab --rhi metal --headless --frames 240 --screenshot cave.png   # from apps/lab
build/debug/tests/veach-tests -tc="*atmosphere.AC*"
```
