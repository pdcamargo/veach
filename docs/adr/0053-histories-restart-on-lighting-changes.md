# 0053: Temporal histories restart on lighting changes

Date: 2026-11-27

## Status

Accepted. Amends [0042](0042-denoiser.md) (when the denoiser drops its history) and
[0051](0051-candela-approach.md) (Candela's reservoirs and cache).

## Context

The real-time renderer's temporal histories only knew about geometry: a disocclusion drops them, nothing else does.
When a light switched off, the denoiser's 32-frame history kept the old light, and the final image took over 20
frames to settle, with GI off as with Candela. Candela's reservoirs and its radiance cache held the old light too.
The owner's gate is 10 frames on the final image, and asked for the reset to come from the engine, not from
guessing at changes in the image.

## Decision

- **The engine says when lighting changed.** `RenderModule` observes `DirectionalLight`, `PointLight`, `SpotLight`,
  `Sky` and `MeshInstance` (whose materials may emit) being set or removed, and sets `RenderWorld::lighting_changed`
  for the next extract.
- **The real-time renderer restarts what holds light** on that frame: the denoiser drops its history (diffuse,
  specular, moments), Candela skips its reservoirs' temporal reuse, and Candela's cache cells keep only this frame's
  samples (cells without one hold no value until one arrives). Global for now; per region if a scene's lights change
  often.
- Observers, not Flecs queries with change detection: those reported every table holding a transform as changed,
  every frame, although only the transform column was written.

## Consequences

- A light switched off settles in 1 frame on the final image (it took over 20), with GI off and with Candela;
  `tests/candela_final.py` gates it on the Cornell box and Bistro.
- The frame after a change is the noisiest: it starts from one frame's samples, as at a disocclusion.
- A light animated every frame restarts the histories every frame: noisy, but never stale. So does a system that
  sets a light component every frame without changing it.
- Changes the engine does not observe (geometry moving, a material edited in place) still go through the
  histories' own lag.
