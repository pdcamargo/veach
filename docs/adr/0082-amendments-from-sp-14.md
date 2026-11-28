# 0082: Amendments from SP-14: metering rules, fp16 clamps, occluded aerial perspective, layered far bodies

Date: 2026-11-27

## Status

Accepted. Amends [0063](0063-astronomical-rendering.md) and [0076](0076-amendments-from-sp-10.md). Validated by SP-14
(VCH-090, [findings](../research/spikes/sp-14-exposure-depth.md)).

## Context

SP-14 rendered a cockpit (0.1 to 2 m), a moon (3.84e8 m), a gas giant (7e8 m) and a second moon behind its limb
(9.5e8 m) in one frame with a 0.05 m near plane, camera-relative positions and reverse-Z float32 depth, as ordinary
rasterized meshes, at s = 1 and 0.01: no depth fighting, and the two scales match. Reverse-Z resolves about 1e-7 of the
distance everywhere (11 m at 1e8 m); a test shell over the moon starts fighting between 2.6e-7 and 7.8e-8 of the
distance. Camera-relative rendering at 1e8 m and at 1 AU is byte-identical to the origin; float32 world positions
lose the cockpit at 1e8 m and tear it in 6 cm steps at 1e6 m, which confirms [0045](0045-large-world-positions-and-reference-frames.md).
Auto-exposure with pre-exposure (`RenderView::auto_exposure`) went ground → cave → cave mouth → orbit → sun → deep
space → ground over 2,250 frames: adapted EV100 13.6, 1.2, 14.9 and −4, 0 % clipped once adapted, 0 non-finite pixels,
about 0.05 ms per frame (histogram and resolve) on the M5 Max at 1080p. A procedural starfield (`ResolvedSky::stars`)
costs 0.15 ms. Neither kill criterion was met.

## Decision

1. **Depth: no far-body pass and no depth partition.** Camera-relative reverse-Z float32 with a 0.05 m near plane
   covers cockpit to gas giant, now and in the [0060](0060-gpu-driven-visibility-buffer.md) renderer.
2. **Layered far bodies:** surfaces drawn as separate geometry must be more than 3e-7 of their distance apart; layers
   closer than that (cloud decks, rings over a limb, atmosphere shells) are composed in one impostor shader.
3. **Metering ignores pixels below the histogram floor**, so black space doesn't drag exposure down and blow out the
   planet; a **highlight term** keeps the 99th percentile unclipped, capped at 4 stops; the default exposure
   compensation is −1 stop for AgX. The adaptation floor is EV100 −4 (stars at physical brightness), a tunable default.
4. **Every pass that writes the HDR target clamps to the fp16 maximum.** Apple GPUs saturate out-of-range half writes
   by themselves; DX12 is not relied on to.
5. **Aerial perspective is occluded.** Inside the cave it filled the chamber with blue haze brighter than the walls.
   The aerial-perspective volume of 0076 carries a sky-visibility term, so caves and interiors receive none, and the
   haze returns through openings.
6. **Looking out of a cave keeps the interior readable** and lets the opening go white (a 13.4-stop frame exceeds what
   AgX shows in 8 bits); bloom, when it lands, makes the opening read as light. Pulling the highlight down without the
   4-stop cap shows the outside but crushes 88 % of the frame.
7. **The exposure cost is about 0.05 ms**, not 0.02, in the frame budget.

## Consequences

- No special-case rendering for distance: every body is a mesh or an impostor in the same pass.
- Caves and interiors need sky visibility before the real atmosphere ships.
- Starfield brightness in deep space follows the EV −4 floor; changing that default is a look decision.

## Alternatives considered

- **A logarithmic depth buffer or a separate far pass:** unneeded at measured precision, and both complicate the
  visibility buffer.
- **Average log luminance without a floor:** dominated by black space from orbit.
- **No highlight cap:** shows the cave's outside but crushes nearly the whole frame.
