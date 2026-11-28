# Temporal anti-aliasing

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

The real-time renderer's anti-aliasing ([ADR 0061](../../adr/0061-temporal-accumulation-no-upscaling.md), as built:
[ADR 0096](../../adr/0096-temporal-aa-as-built.md)): the projection is jittered by a sub-pixel Halton offset each
frame, and each pixel is blended with its history, last frame's result reprojected by the motion vectors, at native
resolution. It removes the stair steps, crawling foliage and specular sparkle that a visibility buffer without MSAA
shows, on every machine, at a cost of 0.4 ms at 1080p on the M5 Max.

## Motivation

SP-16 kept the visibility buffer (ADR 0060) and moved its last kill criterion here: whether TAA on thin alpha-tested
foliage ghosts unacceptably, judged by eye on its vegetation field, against the Metal-only 4x MSAA with alpha to
coverage kept in the RHI as the fallback. Without it the image aliases visibly (SP-16's crops). Backlog VCH-144.

## Requirements

- The real-time renderer MUST jitter its projection by the Halton (2, 3) sequence of length 8, indexed by the frame
  counter, never by time, so record and replay reproduce images.
- `RenderView`'s matrices MUST stay unjittered; the jitter is `RenderView::jitter`, applied by the real-time renderer
  to culling, the visibility pass and shading only. Photo mode MUST never be jittered.
- Motion vectors MUST leave the jitter out: a still surface under a still camera has none. Every geometry feature
  writes them (cooked meshes, runtime meshes, terrain chunks); the sky gets the camera's rotation only.
- The resolve MUST run on pre-exposed radiance before the space backdrop, metering and the display transform, with
  luminance weighting (Karis's c / (1 + luma)), and rescale the history to this frame's exposure.
- It MUST clip the history to this frame's 3x3 neighborhood in YCoCg (variance clipping inside the min/max box).
- It MUST reject history that shows another surface: by a stable per-entity surface id and the depth last frame's
  camera would have seen the surface at, so disocclusions and teleports start over instead of ghosting.
- It MUST survive the render origin moving by 64 m (the history is in screen space; last frame's matrices and
  transforms are moved into this frame's render space).
- It MUST start over on a camera switch, a render-frame switch, a resize, a switch of exposure control, or a frame
  in which it did not run.
- Stars MUST NOT be filtered in time: the space backdrop is drawn after the resolve along unjittered rays, on the
  share of each pixel that shows the sky (blended by the resolve like the color).
- NaN or infinite pixels MUST still reach auto-exposure's check and MUST NOT enter the history.
- Radiance MUST stay within the fp16 maximum (ADR 0082).
- A camera MAY turn it off (`AntiAliasing { temporal = false }`), and `VEACH_DISABLE_TAA=1` turns it off everywhere.
- Golden tests MUST stay deterministic: goldens render with TAA off unless they are temporal goldens, which render a
  fixed number of frames of the fixed sequence.

## API sketch

```lua
camera:set(c.AntiAliasing, { temporal = false })  -- default (no component): on
```

```cpp
struct AntiAliasing { bool temporal = true; };  // on a camera entity, Scriptable
// RenderView: bool temporal_aa; math::vec2 jitter (pixels, y down)
math::vec2 render::taa_jitter(std::uint64_t frame);
math::mat4 render::jittered_view_projection(const math::mat4& vp, math::vec2 jitter, std::uint32_t w, std::uint32_t h);
// The frame blackboard's "hdr_color" is the resolved image after the real-time renderer.
```

```sh
VEACH_DISABLE_TAA=1 veach-lab ...   # every camera without TAA (the goldens)
```

## Acceptance criteria

- **taa.AC1** — The jitter is Halton (2, 3), points 1 to 8 minus half a pixel, by frame, period 8, averaging within
  0.07 px of the center; the jittered projection moves every point by the jitter in pixels (y down) and leaves depth
  alone (`tests/taa_tests.cpp`).
- **taa.AC2** — With TAA on, a still wall and the sky read no motion (under 1e-4 UV) and a moving box its own
  (`[gpudriven.AC5]`, which renders with TAA on and a non-zero jitter).
- **taa.AC3** — A turning helmet before the space backdrop under a panning camera, after 24 frames, matches the
  golden `tests/golden/taa_motion.png`, byte-identical from run to run (`golden: temporal AA in motion`).
- **taa.AC4** — Right after the render origin jumps 64 m, the image matches the same strafe where the origin stays
  put within mean FLIP 0.008 (measured 0.004; 0.019 when the jump is not undone and the history is lost)
  (`taa: the history survives a render-origin jump`).
- **taa.AC5** — On SP-16's field, against 16-sample references, TAA lowers FLIP in all three still views and removes
  the shimmer no-AA adds in a slow pan, without ghost trails visible by eye in a strafe and a pan; deep space is
  unchanged (stars bit-identical); measured in [VCH-144's findings](../../research/spikes/vch-144-taa.md).

## Out of scope

- Upscaling of any kind (ADR 0061). A spatial render scale for the M1 tier waits on ADR 0064.
- Folding the sharpening into the display pass (0.08 ms at 1080p), packing the history ids smaller, and a per-material
  "reactive" mask for transparency and particles when those exist: VCH-145.
- GTAO's temporal filter and cloud reprojection reuse the same motion vectors and blackboard; they come with those
  features.

## Open questions

None.

## Links

- ADRs: [0061](../../adr/0061-temporal-accumulation-no-upscaling.md), [0096](../../adr/0096-temporal-aa-as-built.md),
  [0093](../../adr/0093-gpu-driven-core.md), [0085](../../adr/0085-amendments-from-vch-111.md),
  [0082](../../adr/0082-amendments-from-sp-14.md), [0091](../../adr/0091-space-backdrop.md)
- Backlog: VCH-144, VCH-145
- Guides: [rendering](../../guides/rendering.md)
- Research: [VCH-144 findings](../../research/spikes/vch-144-taa.md), [SP-16](../../research/spikes/sp-16-visibility-vs-forward.md)
