# Photo mode: the reference path tracer

**Status:** Implemented
**Area:** render, runtime
**Owner:** dev lead

## Summary

A camera with a `PhotoMode` component is path traced instead of rendered in real time: an unbiased, progressive image
that accumulates while nothing changes. It is a feature games can offer and the engine's reference for materials,
lights and the sky ([ADR 0041](../../adr/0041-photo-mode.md)).

## Motivation

The reset kept the reference path tracer and re-scoped it ([ADR 0001](../../adr/0001-reset.md)). This spec takes over
the path-tracer criteria of [first-real-scene](../first-real-scene/spec.md) (its old scene.AC5 to AC8) and the sized
lights of the pathtracer-era soft-shadows spec.

## Requirements

- A camera with `PhotoMode` MUST be path traced with the shared materials, lights and sky, where the device has
  hardware ray tracing.
- Accumulation MUST continue while nothing changes and MUST restart when the view, target size, `max_bounces`, the
  sky, exposure, a material, a light, an instance or a mesh changes, or when MCP `photo_mode` asks.
- `samples_per_frame` MUST set the pace only, never the image.
- Lights with a size (`DirectionalLight.angle`, `PointLight.radius`, `SpotLight.radius`) MUST be sampled over their
  extent (soft shadows).
- NaN or infinite samples MUST be left out of the image and fail the run with `render.nonfinite_samples`; an
  accumulation that reads back something other than what it wrote MUST fail it with `render.history_lost`.
- Without hardware ray tracing, photo mode MUST log `rhi.backend_unavailable` with a hint and the camera MUST render in
  real time.
- MCP `photo_mode { samples, path? }` MUST restart the accumulation, render exactly `samples` samples per pixel of the
  current state without advancing simulation, and return the PNG.

## API sketch

```lua
camera:set(veach.components.PhotoMode, { samples_per_frame = 16, max_bounces = 8 })
```

```text
MCP: photo_mode { samples: 256, path: "out.png" }   -> image + "256 samples per pixel, 1280x720"
     gpu_timings                                      -> ... "photo_mode_samples": 256
```

## Acceptance criteria

- **photo.AC1** — The Cornell box (64 samples per pixel, with an alpha-masked panel and an emissive `GlowMaterial`)
  and FlightHelmet (32 samples) match their goldens on Metal and on DX12/WARP.
- **photo.AC2** — Over MCP: `photo_mode 64` matches the golden run; stepping frames continues the accumulation; moving
  the camera restarts it; 4x the samples is closer to a 256-sample reference.
- **photo.AC3** — A light with a NaN intensity fails the run with `render.nonfinite_samples`.
- **photo.AC4** — With `VEACH_DISABLE_RAY_TRACING=1`, a photo-mode scene logs that photo mode has no hardware ray
  tracing and renders in real time instead.

## Out of scope

Denoising, temporal reuse, switching to real time while the camera moves, depth of field and other camera effects,
spectral rendering, volumes.

## Open questions

None.

## Links

- ADRs: [0041](../../adr/0041-photo-mode.md), [0035](../../adr/0035-material-model.md), [0036](../../adr/0036-lights-in-photometric-units.md), [0038](../../adr/0038-physical-sky.md), [0003](../../adr/0003-platforms-and-hardware-target.md)
- Backlog: VCH-057
- Guides: [rendering](../../guides/rendering.md)
