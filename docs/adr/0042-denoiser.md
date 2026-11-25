# 0042: The denoiser: Veach's own, SVGF/ReLAX-style, on demodulated diffuse and specular

Date: 2026-11-25

## Status

Accepted. History feedback superseded by [0045](0045-temporal-implementation-amendments.md).

## Context

ReSTIR DI ([0041](0041-restir-di.md)) gives one sample per pixel of direct light; at 1080p it is noisy and its
noise is spatially correlated by reuse. The [temporal spec](../specs/temporal-direct-lighting/spec.md) asks for a
denoiser (VCH-022) that is stable with a still camera, does not ghost when the camera moves, runs on Metal and on
DX12 (WARP in CI), and fits a frame budget of a couple of milliseconds at 1080p on the M5 Max. Soft shadows,
Candela's GI and reflections will feed it later, so it is shared infrastructure, not a direct-light special case.

## Decision

**Our own filter, in Slang, on both backends.** Passes, at the render resolution, using the surfaces and
reprojection of [0040](0040-temporal-foundation.md):

1. **Demodulated signals.** Lighting writes diffuse irradiance (radiance over diffuse albedo) and specular
   radiance over the specular albedo term (F0-weighted, from the shared BRDF), so texture detail is never
   blurred. Emission seen directly is exact and not filtered; the ambient stopgap joins the diffuse signal.
2. **Temporal accumulation.** Each signal blends with its reprojected history (bilinear taps, valid taps only),
   with an exponential weight `1 / history_length`, history length up to 32 frames for diffuse and up to
   `8 + 24 · roughness` for specular. First and second luminance moments accumulate alongside for variance.
   Disoccluded pixels start at length 1: history is dropped, never blended.
3. **History fix.** Pixels with fewer than 4 frames of history take their variance from a 7x7 spatial estimate
   instead of the moments (no temporal data yet), so new pixels are filtered harder rather than left noisy.
4. **Spatial filter.** An edge-avoiding à-trous wavelet, 5 iterations (steps 1, 2, 4, 8, 16), 3x3 B-spline
   kernel; weights stop at plane distance (depth), normal difference and luminance difference scaled by the
   filtered variance. For specular, the normal weight sharpens with smoothness; mirror-like pixels (roughness
   under 0.05) use only the first iteration. The first iteration's output is the next frame's history (as SVGF).
5. **Anti-firefly.** Before accumulation, a sample brighter than its 3x3 neighborhood's mean plus 3 standard
   deviations in luminance is clamped to it.
6. **Remodulate:** `diffuse · albedo + specular · specular_albedo + emission`.

`RealtimeLighting.denoise = false` skips it (the noisy view and the unbiasedness test use that).

## Consequences

- One denoiser for every lighting signal we add; new signals add their own demodulation and history.
- Accumulation adds latency to lighting changes (a moving light takes a few frames to settle); the history
  length cap bounds it.
- Clamping fireflies is a small bias in the denoised image, by design; the unbiasedness test runs without it.
- About 12 full-screen passes at 1080p; the timings are in `gpu_timings` per pass.

## Alternatives considered

- **NVIDIA NRD (ReLAX/ReBLUR):** the best real-time denoisers available, but its integration layer (NRI) targets
  D3D12 and Vulkan, not Metal, and adopting a third-party denoiser needs the owner's license review first. Ours
  follows the same published ideas (SVGF, ReLAX) on our backends and our surfaces.
- **Intel Open Image Denoise:** Apache 2.0 and runs on Apple GPUs, but it is a neural denoiser built for final
  frames and previews, far over a 2 ms budget at 1080p.
- **Temporal accumulation only:** stable for a still camera, noisy for the first frames after every
  disocclusion, which is most of the screen while moving.
- **Denoising after upscaling:** filters 1.8 times as many pixels, and the upscaler's history would carry noise.
