# 0038: Physical camera exposure and the AgX tonemapper

Date: 2026-11-25

## Status

Accepted. Where exposure is applied (before upscaling, not in the display pass) superseded by
[0043](0043-upscaling.md).

## Context

Lights are in photometric units ([ADR 0034](0034-lights-and-reference-path-tracer.md)): a sunlit scene is
around 100 000 lux, a lamp a few hundred candela. Mapping that to a display needs a camera exposure grounded
in the same units and a tonemapper that compresses highlights without shifting hues. Both renderers must share
the display transform, or comparing them compares tonemappers. Until now the path tracer scaled by
`1 / (1.2 · 2^ev100)` and clamped.

## Decision

**Exposure** comes from a physical camera. `Camera` keeps `ev100` (exposure value at ISO 100) and gains
`aperture` (f-number), `shutter` (seconds) and `iso`. When `ev100` is set to 0 the camera derives it:
`ev100 = log2(N² / t · 100 / S)`. The exposure scale is the standard saturation-based one,
`1 / (1.2 · 2^ev100)`: scene luminance in nits times this scale is the value the tonemapper receives. There is no
automatic exposure in v0 (a histogram-based one is a later addition on the same parameters).

**Tonemapper: AgX** (Troy Sobotka's, as in Blender 4 and Godot), with its base look: the input is converted to
the AgX log space (−12.47 to +4.03 EV around middle grey 0.18), shaped by its sigmoid, and converted back,
then sRGB-encoded. It desaturates very bright values toward white instead of clipping each channel, so a hot
emissive or a sunlit wall keeps its hue at moderate brightness and burns out gracefully.

**One pass for both renderers.** The real-time renderer and the path tracer write linear radiance (nits) to
an HDR target; the display-transform pass applies exposure, AgX and sRGB encoding into `scene_color`. Goldens
and comparisons see the same transform.

## Consequences

- Scenes are lit and exposed in real-world terms: `ev100` around 15 for daylight, 8 to 10 for interiors, near 0
  for the dim Cornell box. Values from photography transfer directly.
- Images change once: every golden is regenerated with the new transform, on purpose.
- AgX is darker in the midtones than a clamp at the same exposure; scenes set their `ev100` for it.
- Comparisons between the renderers happen after the same transform, so tonemapping cancels out; FLIP sees
  display-referred images, as a viewer would.

## Alternatives considered

- **ACES (fitted, or the full RRT+ODT):** the usual film look, but it skews saturated colors (bright blues turn
  purple, reds orange) and clips hues in the highlights. AgX handles those cases better.
- **Khronos PBR Neutral:** keeps base colors close to their authored values, which suits product viewers; it
  compresses highlights less gracefully for scenes with strong lights.
- **Reinhard / clamping:** no highlight shoulder worth the name, or hue shifts per channel.
