# 0099: Bloom: threshold-free and screen-relative, between the backdrop and the display; 2 stops of highlight pull

Date: 2026-11-28

## Status

Accepted. Amends [0082](0082-amendments-from-sp-14.md) (rule 3's 4-stop cap on the highlight term becomes 2, rule
6's bloom lands). Implemented by VCH-148 ([spec](../specs/bloom/spec.md), [findings](../research/bloom/README.md)).

## Context

ADR 0082 kept a cave's interior readable when looking out of it and let the opening go white, "and bloom, when it
lands, makes the opening read as light". The sun's disk at sunset was a white dot, a lit limb stopped dead against
space. Bloom had to fit the post chain as built: temporal AA on pre-exposed radiance ([0097](0097-temporal-aa-as-built.md)),
then the space backdrop with its own exposure ([0091](0091-space-backdrop.md)), metering and AgX
([0037](0037-exposure-and-agx.md)); look the same at 1080p and 1440p; not flicker under TAA; not wash out the
starfield; and cost at most 0.3 ms at 1440p on the M5 Max.

## Decision

1. **Physically based, threshold-free and energy-conserving.** The displayed radiance is `lerp(image, blur,
   intensity)` with `blur` a normalized weighted average of the chain's levels: what the glow spreads it takes from
   the pixel, a uniform region is unchanged, and only light far brighter than its surroundings shows a glow. No
   threshold, so nothing pops in as a highlight crosses one. The default intensity, **0.04**, is the order of real
   veiling glare (a camera lens scatters 1 to 3 % of its light; the eye's Stiles-Holladay glare, 10 E / θ², puts about
   6 % between 1 and 30 degrees) and was picked by eye over 0.01 to 0.04 on the cave, the sun and Bistro.
2. **The chain** (`shaders/bloom.slang`, `bloom.cpp`): after Jimenez (Call of Duty, 2014). A 13-tap downsample (five
   overlapping 2 x 2 boxes, 13 bilinear taps) from the image into level 0 at half resolution, each box weighted by
   Karis's 1 / (1 + luma) there so a lone bright pixel cannot make the glow flicker, then plain 13-tap downsamples
   level to level; upsampling, coarsest first, `lerp(tent(coarser), level, blend)` with a 3 x 3 tent one coarser
   texel wide and `blend = w_k / (w_k + w_k+1 + ...)`, which leaves level 0 as the normalized weighted average. The
   display pass reads level 0 through four bilinear taps half a texel apart and blends it in before AgX. Non-finite
   input reads as 0 (metering still counts it); every value is an average, so fp16 never overflows.
3. **Screen-relative.** `render::bloom_chain(width, height, radius)` weighs each level by its texel's size as a
   fraction of the image height: 1 from 2 / 1080 (1080p's level 0) to `radius`, fading over an octave either side,
   which with one level per octave is equal energy per octave, a 1 / r² glow like the eye's. An image under 1080 rows
   lacks the finest levels; `coverage` scales the intensity so the coarser levels keep their 1080p share. The default
   radius is **0.12** of the height (0.25 washed the cave's walls grey at 0.04).
4. **Where it runs.** After temporal AA and the space backdrop, on the image as it will be shown: the TAA-resolved
   input is already stable, and the backdrop **participates**: its stars lose 4 % of their light into a glow too
   wide to see (deep space changes by a mean FLIP of 0.018, stars intact by eye; the brightest get a faint halo on
   top of their own glare), and a lit planet veils the stars near its limb, as glare does. Excluding it would need a
   second composite for no visible gain. **Metering reads the image without bloom** (it moves no exposure).
5. **Exposure.** The −1 stop compensation stays: bloom conserves energy, so the metered band and mid grey do not
   move. The highlight term's cap drops from 4 to **2 stops** (`AutoExposureSettings::highlight_max_pull`): with the
   opening reading as light through its glow, the last two stops of pull only darkened the interior. It binds only in
   high-contrast frames: of the slice's 22 tour views only the sunset changes (mean FLIP 0.004); the cave mouth's
   interior is two stops brighter.
6. **Creator settings:** `Bloom { intensity = 0.04, radius = 0.12 }` on a camera, Scriptable; no component means
   the defaults; `intensity = 0` turns it off; `VEACH_DISABLE_BLOOM=1` turns it off everywhere. Photo mode never
   blooms (the path-traced reference stays without camera glare).
7. **Goldens render with bloom:** it has no history, and the regenerated goldens are byte-identical from run to run.

Measured on the M5 Max (release, render-graph timestamps, the minimum over rounds on a GPU shared with other work,
which matches TAA's clean 0.42 ms): the chain 0.14 ms at 1080p and 0.20 to 0.22 ms at 1440p (level 0's downsample
0.08, its upsample 0.03, each small level about 0.004), and 0.01 ms more in the display pass.

## Consequences

- A cave's opening, the sun's disk, glints and a bright limb read as light; scenes without strong contrast change
  little (Bistro at noon: mean FLIP 0.026 against no bloom).
- Every real-time golden changed once (a mean FLIP of 0.008 to 0.044).
- Fifteen passes at 1440p; the ten on levels under 1/8 resolution cost about 0.04 ms, mostly per-pass overhead.
  Folding them into one dispatch is the next saving if a preset needs it.
- The interior seen from a cave is two stops brighter by default; a game that wants the old look sets the pull.
- Revisit if: lens effects (dirt, streaks) are wanted, photo mode needs glare to match, or the M1 tier cannot afford
  0.2 ms (fewer levels, a smaller radius).

## Alternatives considered

- **A threshold (or a soft knee):** not physical, pops as highlights cross it, and depends on the exposure.
- **A fixed mip count, or a fixed-size first level (Bevy's 512):** a fixed count changes the glow's size with the
  resolution; a fixed-size level 0 is a non-integer first downsample that skips source texels and flickers.
- **The backdrop excluded** (weighting the first downsample by its share of the pixel): no visible difference in
  deep space, and a planet's glare should veil the stars behind it.
- **Keeping the 4-stop pull:** the cave's interior stays crushed next to a glowing opening; no pull at all floods the
  frame with glow.
- **A 9-tap tent in the display pass:** no visible difference, and more than twice the taps over every pixel.
