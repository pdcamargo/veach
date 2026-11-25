# 0043: Upscaling: one interface, MetalFX's temporal scaler on Apple GPUs, our own TAAU everywhere

Date: 2026-11-25

## Status

Accepted.

## Context

Rendering at 0.75 of the output resolution ([0040](0040-temporal-foundation.md)) cuts the lighting cost by
almost half; an upscaler turns the jittered render-resolution frames back into a full-resolution, anti-aliased
image (VCH-023). Apple GPUs ship MetalFX, whose temporal scaler is tuned for them. DX12 runs only on WARP in CI
([0031](0031-dx12-on-warp-in-ci.md)), and goldens are shared across backends ([0028](0028-golden-images-across-backends.md)),
so an upscaler must run there too. DLSS, FSR and XeSS come with vendor SDKs and licenses, which the owner decides
on first.

## Decision

**One renderer-side interface, two implementations.** The scene renderer picks an upscaler per view from
`Upscaling.mode`: `automatic` (MetalFX where the device supports it, else TAAU), `metalfx`, `temporal` (TAAU),
`none`. Both take the same inputs and produce the same output:

- input: exposed linear color (radiance times the camera's exposure, `rgba16_float`), reverse-Z depth and motion
  (render pixels, unjittered) at the render resolution; the jitter; a reset flag (history reset, [0040](0040-temporal-foundation.md));
- output: exposed linear color at the output resolution; the display pass then applies AgX only
  ([0038](0038-exposure-and-tonemapping.md)'s exposure moves before the upscaler, so both upscalers see values
  near 0..1).

**MetalFX through the RHI.** `rhi::Device::create_temporal_scaler(desc)` returns a handle (or
`rhi.backend_unavailable` where MetalFX's temporal scaler is unsupported, and always on DX12 and null);
`CommandList::temporal_scale(handle, params)` encodes it. The render graph treats it as a pass that reads color,
depth and motion and writes the output, like any other. The Metal backend links MetalFX (a system framework).

**TAAU, our own, in Slang.** One compute pass at the output resolution:

- reconstruct the current frame at each output pixel from the 3x3 nearest render samples with a Gaussian weight
  on their jittered positions;
- sample history with a 5-tap Catmull-Rom filter at the reprojected position (motion dilated to the nearest-depth
  of the 3x3 neighborhood, so edges carry their foreground motion);
- clip history to the current neighborhood's YCoCg variance box (gamma 1);
- blend with weight `max(0.04, 1 / samples_in_history)`, luminance-weighted against fireflies; disoccluded
  pixels (no valid history, [0040](0040-temporal-foundation.md)) take the current frame only.

At `render_scale = 1` it is a TAA. Its history is a persistent output-resolution texture per view.

**Goldens use TAAU** so one image serves Metal and DX12; a Metal-only test checks MetalFX against TAAU's image.

## Consequences

- Apple GPUs get the vendor's upscaler; CI and Windows get a working one with no SDK.
- Two upscalers to keep in step on inputs; a third (FSR, DLSS) is another implementation of the same interface,
  after a license decision.
- Exposure is applied before upscaling: auto-exposure (later) must feed the upscaler's input, not the display.
- TAAU is simpler than MetalFX and will soften fine detail more at 0.5 scale; 0.75 is the default for that reason.

## Alternatives considered

- **MetalFX only, no fallback:** DX12 and CI would have no temporal path, and goldens would split by backend.
- **TAAU only:** works everywhere, but leaves Apple's tuned scaler unused on our primary platform.
- **FSR 2/3 as the fallback:** open source (MIT) and good, but a large integration (its own shader framework and
  a Metal port) for this milestone; a candidate behind the same interface later.
- **Upscale after tonemapping:** cheaper bandwidth, but MetalFX and TAAU both resolve HDR edges better in linear,
  and highlights would clip before reconstruction.
