# 0045: Temporal renderer: history in ping-ponged targets; the denoiser's history is its temporal accumulation

Date: 2026-11-25

## Status

Accepted. Supersedes two details of [0040](0040-temporal-foundation.md) and [0042](0042-denoiser.md), named below.

## Context

Implementing the [temporal spec](../specs/temporal-direct-lighting/spec.md) changed two details of its ADRs.

## Decision

**History targets are ping-ponged, not copied** (supersedes 0040's "created by the render graph each frame ... copied
into persistent per-view textures"). Every real-time target is a persistent per-view texture. The surfaces,
reservoirs, denoiser histories and TAAU's output exist twice; each frame writes one set and reads last frame's from
the other, by the parity of the view's frame count. No copy pass. A side effect the renderer relies on: every
handle is known when the frame is recorded, so one per-frame record (`RealtimeFrame`) carries them all and each
pass's parameter block stays under the RHI's 240 bytes.

**The denoiser's history is its temporal accumulation** (supersedes 0042's "the first iteration's output is the
next frame's history (as SVGF)"). `denoise_temporal` writes the accumulated, not yet spatially filtered, signal as
history (ReLAX-style). Feeding filtered output back blurs history further every frame; with ReSTIR's reuse already
correlating samples, keeping the spatial filter out of the feedback loop keeps edges stable.

**Disocclusion has a 3x3 fallback** (adds to 0040). When none of the four bilinear taps around a pixel's previous
position holds its surface (an edge whose sample moved with the jitter), the closest matching pixel of the 3x3
around it is used alone; the surface pass stores both decisions (4 and 9 bits).

## Consequences

- Twice the memory for history targets, none of the copy bandwidth.
- The denoiser converges as fast as its history length allows (32 frames diffuse), not faster through feedback.
- Silhouettes keep history under jitter instead of flickering between accumulated and fresh.

## Alternatives considered

- **Copying history at the end of the frame (as 0040 said):** about 100 MB of copies per frame at 1080p.
- **SVGF's filtered feedback:** faster convergence, at the cost of history that drifts toward the spatial filter.
