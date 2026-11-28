# VCH-144: temporal anti-aliasing, measured (and SP-16's foliage verdict)

Builds [ADR 0061](../../adr/0061-temporal-accumulation-no-upscaling.md) in the GPU-driven renderer
([ADR 0093](../../adr/0093-gpu-driven-core.md)); decisions in [ADR 0097](../../adr/0097-temporal-aa-as-built.md),
requirements in the [spec](../../specs/temporal-aa/spec.md). SP-16 ([findings](sp-16-visibility-vs-forward.md)) moved
its last kill criterion here: is ghosting on thin alpha-tested foliage acceptable by eye, or does the renderer need
forward shading with 4x MSAA and alpha to coverage? Apple M5 Max, Metal, release build, 2026-11-28.

**Verdict: TAA is acceptable on foliage; keep ADR 0060, no MSAA fallback.**

- Still, on SP-16's field through the engine (`VEACH_SP16=engine`), TAA lowers FLIP against a 16-sample reference
  by 10 to 19 % (SP-16's 4x MSAA managed 6 to 17 % on its own renderer) and is close to it by eye: canopies against
  the sky, single-pixel grass at a distance, the dotted wires and sparkle in Bistro all resolve.
- Moving, it adds no shimmer (the far view's frame-to-frame change in a slow pan: no-AA 2.5 times the reference's,
  TAA below it), and strafing through the grass at 6 and 18 m/s or panning shows no ghost trails by eye.
- The price: in motion the image is softer than the reference, so FLIP in motion on the ground view is 9 to 27 %
  above no-AA's, which is sharp but crawls; a faint one-pixel rim can follow a fast silhouette. Stars are untouched.
- Cost: 0.41 ms at 1080p and 0.65 ms at 1440p (resolve and sharpening), above ADR 0061's 0.08 ms estimate.

## What was built

| Where | What |
|---|---|
| `engine/render/shaders/taa.slang` | `taa_resolve` (neighborhoods in groupshared memory, nearest-depth motion, surface-id and depth disocclusion over the 3x3 history texels, 5-tap Catmull-Rom history, variance clipping in YCoCg, Karis's luminance-compressed blend, 1/n while young) and `taa_sharpen` (a limited unsharp mask scaled by motion) |
| `engine/render/src/realtime_renderer.cpp` | the jittered projection for culling, visibility and shading; last frame's jittered and unjittered matrices; the history (two `rgba16_float` + two `rg32_uint`, ping-pong) and its invalidation; the passes between shading and the backdrop |
| `realtime_visibility.slang`, `classify.slang` | motion without the jitter; the sky's from unjittered rays |
| `backdrop_composite.slang` | the backdrop after the resolve, on the share of the pixel that shows the sky (alpha 1 + share) |
| `InstanceRecord::id` | 24 bits from the drawing entity (`surface_id`), stable across frames |
| `AntiAliasing { temporal }`, `RenderView::temporal_aa`, `jitter`, `render::taa_jitter`, `jittered_view_projection`, `VEACH_DISABLE_TAA` | the camera's switch and the views' jitter (matrices unjittered) |
| `tests/cmake/golden_test.cmake` | goldens without TAA unless `TAA`; `same_image_test.cmake` compares by FLIP with `MAX_MEAN` |
| `apps/lab/src/sp16_spike.cpp` | `VEACH_SP16_PAN` and `VEACH_SP16_STRAFE` for the engine field, `VEACH_SP16_TAA=0` |

## The field, still

1920 x 1080, 16 frames. The reference is the engine at 7680 x 4320 without TAA, box-filtered in linear light (16
samples per pixel). Crops: **no AA | TAA | 16-sample reference**, 240 x 135 pixels enlarged 4 times (SP-16's spots).

| Mean FLIP against the reference | no AA | TAA |
|---|---|---|
| ground | 0.0362 | 0.0327 (-10 %) |
| mid | 0.0652 | 0.0561 (-14 %) |
| far | 0.1246 | 0.1013 (-19 %) |

The whole frame's FLIP includes a horizon band the field draws differently at 7680 x 4320 (aerial perspective at the
field's edge); away from it (rows 0 to 460 and 520 to 1080 of the ground view) TAA is 0.0208 and 0.0273 against no
AA's 0.0233 and 0.0305.

![](vch-144/ground_crop0.png)
![](vch-144/ground_crop1.png)
![](vch-144/mid_crop0.png)
![](vch-144/mid_crop1.png)
![](vch-144/far_crop0.png)
![](vch-144/far_crop1.png)

By eye: the stair steps on canopy edges and leaf cards are gone, the far grass becomes the texture the reference shows
instead of dark and light dots, and the leaf interiors keep their detail. Over the whole far view TAA is about 2 %
darker than the reference: the luminance weighting favors the darker sample where grass and ground alternate within a
pixel. The ground view at full size: [ground_taa.png](vch-144/ground_taa.png).

## The field, moving

The camera pans (`VEACH_SP16_PAN`, degrees a frame) or strafes (`VEACH_SP16_STRAFE`, meters a frame), frame 40
against the reference rendered at the same pose. FLIP excludes the horizon band (rows 460 to 520).

| Ground view | no AA | TAA |
|---|---|---|
| strafe 0.1 m a frame (6 m/s): trees / grass | 0.0226 / 0.0401 | 0.0270 / 0.0451 |
| pan 0.25 degrees a frame (15 degrees/s) | 0.0233 / 0.0299 | 0.0266 / 0.0326 |
| pan 0.02 degrees a frame (SP-16's slow pan) | 0.0232 / 0.0307 | 0.0242 / 0.0390 |

| Slow pan (0.02 degrees a frame) | frame-to-frame change: reference | no AA | TAA | FLIP no AA / TAA (upper, lower rows) |
|---|---|---|---|---|
| ground | 0.0121 | 0.0149 | 0.0113 | 0.023 / 0.024, 0.031 / 0.039 |
| mid | 0.0248 | 0.0397 | 0.0224 | 0.056 / 0.053, 0.069 / 0.068 |
| far | 0.0446 | 0.1129 | 0.0383 | 0.035 / 0.033, 0.191 / 0.159 |

The frame-to-frame change is the mean absolute difference of frames 40 and 41 (red, 0 to 1), as SP-16 measured it;
the reference's is the real motion, so what a path adds above it is shimmer. No AA adds 24 to 153 %; TAA none.

**Strafing past a tree, and grass against a rock** (no AA | TAA | reference):

![](vch-144/strafe_canopy.png)
![](vch-144/strafe_rock.png)

**Panning, and the slow pan in the near grass** (no AA | TAA | reference):

![](vch-144/pan_canopy.png)
![](vch-144/slow_pan_grass.png)

**Four consecutive frames strafing at 6 m/s** (TAA, then no AA): the bush and grass move without trails.

![](vch-144/strafe_sequence_taa.png)
![](vch-144/strafe_sequence_off.png)

**18 m/s** (no AA frame 30 | TAA frames 30, 31):

![](vch-144/fast_strafe.png)

By eye: no trails behind the grass or the leaf cards; edges in motion are smooth but softer than the reference. The
blend was tuned on these sequences: with this frame's weight at 0.2 until a pixel moved 8 pixels a frame, the slow pan
lagged into blur (FLIP 0.048) and grass tips against the rock dragged short horizontal smears; with 0.5 from one
pixel a frame, an edge weight where the surface is found only beside where it was, and the motion-scaled sharpening,
it is 0.039 and the smears are gone. Testing disocclusion on the landing texel only reset every foliage edge each
frame (jitter alternates which surface an edge pixel shows): static FLIP on the grass 0.032 against 0.027 with the 3x3.

## Other scenes

**Bistro at noon** (no AA | TAA; 640 x 360): the wires stop being dotted, the wrought iron resolves, the specular
sparkle on the facade goes.

![](vch-144/bistro_noon.png)

**The slice in flight at 200 m** (s = 1, the autopilot demo at 2560 x 1440; no AA | TAA): the craft's edges are
smooth over the moving terrain, without trails; a faint light rim follows its left edge. Over the demo's twenty
views, from orbit to the dug hole and twilight, mean FLIP between no AA and TAA is 0.001 to 0.017.

![](vch-144/slice_ship_200m.png)

**Deep space**: stars are drawn after the resolve along unjittered rays, so they are bit-identical with TAA on and off
(`space_backdrop.luau`: FLIP 0; the slice's descent view: the stars' pixels identical).

![](vch-144/slice_stars.png)

**The temporal golden** (`taa_motion.luau`, a turning helmet and a panning camera before the backdrop, frame 24;
no AA | TAA):

![](vch-144/taa_motion.png)

## GPU time

Render-graph timestamps (`VEACH_GPU_TIMINGS=120,120`), median of 120 frames after 120, the field panning slowly.

| ms | `taa` | `taa sharpen` |
|---|---|---|
| 1920 x 1080, ground / mid / far | 0.33 / 0.37 / 0.33 | 0.08 / 0.08 / 0.07 |
| 2560 x 1440, ground / mid / far | 0.50 / 0.54 / 0.54 | 0.12 / 0.12 / 0.12 |
| Bistro, 2560 x 1440, still | 0.42 | 0.12 |

For scale, the display pass (one read, one write per pixel) takes 0.07 ms at 1080p. Groupshared neighborhoods and
reading the history ids beyond the landing texel only when it misses brought the resolve from 0.44 to 0.33 ms; dropping
the instance-id fetch, the separate resolved write or four of the five history taps (each tried alone) changed nothing
measurable: the pass is bound by its memory traffic as a whole. Next savings (VCH-145): the sharpening folded into the
display pass, the ids packed into 32 bits.

## Engine findings

1. **The render origin's 64 m jump keeps the history.** Last frame's matrices and transforms are moved into this
   frame's render space (0093), and the history is in screen space. Measured on the frame right after the jump against
   the same strafe 32 m away, where the origin does not move: mean FLIP 0.004 (two offsets that never jump: 0.003);
   with the jump not undone, the disocclusion test throws the history away and the frame is one jittered frame: 0.019
   (`taa: the history survives a render-origin jump`).
2. **Photo mode must not be jittered**: it hashes the view to restart its accumulation, so a jittered `RenderView`
   would restart it every frame. Hence views stay unjittered and the real-time renderer offsets its own projection.
3. **Goldens stayed byte-identical** with TAA off in them; a byte-compare culling test and an MCP test that compares
   two frames pixel for pixel turn TAA off on their camera.
4. **The horizon band of the SP-16 field** is drawn differently at 7680 x 4320 than at 1080p (aerial perspective at the
   field's edge), with or without TAA; it dominates whole-frame FLIP against the references.

## Reproduce

Release build. The lab runs from `apps/lab`:

```sh
VEACH_SP16=engine VEACH_SP16_VIEW=ground veach-lab --rhi metal --headless --frames 16 --screenshot taa.png
VEACH_SP16=engine VEACH_SP16_VIEW=ground VEACH_SP16_TAA=0 veach-lab ... --screenshot off.png
VEACH_SP16=engine VEACH_SP16_VIEW=ground VEACH_SP16_TAA=0 VEACH_SP16_SIZE=7680x4320 veach-lab ... --screenshot ref4k.png
VEACH_SP16=engine VEACH_SP16_STRAFE=0.1 veach-lab ... --frames 40 --screenshot strafe40.png
VEACH_SP16=engine VEACH_SP16_SIZE=2560x1440 VEACH_SP16_PAN=0.05 VEACH_GPU_TIMINGS=120,120 veach-lab ... --frames 260
veach imgdiff ref.png taa.png --max-mean 1
```

The reference is `ref4k.png` box-filtered 4 x 4 in linear light; frame-to-frame change and crops as SP-16's
`shots.py`.
