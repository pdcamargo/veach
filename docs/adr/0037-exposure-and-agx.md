# 0037: Physical camera exposure and the AgX tonemapper

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0038).

## Context

Lights are photometric ([ADR 0036](0036-lights-in-photometric-units.md)): a sunlit scene is around 100,000 lux, a
lamp a few hundred candela. Mapping that to a display needs exposure grounded in the same units and a tonemapper
that compresses highlights without shifting hues. The real-time renderer and photo mode must share the display
transform, or comparing them compares tonemappers.

## Decision

**Exposure** comes from a physical camera. `Camera` (`veach/render/camera.hpp`) has `ev100` (exposure value at
ISO 100), `aperture` (f-number), `shutter` (seconds) and `iso`. With `ev100` set to 0 the camera derives it,
`ev100 = log2(N² / t · 100 / S)` (`ev100_of`). The exposure scale is the saturation-based `1 / (1.2 · 2^ev100)`:
scene luminance in nits times this scale is what the tonemapper receives. No automatic exposure yet (a
histogram-based one would use the same parameters).

**Tonemapper: AgX** (Troy Sobotka's, as in Blender 4 and Godot), base look: the input goes to the AgX log space
(−12.47 to +4.03 EV around middle grey 0.18), through its sigmoid, back, then sRGB-encoded. Very bright values
desaturate toward white instead of clipping per channel.

**One display pass for both renderers** (`engine/render/shaders/display.slang`). Both write linear radiance (nits) to
an HDR target; the display pass applies exposure, AgX and sRGB encoding into `scene_color`, which screenshots,
goldens and presentation read. Debug views can bypass the tonemap.

## Consequences

- Scenes are lit and exposed in real-world terms: `ev100` around 15 for daylight, 8 to 10 for interiors, near 0 for
  the dim Cornell box.
- AgX is darker in the midtones than a clamp at the same exposure; scenes set their `ev100` for it.
- Comparisons between the renderers happen after the same transform; FLIP sees display-referred images, as a viewer
  would.
- A stylized look (research phase) is a grading step on top of this transform, not a replacement for physical
  exposure.

## Alternatives considered

- **ACES (fitted or full RRT+ODT):** skews saturated colors (bright blues to purple, reds to orange) and clips hues
  in highlights.
- **Khronos PBR Neutral:** keeps base colors close to authored values, but compresses strong highlights less
  gracefully.
- **Reinhard / clamping:** no real shoulder, or per-channel hue shifts.
