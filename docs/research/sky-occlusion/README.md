# Large-scale sky occlusion (VCH-141)

[Spec](../../specs/sky-occlusion/spec.md), [ADR 0101](../../adr/0101-large-scale-sky-occlusion.md). Apple M5 Max,
Metal, release builds, 2026-11-28. "Before" is the same build with `VEACH_DISABLE_SKY_OCCLUSION=1` (main's ambient light
of ADR 0096 with GTAO and sun shadows); "after" is the default. Taken before merging bloom (ADR 0099), so both sides
are without it.

**Verdict.** The shadowed facade at sunset reads as shadowed, close to photo mode's
([sun shadows' reference](../sun-shadows/bistro_sunset_photo.png)); the street's walls lose the sky behind the
buildings across it; under an awning, in the SP-14 cave and at the bottom of the dug hole the light comes from what the
opening shows, not from sunlit open ground; the open field barely changes. 0.26 ms at 1440p, 0.16 ms at 1080p
(steady), plus a redraw of the height map every 60 frames or 62.5 m. The visible flaws are the height map's: stair
steps at a cave's drip line and blocky dark patches at the bottom of the s = 1 hole (0.5 m texels against a 2 m hole).

## Before and after

| | before | after |
|---|---|---|
| Bistro at sunset (the golden's view) | ![](bistro_sunset_before.png) | ![](bistro_sunset_after.png) |
| Bistro at noon | ![](bistro_noon_before.png) | ![](bistro_noon_after.png) |
| Bistro at sunset, under the café's awning (`scripts/street.luau`) | ![](bistro_street_before.png) | ![](bistro_street_after.png) |
| SP-14's cave, 20 m in (auto-exposure) | ![](cave_before.png) | ![](cave_after.png) |
| SP-14's cave, turned to the mouth | ![](mouth_before.png) | ![](mouth_after.png) |
| The SP-16 field from the ground | ![](field_before.png) | ![](field_after.png) |
| The slice at s = 1, from the bottom of the dug hole | ![](slice_s1_hole_eye_before.png) | ![](slice_s1_hole_eye_after.png) |
| The slice at s = 0.01, from the bottom of the dug hole | ![](slice_s0.01_hole_eye_before.png) | ![](slice_s0.01_hole_eye_after.png) |

- **Bistro at sunset.** Before, the facade facing the low sun was warm: it saw the bright sky around the sun that the
  buildings behind the camera hide, and the open, sunlit ground of the ambient table below. After, it sees the bluish
  sky above those buildings, their shaded faces and the shaded street: dark blue-grey as in photo mode. The left
  building's upper floors, which catch the sun's bounce, stay lighter. Golden FLIP 0.58 against main's.
- **Bistro at noon.** The facade and the far walls darken where the street is narrow; the sunlit street in front still
  bounces onto them (its cascade lookups say it is in the sun). Golden FLIP 0.18.
- **Under the awning.** At sunset the café front, the table and the bench under the awning were lit like the open
  street; now they are in deep shade, with the street beyond lit by the sky.
- **The cave.** Before, the spike's own `SkyVisibility` (0.0003 inside) stood in; it no longer applies within the
  reach. After, the chamber's light comes from the mouth: the walls near it and the floor in front of it are lit, the
  back is dark; the walls no longer get "ground" light from below. Steps along the drip line follow the height map's
  texels.
- **The field.** Unchanged but for the grass toward the trunks; the canopies are slabs (their undersides are open).
- **The dug hole.** The hole's lower walls lose the sky the rim hides and the sunlit ground bounce; at s = 1 (a hole
  about 2 m wide) the bottom shows blocky dark patches where the 0.5 m texels of the walls fall between steps.

## Cost

GPU ms, Bistro, median over 60 frames after 60 of warmup, two rounds (`timings.txt`, `scripts/sweep.sh`).

| Scene | Size | Trace + blur | GPU frame span without | with | Δ |
|---|---|---|---|---|---|
| Bistro noon | 1920x1080 | 0.17 / 0.17 | 2.98 / 2.99 | 3.13 / 3.12 | +0.14 |
| Bistro noon | 2560x1440 | 0.27 / 0.27 | 4.03 / 4.04 | 4.37 / 4.39 | +0.35 |
| Bistro sunset | 1920x1080 | 0.16 / 0.16 | 4.36 / 4.40 | 4.54 / 4.60 | +0.19 |
| Bistro sunset | 2560x1440 | 0.26 / 0.26 | 5.59 / 5.74 | 5.85 / 5.95 | +0.24 |

- **The height map** (three depth views from above, culled by the GPU-driven path) costs 1.7 ms of passes at sunset
  and 2.3 ms at noon in Bistro, +1.4 ms of frame span, on the frames that draw it: the camera crossing a 62.5 m cell,
  or every 60 frames. The heights and the pyramid cost 0.03 ms.
- **The trace** first ran per quarter-resolution pixel: 0.6 to 0.7 ms at 1440p, most of it the march (8 azimuths of 12
  steps); the shadow lookups were 0.05 ms and the table's evaluation 0.04 ms. One thread per 8 × 8 cell costs 0.25 ms;
  the result is smooth by construction (a ratio of the table with and without what is hidden), and the shading keeps
  the normal's detail through the table.
- The field, the slice and the cave were not timed (the field's MCP sweep did not run in this tree); their trace has the
  same per-cell cost at the same size.

## Findings

1. **A height map needs to know what is solid.** One top-down layer makes every roof a column to the ground, so a
   point under an awning sees no sky at all; a "highest face turned down" layer alone cannot tell a single-sided eave
   from a wall (Bistro's buildings are shells whose roofs overhang). Three layers (highest, lowest, highest face turned
   down) and a thin-slab guess for single-sided surfaces get Bistro, the cave and the hole right. Overlapping closed
   meshes (a roof box resting on wall boxes) still read the walls as air under a slab.
2. **The shadow maps answer "is what I see sunlit?", not "what do I see?".** A cascade is a height map from the sun;
   at sunset it is a side view. Sampled at a few points in front of what each azimuth sees, it tells whether the
   surroundings bounce sunlight or only sky light, which is what made the sunset facade dark.
3. **Subtracting the hidden sky's estimate leaves light where everything is hidden.** The sectors' quadrature misses
   the table's irradiance by tens of percent; a fully enclosed floor stayed blue. The ratio of the same quadrature
   with and without the occlusion is exact at both ends.
4. **A step must cover its whole stretch.** Read only at its middle distance, a slab's elevations left gaps between
   steps (stripes on a covered floor); bounding each step's elevations by its near and far ends removed them. A coarser
   mip per step catches every wall but its maximum takes in the surface's own building.
5. **The map must not depend on history.** Redrawing when the camera moved some meters made the image depend on where
   the camera came from (a camera jumping round a wall rendered differently from one that started there); snapping
   the map's center to its coarsest mip's texels makes every mip the same whichever frame drew it.

## Reproduce

```sh
VEACH_DISABLE_SKY_OCCLUSION=1 build/release/apps/lab/veach-lab --rhi metal --headless --frames 3 \
    --script scripts/bistro_sunset.luau --screenshot before.png      # from apps/lab; without it: after
VEACH_SHOW_SKY_OCCLUSION=1 ...                                        # the ambient light alone, on white surfaces
VEACH_SP14=exposure VEACH_SP14_FROM=650 VEACH_SP14_HOLD=1 build/release/apps/lab/veach-lab --rhi metal --headless \
    --frames 90 --screenshot cave.png                                 # 950: the mouth
zsh docs/research/sky-occlusion/scripts/sweep.sh                      # edit its paths to your tree
build/debug/tests/veach-tests -tc="*skyocc*"
```
