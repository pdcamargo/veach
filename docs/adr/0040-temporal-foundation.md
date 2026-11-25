# 0040: Temporal foundation: render resolution, jitter, a surface pass, motion, history and disocclusion

Date: 2026-11-25

## Status

Accepted. Supersedes the "shading in one compute pass" part of [0037](0037-realtime-renderer-visibility-buffer.md).

## Context

The [temporal spec](../specs/temporal-direct-lighting/spec.md) makes the real-time renderer temporal: ReSTIR
reuses last frame's reservoirs ([0041](0041-restir-di.md)), the denoiser accumulates history
([0042](0042-denoiser.md)), and the upscaler reconstructs output pixels from jittered frames
([0043](0043-upscaling.md)). All three need the same things: where each pixel was last frame, whether that
history belongs to the same surface, and the surfaces of both frames. v0 rebuilds the surface inside one shading
pass and throws it away, renders at the output resolution, and has no history.

## Decision

**Two resolutions per view.** The render resolution is the output resolution times `Upscaling.render_scale`
(default 0.75, clamped to 0.5..1, rounded to whole pixels). Visibility, surfaces, lighting and denoising run
there; the upscaler writes the output resolution; the display pass (exposure is applied before upscaling,
[0043](0043-upscaling.md)) runs at the output. `Upscaling.mode = none` renders at the output resolution with no
jitter and no upscaler.

**Jitter.** While an upscaler runs, the projection is offset by a Halton(2,3) sequence of 16 phases, in render
pixels. The visibility pass rasterizes with it, and the surface pass rebuilds camera rays with it, so they agree.
Motion and every reprojection use the unjittered matrices.

**A surface pass.** One compute pass resolves the visibility buffer into a compact G-buffer, evaluating each
pixel's material once (as v0's shading did), and later passes read it instead of the visibility buffer:

| Target | Format | Contents |
|---|---|---|
| `surface position` | `rgba32_float` | world position, w = linear view depth (0 = no geometry) |
| `surface normal` | `rgba16_float` | shading normal, w = roughness |
| `surface geometric` | `rgba16_float` | geometric normal (ray offsets), w = metalness |
| `surface diffuse` | `rgba16_float` | diffuse albedo, w = 1 if the material is double sided |
| `surface specular` | `rgba16_float` | specular F0, w unused |
| `surface emission` | `rgba16_float` | emission seen directly |
| `motion` | `rg16_float` | previous minus current pixel position, render pixels, unjittered |

The textures are created by the render graph each frame; the ones history needs are copied into persistent
per-view textures at the end of the frame (below). This keeps v0's property that a material is evaluated once
per pixel by the code the path tracer uses; it gives up recomputing surfaces in every pass, which ReSTIR's
neighbor and previous-frame evaluations would make expensive.

**Motion vectors are analytic.** `InstanceRecord` gains `previous_object_to_world`. The surface pass
transforms the pixel's object-space point by it and projects with the previous unjittered view-projection. A
moving camera and a moving object both reproject exactly; deforming meshes would add a previous vertex position
later (no animation exists yet).

**Per-view history.** The scene renderer keeps, per view (keyed by the camera entity), persistent textures at the
render resolution: the previous surface position, normal, geometric, diffuse and specular targets; ReSTIR's
reservoirs (two, ping-ponged); the denoiser's history; the TAAU history (output resolution). They enter the render
graph as imported resources, so passes declare them like any other. A `history_valid` flag is false on the first
frame and after a reset.

**Resets.** History is dropped on the first frame, a resize, a render-scale or upscaler change, and a camera cut:
a translation of more than 2 m or a rotation of more than 45 degrees in one frame. Lights changing (count or
records) resets ReSTIR's reservoirs only.

**Reprojection and disocclusion** live in one Slang module, `veach/temporal.slang`, that every temporal pass
imports. `reproject(pixel, motion)` returns the previous position and four bilinear taps, each valid only if it
lies on the screen, its previous surface lies on the current surface's plane (distance under 1% of the view
depth) and its normal is within 25 degrees. Callers renormalize the weights of the valid taps; zero valid taps is
a disocclusion. The `history` debug view shows it.

## Consequences

- One definition of "same surface last frame": ReSTIR, the denoiser and TAAU cannot disagree about it.
- The G-buffer costs memory: about 66 bytes per render pixel, twice for history, about 270 MB at 1080p with
  reservoirs and denoiser history. Acceptable on our targets; packing is an optimization for later.
- Every new pass is in `gpu_timings`; the visibility buffer stays the only rasterized target.
- The surface pass rebuilds a moving instance from its current transform, so motion is exact for rigid motion only.

## Alternatives considered

- **Keep one shading pass and reconstruct surfaces on demand:** ReSTIR evaluates neighbors' and the previous
  frame's surfaces several times per pixel; re-running materials (texture fetches included) for each is the cost
  the G-buffer avoids, and the previous frame's visibility buffer would need last frame's instance data anyway.
- **Rasterized motion vectors (a second target in the visibility pass):** same result for rigid motion, but the
  vertex shader would need both transforms and the target would be written for overdrawn fragments too.
- **Depth-only disocclusion:** misses surfaces at the same depth with different orientation (wall corners); the
  plane and normal tests cost two more texture reads.
- **Detecting cuts from a component flag only:** scripts would have to set it on every teleport; the distance
  threshold catches them without help (a flag can be added if a game needs one).
