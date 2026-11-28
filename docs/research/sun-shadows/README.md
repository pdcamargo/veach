# Sun shadows (VCH-142)

What the sun shadows of [ADR 0095](../../adr/0095-sun-shadows.md) ([spec](../../specs/sun-shadows/spec.md)) look like
and cost. Apple M5 Max, Metal, release builds, 2026-11-28, default settings (one shadowed sun per scene, 4 cascades of
2048², out to 1000 m). The build before is `main` at `818f67c` (the GPU-driven core) with the same measurement aid
added: `VEACH_GPU_TIMINGS` also logs `frame_ms`.

## How it was measured

`scripts/sweep.py` runs every configuration on both builds in turn, twice, and takes the median of the two rounds'
medians (`--table timings.json` prints the table). It reuses [the GPU-driven core's](../gpu-driven/README.md)
`timings.py`: the renderer times itself over 100 frames (40 for the field, 60 for the slice) while frames run back to
back.

- **GPU frame** is the command buffer's span (`MTLCommandBuffer` GPU start to end), the cost that matters. The **sum of
  passes** overstates the shadows: Apple GPUs run the cascades' passes beside the camera's culling and visibility pass,
  and each pass's timestamps include the time it shares the GPU, so the shadow passes' sum is larger than what they
  add to the frame (in the field, 12 ms of passes for 2 ms of frame).
- **The slice** runs at 4 to 14 ms of CPU per frame, so its GPU idles and clocks down between frames; both builds'
  frames are within that noise (the shadows build measured *faster*), and only the shadow passes' sum bounds their cost.
- Other sessions shared the machine during the sweep; repeated runs moved by up to 10%.

## Results

| Scene | Size | GPU frame before | GPU frame with shadows | Added | Shadow passes (sum) | Shade before / after | CPU ms/frame before / after |
|---|---|---|---|---|---|---|---|
| Bistro, noon | 1920x1080 | 1.35 | 2.43 | +1.08 | 3.21 | 0.29 / 0.42 | 3.2 / 4.9 |
| Bistro, noon | 2560x1440 | 1.75 | 2.77 | +1.02 | 3.34 | 0.49 / 0.66 | 3.2 / 5.0 |
| Bistro, sunset | 1920x1080 | 1.35 | 3.85 | +2.50 | 3.96 | 0.30 / 0.48 | 3.2 / 5.3 |
| Bistro, sunset | 2560x1440 | 1.78 | 4.56 | +2.78 | 4.20 | 0.50 / 0.88 | 3.3 / 5.3 |
| SP-16 field, ground | 1920x1080 | 5.85 | 7.55 | +1.70 | 12.26 | 0.42 / 0.44 | 33.8 / 37.4 |
| SP-16 field, ground | 2560x1440 | 5.55 | 7.78 | +2.23 | 11.93 | 0.64 / 0.71 | 34.4 / 36.7 |
| Slice landed, s = 0.01 | 2560x1440 | 2.48 | 1.98 | (noise) | 0.98 | 0.86 / 0.69 | 13.6 / 14.2 |
| Slice landed, s = 1 | 2560x1440 | 2.73 | 2.15 | (noise) | 0.89 | 0.94 / 0.79 | 4.8 / 6.0 |

Meshlets each cascade drew (`CullStats::shadow_meshlets_drawn`; the camera drew 9,379 in Bistro and 157,409 in the
field):

| Scene | Cascade 0 (to 15.6 m) | 1 (to 62.5 m) | 2 (to 250 m) | 3 (to 1000 m) |
|---|---|---|---|---|
| Bistro, noon | 8,798 | 11,431 | 197 | 0 |
| Bistro, sunset | 25,648 | 48,084 | 37,784 | 25,978 |
| SP-16 field, ground | 1,077 | 15,209 | 185,917 | 261,374 |

Bistro, 1920x1080, other settings (GPU frame, ms; 1.31 without shadows in the same session):

| Cascades x size | 4 x 2048² | 3 x 2048² | 4 x 1024² | 3 x 1024² | 2 x 1024², to 250 m |
|---|---|---|---|---|---|
| At noon | 2.24 | 2.11 | 2.08 | 1.86 | |
| At sunset | 3.85 | 3.63 (to 250 m) | 3.39 | | 2.78 |

Two suns (Bistro at noon with a second directional light, 1920x1080): 1.41 ms with no shadow maps, 3.49 with the
brighter one shadowed, 4.20 with both.

## Images

| | Real time | Photo mode (path traced) |
|---|---|---|
| Bistro, noon | ![](bistro_noon.png) | ![](bistro_noon_photo.png) |
| Bistro, sunset: the low sun is behind the buildings across the square, only a sliver reaches the far facade | ![](bistro_sunset.png) | ![](bistro_sunset_photo.png) |

Two suns (the second orange, from the left): both cast; the awnings shadow the facade.

![](bistro_two_suns.png)

The SP-16 field: grass and trunks dappled by the canopies, leaves shadowing leaves.

| ground | mid |
|---|---|
| ![](field_ground.png) | ![](field_mid.png) |

The slice (2560x1440, the demo's tour):

| | s = 0.01 | s = 1 |
|---|---|---|
| Landed | ![](s0.01_landed.png) | ![](s1_landed.png) |
| The pilot in the dug hole | ![](s0.01_step_in.png) | ![](s1_step_in.png) |
| A 2° sun from 300 m: long shadows to the fade at 800 to 1000 m | ![](s0.01_sunset_300m.png) | ![](s1_sunset_300m.png) |

From the bottom of the hole at s = 1, its sunlit wall is now shadowed by the rim
([before](../spikes/vch-135-atmosphere/s1_hole_eye.png)):

![](s1_hole_eye.png)

A camera moving 1.3 cm a frame in Bistro (crops of frames 0 and 5, enlarged 3x): the bollards' shadow edges keep their
shape and only move with the view.

| frame 0 | frame 5 |
|---|---|
| ![](moving_0.png) | ![](moving_5.png) |

## Findings

1. **Shadows match photo mode** where both can be compared: Bistro's street at noon (the building's shade on the
   sidewalk, the bollards' shadows) and at sunset, where the low sun is blocked by the buildings across the square
   and only a sliver of light reaches the far facade, in both images. The sunset golden, which showed the facade lit,
   was wrong for lack of shadows.
2. **No acne, no peter-panning, no shimmer seen.** Flat ground, Bistro's facades and cobbles, the terrain at a 2° sun
   and the field's cards show no self-shadowing pattern; shadows start at the bollards' and the pilot's feet; moving
   the camera by fractions of a texel leaves static shadows in place (`[shadows.AC2]` checks the snapping, including
   across a 64 m render cell). Long shadows show a faint stair at their far end in the nearest cascade (1.5 texel PCF
   at 2560x1440); TAA (ADR 0061) will smooth it.
3. **Cost: about 1 ms at noon, 2.5 to 2.8 ms at sunset** in Bistro on the M5 Max, above ADR 0062's 0.4 ms estimate.
   It is geometry: at noon the two near cascades draw about as many meshlets as the camera (8,798 and 11,431 against
   9,379), and at sunset a 3° sun sweeps every caster in the town into every cascade (25,000 to 48,000 meshlets each;
   resolution or fewer cascades barely help, 2 x 1024² to 250 m still adds 1.4 ms). Each cascade also has a fixed
   cost of about 0.1 to 0.2 ms (clear, store, a render pass). The fix for low suns is occlusion from the light's side
   (last frame's cascade as a HiZ, as the camera does) and caching the far cascades: VCH-143.
4. **The CPU pays 1.7 to 2 ms per frame in Bistro**: the six new passes each declare every mesh buffer and texture
   (about 3,000 in Bistro) for residency and barriers, which the render graph walks. Declaring scene residency once per
   frame is VCH-143's too.
5. **The field adds 1.7 to 2.2 ms**: the far cascades draw the grass of a square kilometer (186,000 and 261,000
   meshlets); vegetation distances and LOD (VCH-139) cut this with the camera's own cost.
6. **A second sun adds about 0.7 ms** in Bistro (the second, higher sun's cascades hold fewer casters).
7. **The shading lookup costs 0.13 to 0.38 ms** (shade 0.29 to 0.42 ms at noon, 1080p; most at sunset, where nearly
   every pixel is in a penumbra search).

## Reproduce

From the repository root, with a release build here and one of the build before elsewhere (both with `frame_ms`):

```sh
python3 docs/research/sun-shadows/scripts/sweep.py --new $PWD --old <before tree> --rounds 2 \
    --out docs/research/sun-shadows/timings.json
python3 docs/research/sun-shadows/scripts/sweep.py --table docs/research/sun-shadows/timings.json
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out build/release/apps/slice/veach-slice \
  --script scripts/slice_s1.luau --rhi metal --headless --frames 30000   # from apps/slice
```
