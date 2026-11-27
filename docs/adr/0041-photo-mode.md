# 0041: Photo mode: the reference path tracer, accumulated while the camera is still

Date: 2026-11-27

## Status

Accepted. Re-scopes the pathtracer-era reference path tracer (old ADRs 0034, 0035 and 0048) as photo mode.

## Context

The engine has an unbiased, progressive GPU path tracer that shares the materials, lights and sky with the real-time
renderer. In the old plan it was ground truth for real-time ray-traced lighting. With that lighting gone
([0039](0039-realtime-renderer-is-raster-pbr.md)) it keeps two uses: a feature games can offer (a photo mode: freeze
the camera, let the image converge), and a reference for debugging materials, lights and the sky.

## Decision

**The component.** `PhotoMode { samples_per_frame = 1, max_bounces = 8 }` on a camera entity (Scriptable). Its view is
path traced instead of rendered in real time. The scene renderer (`engine/render/src/scene_renderer.cpp`) builds BLASes
and the TLAS only then.

**Accumulation, no tricks.** Samples accumulate in an `rgba32_float` texture. Accumulation restarts when anything it
depends on changes: the view, the target size, `max_bounces`, the sky, exposure, materials, lights, instances or a
reloaded mesh (a hash computed each frame), or on request (MCP `photo_mode`). No denoiser, no temporal reuse: it
converges by accumulation, which is what "while the camera is still" means.

**The path tracer** (`engine/render/shaders/path_tracer.slang`, compute with inline ray queries):

- one camera ray per pixel per sample, jittered in the pixel; PCG hash RNG of (pixel, sample index, dimension), so the
  same sample index gives the same numbers on every backend;
- at each hit, `evaluateSurface` and the glTF BRDF ([0035](0035-material-model.md)); next-event estimation to one light
  (a punctual light, or an emissive triangle chosen by area × emission weight) with MIS against BSDF sampling;
- sized lights ([0036](0036-lights-in-photometric-units.md)) are sampled over their extent: soft shadows;
- the sky ([0038](0038-physical-sky.md)) for escaped rays (the sun's disk for camera rays only), aerial perspective on
  camera hits; Russian roulette after 3 bounces;
- **alpha**: closest-hit rays trace everything as opaque and continue past uncovered texels (up to 16 layers);
  visibility rays trace opaque instances first, then alpha-tested ones as non-opaque candidates in one traversal;
- NaN or infinite samples are left out and reported as `render.nonfinite_samples`; an accumulation that reads back
  something other than what it wrote is `render.history_lost`. Both fail the run.
- One closest-hit `trace()` call site per kernel: Metal's compiler crashes with two
  (`tools/metal-repros/path-tracer-second-trace`).

**Without hardware ray tracing** ([0003](0003-platforms-and-hardware-target.md)) photo mode logs
`rhi.backend_unavailable` once, with a hint, and the camera renders in real time.

**Tools.** MCP `photo_mode { samples }` restarts the accumulation, renders exactly that many samples per pixel of the
current state and returns the PNG: ground truth on demand for agents. Photo-mode goldens (Cornell box, FlightHelmet)
run on Metal and DX12/WARP.

## Consequences

- Games get a photo mode for free, and every material and light has a reference image.
- Slow by design; interactive use is "hold still, then wait".
- Not part of the real-time frame budget, and never a real-time feature.

## Alternatives considered

- **Delete the path tracer.** Loses the only physically correct reference for materials and lights, and a feature
  games in this genre ship.
- **Add a denoiser for faster convergence.** Reintroduces the temporal machinery the reset removed, and a denoised
  image is no longer a reference.
- **Show the real-time image while the camera moves and switch to path tracing when it stops.** Nicer to use, but a
  game decides when photo mode starts by adding the component; an automatic switch can come later as a module.
