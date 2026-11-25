# 0046: Directional lights are evaluated exactly, beside ReSTIR DI

Date: 2026-11-25

## Status

Accepted. Supersedes "Directional lights take part like any other" in [0041](0041-restir-di.md).

## Context

ReSTIR DI traced one shadow ray per pixel for every light, the sun included ([0041](0041-restir-di.md)). Its final
image was 0.0048 FLIP from the path tracer's direct light on FlightHelmet, where v0, tracing one ray per punctual
light, had 0.0017 ([temporal spec results](../specs/temporal-direct-lighting/spec.md#results)): the one ray samples
a scene's few strong lights instead of evaluating them. The owner chose accuracy over the one-ray budget for
directional lights (VCH-061).

## Decision

- **Directional lights leave ReSTIR's alias table** (weight 0) and the shade pass evaluates each one exactly, with its
  own shadow ray. A directional light with an angle samples its disk with that ray ([soft-shadows spec](../specs/soft-shadows/spec.md)).
  Every other light (point, spot, emissive triangles) stays in ReSTIR, with its one ray.
- **Candidates read one record each.** With the sun out of the table, every candidate in a sunlit scene was an
  emissive triangle, and evaluating its material cost 4.3 ms in Bistro. `EmissiveTriangle` now carries its world
  corners, emissive weight and double-sidedness; RIS ranks candidates with the material's emissive weight as the
  radiance (nonzero wherever the triangle emits, so the reservoir's weight stays unbiased), and only the kept
  candidate's real emission is evaluated.
- **Defaults: 12 candidates, 2 spatial neighbors** (were 16 and 4), to pay for the sun's ray within 16.6 ms in
  Bistro: 15.75 ms median, 16.32 ms p95 at 1440p. 8 candidates were faster but lost the ghosting criterion.

## Consequences

- FlightHelmet's final image: 0.0048 -> 0.0018 FLIP (v0: 0.0017); its accumulated lighting matches v0 exactly.
- Sunlit scenes trace two shadow rays per pixel: ReSTIR's and the sun's (temporal.AC5 counts ReSTIR's).
- Several directional lights cost one ray each; scenes have one sun in practice.

## Alternatives considered

- **Keeping the sun in ReSTIR (one ray):** misses the accuracy requirement on punctual-lit scenes.
- **Evaluating every punctual light exactly (v0):** one ray per light, unbounded with light count.
