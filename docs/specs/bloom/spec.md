# Bloom

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

A physically based bloom in the real-time renderer: a small share of every pixel's light is spread over a wide,
screen-relative kernel (a mip chain downsampled with a 13-tap filter and upsampled with a tent, after Jimenez's Call
of Duty bloom), in pre-exposed HDR, after temporal AA and before the display transform. It models the glare of a
lens or an eye, so a cave's opening, the sun's disk, specular glints and a planet's limb from orbit read as light
instead of flat white.

## Motivation

ADR 0082 kept a cave's interior readable when looking out of it and let the opening go white: a 13-stop frame
exceeds what AgX shows in 8 bits, and "bloom, when it lands, makes the opening read as light". Today the opening is a
white hole with a hard edge, the sun's disk at sunset is a white dot, and a lit limb from orbit stops dead against
black space. Item 9 of the renderer pass. Backlog VCH-148.

## Requirements

- Bloom MUST run in the real-time renderer on pre-exposed radiance, after the temporal AA resolve and the space
  backdrop and before the display transform; the composite MUST be folded into the display pass (no full-resolution
  pass of its own).
- It MUST be threshold-free and energy-conserving: the displayed radiance is `lerp(image, blur, intensity)`, where
  `blur` is a normalized weighted average of the chain's levels, so a uniform region stays as it was and no light is
  added or lost away from the image's borders.
- The chain MUST start at half resolution with a 13-tap downsample, the first with Karis's luminance-weighted average
  over its five 2x2 groups so a single bright pixel cannot flicker the glow; upsampling MUST use a 3x3 tent.
- Its size MUST be screen-relative: the levels are weighted by their texel size as a fraction of the image height, so
  the same scene at 1080p and 1440p shows the same glow.
- Auto-exposure MUST meter the image before bloom (bloom moves no exposure). With bloom the highlight term darkens
  the metered exposure by at most 2 stops (ADR 0099 amends ADR 0082's 4).
- Values MUST stay within the fp16 maximum (ADR 0082) and finite.
- A camera MAY set `Bloom { intensity, radius }` (Scriptable; no component: the defaults); `intensity = 0` turns it
  off; `VEACH_DISABLE_BLOOM=1` turns it off everywhere (A/B comparisons).
- Golden tests MUST stay deterministic; bloom is on in them (it has no history).
- It SHOULD cost at most 0.3 ms at 1440p on the M5 Max.

## API sketch

```lua
camera:set(c.Bloom, { intensity = 0.04, radius = 0.25 })  -- default (no component): these values
camera:set(c.Bloom, { intensity = 0 })                     -- off
```

```cpp
struct Bloom {          // on a camera entity, Scriptable, visual only
    float intensity;    // the share of each pixel's light spread by the glow; 0 turns it off
    float radius;       // the widest level's texel, as a fraction of the image height
};
// RenderView::bloom: the camera's Bloom (or the defaults), intensity 0 under photo mode or VEACH_DISABLE_BLOOM=1.
render::BloomChain render::bloom_chain(std::uint32_t width, std::uint32_t height, float radius);  // levels, weights
```

```sh
VEACH_DISABLE_BLOOM=1 veach-lab ...   # every camera without bloom
```

## Acceptance criteria

- **bloom.AC1** — The chain is screen-relative: its normalized level weights, as a function of the level's texel size
  over the image height, put the same share of the glow at each screen scale at 1080p and 1440p (the weighted mean
  log2 scale within 0.1 octave, the weights sum to 1) (`tests/bloom_tests.cpp`).
- **bloom.AC2** — Threshold-free and energy-conserving: a bright cube on a uniform constant sky leaves sky pixels far
  from it unchanged (within one 8-bit level) while the sky beside it glows.
- **bloom.AC3** — The same scene at two resolutions glows the same: the brightness at the same screen-relative
  distance from a bright source matches within two 8-bit levels.
- **bloom.AC4** — The goldens render with bloom on and are byte-identical from run to run.
- **bloom.AC5** — Looking out of SP-14's cave the opening reads as light (a glow over the wall around it), the sun's
  disk at sunset and the lit limb from orbit glow, deep space's stars are not washed out, judged by eye in the
  [findings](../../research/bloom/README.md); the cost is measured there at 1080p and 1440p (0.15 and 0.21 to 0.23 ms
  on the M5 Max).

## Out of scope

- Lens dirt, anamorphic streaks, and a threshold (bloom is a share of all light, not of highlights).
- Bloom in photo mode (the path-traced reference stays the scene without camera glare).
- Local tone mapping.

## Open questions

None.

## Links

- ADRs: [0099](../../adr/0099-bloom.md), [0082](../../adr/0082-amendments-from-sp-14.md), [0097](../../adr/0097-temporal-aa-as-built.md),
  [0091](../../adr/0091-space-backdrop.md), [0037](../../adr/0037-exposure-and-agx.md)
- Backlog: VCH-148
- Guides: [rendering](../../guides/rendering.md)
- Research: [findings](../../research/bloom/README.md), [SP-14](../../research/spikes/sp-14-exposure-depth.md)
