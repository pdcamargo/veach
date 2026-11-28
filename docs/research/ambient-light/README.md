# Ambient light and occlusion (VCH-140)

Item 6 of the renderer pass ([spec](../../specs/ambient-light/spec.md), [ADR 0096](../../adr/0096-ambient-light-and-occlusion.md)):
the sky's diffuse and specular light from the air around each shaded point, and GTAO. Apple M5 Max, Metal, release
builds, 2026-11-28. "Before" is `main` at `f7be0b4` (the GPU-driven core and the twilight fix) with the same slice
tour views added; "after" is this branch.

**Verdict.** Surfaces read as 3D: walls take the lit ground's neutral light beside blue roofs, window recesses, the
hedge, the grass field, the dug hole and the cave's corners darken, metals reflect the sky, the craft in the upper air
is lit from the planet below, and a surface where the sun has set is dark whatever the camera's sky. GTAO is clean
without any history (no noise, halos or bands in the views below) and the same every frame. Its cost, 0.5 to 1.2 ms
at 1440p, is well above ADR 0062's estimate; the levers are listed in VCH-141.

## Before and after

| | before | after |
|---|---|---|
| Bistro at noon | ![](bistro_default_before.png) | ![](bistro_default_after.png) |
| Bistro, under the awnings | ![](bistro_awning_before.png) | ![](bistro_awning_after.png) |
| Bistro at sunset | ![](bistro_sunset_before.png) | ![](bistro_sunset_after.png) |
| The SP-16 field, from the ground | ![](field_ground_before.png) | ![](field_ground_after.png) |
| SP-14's cave (sky visibility 0.3 within the chamber) | ![](cave_before.png) | ![](cave_after.png) |
| The slice at s = 1, from the bottom of the dug hole | ![](slice_s1_hole_eye_before.png) | ![](slice_s1_hole_eye_after.png) |
| The slice at s = 0.01, from the bottom of the dug hole | ![](slice_s0.01_hole_eye_before.png) | ![](slice_s0.01_hole_eye_after.png) |
| The craft at the top of the air, s = 1 | ![](slice_s1_atmosphere_top_before.png) | ![](slice_s1_atmosphere_top_after.png) |
| The craft at the top of the air, s = 0.01 | ![](slice_s0.01_atmosphere_top_before.png) | ![](slice_s0.01_atmosphere_top_after.png) |
| `ambient_night_side.luau`: away from the sun, where it has set | ![](nightside_toward_night_before.png) | ![](nightside_toward_night_after.png) |
| `ambient_night_side.luau`: toward the sun | ![](nightside_toward_day_before.png) | ![](nightside_toward_day_after.png) |

The occlusion alone (`VEACH_SHOW_AO=1`), under the awnings: ![](bistro_awning_occlusion.png)

- **Bistro.** The facade facing away from the sun was the camera's blue sky averaged over an upward hemisphere; now it
  sees half sky and half the sunlit street (the analytic ground under the Sky), so it turns a lighter neutral grey, as
  shaded walls do outdoors. Window recesses, sills, the undersides of balconies and awnings, the hedge's depth and the
  bollards' feet darken. Metals (lanterns, the hotel sign, chair frames) reflect the sky instead of rendering black.
  With no sun shadows yet (a parallel item), the street in front of the facade is all sunlit, so the ground bounce is
  the open-field value.
- **The field.** Grass blades were one flat teal mass; now each blade shows its orientation and the grass darkens
  toward its roots. A faint 4 × 4 residue of the slice pattern is visible on some blades up close at 1080p.
- **The cave.** Before, every wall, the floor and the ceiling were the same blue: the chamber read as one surface. Now
  the corners are occluded and each wall takes the light its normal sees (times the camera's sky visibility). The walls
  are lit as if by open ground below them, brighter than the floor: an outdoor sky's light, which local probes
  (ADR 0062) will replace in caves.
- **The dug hole.** The walls' facets and creases show; the light at the bottom is greener (the ground's share).
- **The craft at the top of the air.** Its sides were black (the camera's upward hemisphere is space up there); the
  lit planet below now lights them.
- **The night side.** The slice cannot show it without haze: from inside the air the terminator lies under the haze
  near the horizon (s = 0.01), or beyond the horizon (s = 1), and from orbit the camera's sky was dark already. The lab
  scene `ambient_night_side.luau` isolates it (no haze, the sky's sun no light): two patches 1,500 km away on the
  curve of the Sky's planet, one where the sun is 18 degrees up and one where it is 8 degrees down. Before, both got
  the camera's sky; after, the night one is black and the day one lit. The day patch darkens toward its far edge: the
  flat patch rises 9 km off the curved ground there, into thinner air.

## Cost

GPU ms per pass, render-graph timestamps, median over 100 frames (60 for the slice) after a warmup, the median of two
rounds (`scripts/sweep.py`, the GPU-driven core's `timings.py`). Other agents' runs shared the GPU during these
measurements, so totals between builds move by more than the change itself; the passes this work added are the
reliable part.

| Scene | Size | Build | Total | Shade | Sky irradiance | AO: depth | AO: trace | AO: denoise | AO: upsample | AO total | Visibility | Display |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bistro | 1920x1080 | before | 1.43 | 0.33 | 0.11 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.70 | 0.05 |
| bistro | 1920x1080 | after | 1.66 | 0.40 | 0.00 | 0.06 | 0.18 | 0.05 | 0.05 | 0.34 | 0.65 | 0.04 |
| bistro | 2560x1440 | before | 2.55 | 0.74 | 0.14 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 1.13 | 0.12 |
| bistro | 2560x1440 | after | 3.10 | 0.88 | 0.00 | 0.10 | 0.46 | 0.10 | 0.12 | 0.79 | 0.98 | 0.10 |
| field_ground | 1920x1080 | before | 8.19 | 0.64 | 0.21 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 6.19 | 0.10 |
| field_ground | 1920x1080 | after | 7.41 | 0.70 | 0.00 | 0.09 | 0.42 | 0.09 | 0.09 | 0.68 | 5.10 | 0.08 |
| field_ground | 2560x1440 | before | 7.48 | 0.91 | 0.18 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 5.37 | 0.15 |
| field_ground | 2560x1440 | after | 8.45 | 1.11 | 0.00 | 0.13 | 0.77 | 0.15 | 0.15 | 1.20 | 5.02 | 0.14 |
| field_mid | 1920x1080 | before | 7.46 | 0.56 | 0.16 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 5.79 | 0.07 |
| field_mid | 1920x1080 | after | 7.90 | 0.73 | 0.00 | 0.08 | 0.19 | 0.08 | 0.07 | 0.42 | 5.78 | 0.07 |
| field_mid | 2560x1440 | before | 8.39 | 0.88 | 0.15 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 6.19 | 0.12 |
| field_mid | 2560x1440 | after | 7.80 | 0.92 | 0.00 | 0.11 | 0.31 | 0.11 | 0.10 | 0.63 | 5.26 | 0.09 |
| slice_s0.01_landed | 2560x1440 | before | 2.74 | 0.95 | 0.21 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.51 | 0.17 |
| slice_s0.01_landed | 2560x1440 | after | 4.00 | 1.31 | 0.00 | 0.14 | 0.58 | 0.15 | 0.16 | 1.03 | 0.46 | 0.16 |
| slice_s1_landed | 2560x1440 | before | 3.28 | 1.14 | 0.25 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.68 | 0.20 |
| slice_s1_landed | 2560x1440 | after | 3.47 | 1.19 | 0.00 | 0.13 | 0.56 | 0.13 | 0.15 | 0.97 | 0.47 | 0.13 |

- **GTAO**: 0.34 to 0.68 ms at 1080p and 0.63 to 1.20 ms at 1440p across Bistro, the field and the slice; the trace
  is about 60 % of it, the depth pyramid, the two blurs and the upsample about 0.1 ms each at 1440p. Quieter runs of
  Bistro at 1440p while tuning measured 0.53 ms (trace 0.27 ms), so contention inflates these by up to half. That is
  well above ADR 0062's estimate (0.1 ms at 1080p, with temporal reuse) and is the number to watch on M1-class GPUs
  (VCH-097, VCH-141).
- **Shading** grows by the ambient evaluation (6 table samples and two SH evaluations per sun per pixel) and loses the
  per-frame irradiance pass (one thread: 0.11 to 0.25 ms here). Net, shading plus that pass: from -0.05 to +0.15 ms.
- **The ambient table** is built once per medium (and when a planet's ground albedo changes): 256 directions per entry,
  1,024 entries. Its first build measured 3.3 ms with 512 directions on the frame the GPU starts cold; with 256 it is
  1.9 ms (the medium's multiple-scattering table takes 0.87 ms on the same frame).
- **The trace's cost**: at full resolution, 3 slices × 4 steps took 2.2 ms at 1440p; 1 slice × 1 step still 0.48 ms
  (0.07 of it the loads, the rest per-pixel math and the dependent samples). Half resolution with 2 slices × 3 steps
  was the budget's answer (VCH-141 lists the next levers).

## Goldens

Regenerated deliberately and inspected, again after merging the sun shadows (ADR 0095); mean FLIP against the images
before this work, and against `main`'s with shadows:

| Golden | Before this work | Main (with shadows) | What changed |
|---|---|---|---|
| bistro_noon | 0.327 | 0.354 | facades neutral grey instead of blue; AO; metals reflect |
| bistro_sunset | 0.164 | 0.277 | shaded walls warm instead of blue (too warm in shadow: finding 2); AO |
| runtime_mesh_realtime | 0.045 | 0.046 | slopes facing away from the sun see the ground; AO in the valleys |
| flight_helmet_realtime | 0.033 | 0.033 | the brass plate and metal parts reflect the constant sky |
| thin_triangles_realtime | 0.006 | 0.006 | regenerated to keep WARP's margin |

The Cornell box (no sky), photo mode, the twilight goldens, blend modes and the space backdrop are unchanged. The
DX12/WARP jobs compare against the same images.

## Findings

1. **The sky around the camera is the wrong source for ambient light anywhere but near the camera.** A table by
   altitude and sun angle per medium gives every point its own sky for one bilinear fetch per sun, and costs nothing
   per frame.
2. **Walls see the ground.** Half of a vertical surface's hemisphere is the sunlit ground; including it (the
   analytic ground in the table) is what makes shaded walls neutral rather than blue. In a street canyon without
   shadows that bounce is too strong; shadows on the ground will not reach it (the table's ground is analytic): probes
   or a screen-space bounce would. With the sun shadows merged (ADR 0095), Bistro at sunset shows the same limit from
   the sky's side: the facade facing the low sun is in the opposite buildings' shadow, yet still sees the bright sky
   around the sun that those buildings hide, so it stays warm and the shadow reads weakly (golden `bistro_sunset`,
   FLIP 0.28 against main's shadowed image). Occlusion beyond GTAO's 1.5 m (a street, a canyon) needs a large-scale
   sky visibility: a bent cone from a coarse, long-radius pass, or the sun's shadow applied to the sky near the sun
   (VCH-141).
3. **GTAO without history needs its estimates blurred before they are clamped.** A pixel's two slices of an open plane
   seen at an angle sum to around 1, not exactly 1; clamping per pixel before the blur darkened open walls by their
   angle to the view, in rings. Blurring the unclamped estimate and clamping in the upsample removed it, as did
   sampling the depth pyramid at each texel's own center and taking the tangent exactly on open sides.
4. **A 2D threadgroup array of float3 was nondeterministic on Metal** in the table's reduction (Bistro's image varied
   by up to FLIP 0.04 between runs, not under shader validation); a 1D float4 array is not. Checked over 8 runs.
5. **Full-resolution GTAO was too slow on this GPU for its budget**: the per-pixel cost, not the samples' bandwidth
   (coarser mips did not help), dominated; a quarter of the pixels did.

## Reproduce

```sh
python3 docs/research/ambient-light/scripts/sweep.py --new $PWD --old <tree with main's release build> --rounds 2
python3 docs/research/ambient-light/scripts/tables.py
build/release/apps/lab/veach-lab --rhi metal --headless --frames 3 --script scripts/ambient_night_side.luau \
    --screenshot night.png          # from apps/lab; VEACH_SHOW_AO=1 shows the occlusion
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out build/release/apps/slice/veach-slice --script scripts/slice_s1.luau \
    --rhi metal --headless --frames 30000   # from apps/slice; the tour ends with the night-side view
build/debug/tests/veach-tests -tc="*ambient.AC*"
```
