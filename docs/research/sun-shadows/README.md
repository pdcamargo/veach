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

## VCH-143: culling and caching

What [ADR 0100](../../adr/0100-sun-shadow-culling.md) changed and what it saves: scene residency declared once per
frame, casters culled by this frame's receivers and by two-phase occlusion from the light, the shading's
fully-shadowed early-out, and the far cascades kept while the scene is still. The build before is `main` with ambient
occlusion and TAA (ADR 0096, 0097); "no maps" is this build with `SunShadows.suns = 0`, the cost's zero line. Apple M5
Max, Metal, release builds, default settings (4 cascades of 2048² to 1000 m).

### How it was measured

Other sessions ran GPU work on the machine all along; single runs moved by up to a factor of two. The GPU numbers
below are therefore direct, unpaced runs (`--frames`, `VEACH_GPU_TIMINGS=100,120`), the three builds alternating four
times, medians: the frame's GPU span is what counts (Apple GPUs overlap passes, so pass times overstate). The CPU
numbers are wall time per frame of unpaced runs at the scripts' own 640x360, where the CPU is the limit (1,200 frames
minus 300, three rounds). `scripts/culling_sweep.py` also runs every scene over MCP, twice (three times for the field),
for the meshlets each cascade drew, the pass sums and the slice ([culling_timings.json](culling_timings.json); its
frame times carry the machine's noise, and the field's and the slice's frames are CPU-bound, so their GPU spans clock
down and only the shadow passes' sums compare).

### Results

GPU frame (ms), Bistro, M5 Max:

| Scene | Size | Before | After | No maps | Shadows added before | Shadows added after |
|---|---|---|---|---|---|---|
| Bistro, sunset | 1920x1080 | 4.38 | 2.44 | 2.01 | +2.36 | **+0.43** |
| Bistro, sunset | 2560x1440 | 5.58 | 3.59 | 3.04 | +2.54 | **+0.55** |
| Bistro, noon | 1920x1080 | 2.97 | 2.53 | 1.99 | +0.98 | **+0.54** |
| Bistro, noon | 2560x1440 | 4.04 | 3.67 | 3.04 | +1.00 | **+0.63** |

Meshlets drawn per cascade, and the shadow passes' sum (ms, 2560x1440):

| Scene | Before: meshlets | After: meshlets | Before: passes | After: passes |
|---|---|---|---|---|
| Bistro, noon | 8,798 / 11,431 / 197 / 0 | 1,930 / 4,817 / 0 / 0 | 4.13 | 0.81 |
| Bistro, sunset | 25,647 / 48,084 / 37,784 / 25,979 | 315 / 5,563 / 0 / 0 | 5.35 | 0.56 |
| SP-16 field, ground | 1,077 / 15,209 / 185,917 / 261,374 | 673 / 4,552 / 0 / 0 | 11.41 | 1.07 |
| Slice landed, s = 0.01 | | | 1.02 | 0.65 |
| Slice landed, s = 1 | | | 0.94 | 0.65 |

(The far cascades show 0 after: kept from earlier frames, as the scenes are still. Drawn with the receiver test alone,
the field's take 106,000 and 117,000.)

CPU per frame (ms, unpaced at 640x360): Bistro at sunset 4.80 before, 1.25 after (0.87 without maps); at noon 4.54
before, 1.31 after.

Other settings, Bistro at 2560x1440, after (GPU frame, ms; 3.06 without maps):

| | 4 x 2048² to 1000 m | 3 x 2048² to 1000 m | 2 x 1024² to 250 m |
|---|---|---|---|
| At sunset | 3.59 (+0.55) | 3.48 (+0.42) | 3.71 (+0.65: two cascades keep nothing) |
| At noon | 3.67 (+0.63) | 3.53 (+0.49) | |

Where Bistro's shadow cost goes now (2560x1440, pass times): the receivers pass 0.12 to 0.14 ms, the two near cascades'
draws 0.04 to 0.15 ms each, the culling chain (eight small passes) about 0.1 ms, all serial after the camera's
visibility pass, and the shading's lookups 0.11 to 0.15 ms.

The CPU: sampling a Bistro sunset run (`sample`, 6 s), the render thread spent 36% of its time in
`RenderGraph::compile` and 27% in `RenderGraph::describe` before (every pass that read the scene declared ~3,000 mesh
buffers and textures); with the scene as two read-only groups, 0.3% and 1%, and the thread waits for the GPU
(`begin_frame`) most of the frame. CPU ms per frame in the table includes MCP's stepping.

### Images

Every image is the build before's, frame by frame (FLIP against `main` at the same frame):

| Sequence | Frames | Mean FLIP | Worst pixel |
|---|---|---|---|
| Bistro at noon, still, 2560x1440 | 90 | 0.00000 | 0.028 |
| Bistro at sunset, still, 2560x1440 | 90 | 0.00000 | 0.012 |
| Bistro at noon, camera 1.3 cm a frame | 60, 61, 62, 120 | 0.00011 to 0.00031 | 0.054 |
| Bistro at sunset, camera 1.3 cm a frame | 60, 61, 62, 90, 120 | 0.00000 to 0.00001 | 0.023 |
| Bistro at sunset, flying 30 m/s and turning | 60, 61, 90, 120 | 0.00000 | 0.032 |
| Bistro at sunset, the sun moving 1.8 s of day a frame | 60, 61, 90, 120 | 0.00000 | 0.023 |
| SP-16 field, ground, 1920x1080 | 90 | 0.00070 | 0.290 |

The goldens with a sun (Bistro at noon and sunset, FlightHelmet and the runtime mesh in real time) match theirs at FLIP
0.00000; none was regenerated.

The camera walking at noon: frame 60 before, frame 60 and 61 after (crops enlarged 2x). The bollards' shadows keep
their edges as the view moves.

![](vch143_moving_noon.png)

The sun moving at sunset, frames 60, 90 and 120 (after): the sliver of sun on the far facade moves smoothly.

![](vch143_sun_moving.png)

The field's largest difference, before and after (enlarged 3x): the canopy's dappling, from the far cascades' 12.5%
wider texels.

![](vch143_field_worst.png)

There since VCH-142 (left: the VCH-142 build; right: this one): a lighter band where the first cascade ends on
Bistro's facade, with or without the blend between cascades. Left for VCH-143's remainder.

![](vch143_band.png)

### Findings

1. **Bistro's sun shadows cost 0.43 to 0.63 ms instead of 1.0 to 2.5**, sunset now cheaper than noon. At sunset the
   receivers and the light's occlusion leave 315 and 5,563 meshlets in the near cascades (from 25,647 and 48,084):
   a facade in shade only needs the first building toward the sun, and only where the camera sees it. The far
   cascades cost nothing while the scene is still.
2. **The receiver test is worth its serialization.** It makes the shadow work wait for the camera's depth instead of
   overlapping the camera's culling, yet occlusion alone, keeping the overlap, measured 0.2 to 0.35 ms slower.
3. **Occlusion needs depth order.** A cascade's depth used to end at twice its radius toward the light, flattening
   every farther caster onto one depth where none can hide another. Reaching the shadow distance (1 km in depth32
   float, under 0.1 mm a step) let the light's occlusion work; it changed no pixel.
4. **The cost is vertices, not texels**: a cascade costs the same at 1024² and 4096², so smaller maps would not have
   helped, and opaque casters reading only their position changed nothing measurable either.
5. **No new shimmer, no changed pixel**: every sequence above is the build before's frame by frame, the goldens
   included. Only the far cascades' texels grew (12.5%), visible as FLIP 0.0007 in the field's canopies.
6. **The CPU's 3.5 ms of graph work per Bistro frame is gone**: scene residency is two read-only groups.
7. **The field still pays about 1 ms of shadow passes**, now mostly phase 2's instance recheck: occluded grass
   instances re-read in phase 2 in append order, scattered over 112 MB of records. Vegetation LOD (VCH-139) shrinks it
   with the camera's own cost.
8. **The M3 Pro**: scaling by GPU size (18 cores against 40), four cascades would cost about 1.6 to 1.9 ms at 1440p,
   three about 1.3 to 1.5, above ADR 0062's 1.1 ms estimate for it. Unmeasured (VCH-097).
9. **A lighter band where the first cascade ends** on Bistro's facade, at noon and at sunset, was there in VCH-142's
   images and does not depend on the blend between cascades; left in VCH-143.

### Reproduce

```sh
python3 docs/research/sun-shadows/scripts/culling_sweep.py --new $PWD --old <main tree> --rounds 2 \
    --out docs/research/sun-shadows/culling_timings.json
python3 docs/research/sun-shadows/scripts/culling_sweep.py --table docs/research/sun-shadows/culling_timings.json
VEACH_SHADOW_NO_RECEIVERS=1 VEACH_SHADOW_NO_OCCLUSION=1 VEACH_SHADOW_NO_CACHE=1 build/release/apps/lab/veach-lab ...
```
