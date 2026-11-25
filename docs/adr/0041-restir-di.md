# 0041: Direct lighting with ReSTIR DI: alias-table candidates, pairwise MIS reuse, one shadow ray

Date: 2026-11-25

## Status

Accepted. Supersedes the "direct lighting" part of [0037](0037-realtime-renderer-visibility-buffer.md). Directional lights superseded by [0046](0046-exact-directional-lights.md).

## Context

v0 evaluates every punctual light with its own shadow ray and takes 4 emissive light samples, each with a shadow
ray. In Bistro that is 17.5 of the shading pass's 21.6 ms, and it is still noisy
([v0 results](../specs/realtime-renderer-v0/spec.md#results)). The [temporal spec](../specs/temporal-direct-lighting/spec.md)
asks for about one shadow ray per pixel, temporal and spatial reuse, and no bias against the path tracer's
direct-light reference ([0034](0034-lights-and-reference-path-tracer.md)), which the comparison harness checks.

## Decision

**One light list.** Every light is a sample source: directional, point and spot lights, and each emissive triangle.
A light sample is `(light index, two numbers)`: the two numbers place a point on a triangle (uniform barycentrics)
and are ignored by punctual lights. Indices stay stable while the lights do not change, so a sample survives a
frame.

**Power-weighted candidates from an alias table.** The CPU builds a Vose alias table over the list whenever the
lights change, weighted by the power each light sends into the scene: luminance of emitted radiance times area
times pi for a triangle, intensity times the cone's solid angle for point and spot lights, and illuminance times
the scene's bounding-sphere cross-section for directional lights. Sampling is O(1): one uniform number picks a
column, a second picks the light or its alias.

**Reservoirs.** Each pixel keeps `{sample, W, M}` (16 bytes). The target function is
`p̂(x) = luminance(Le · f · cosθ · G)`, unshadowed, evaluated with the shared BRDF (`veach/brdf.slang`) and the
pixel's surface ([0040](0040-temporal-foundation.md)).

**The passes**, all at the render resolution:

1. **Initial candidates:** `light_candidates` (default 16) alias-table samples, resampled into one reservoir by RIS.
   No rays.
2. **Temporal reuse:** the reservoir at the reprojected pixel (`veach/temporal.slang`; disoccluded pixels skip it),
   its `M` capped at 20 times the current one, combined with the current reservoir.
3. **Spatial reuse:** `spatial_samples` (default 4) neighbors within 24 render pixels whose surfaces pass the same
   plane and normal tests, combined with the pixel's own.
4. **Shading:** one shadow ray (`visible()`, which stops at the first opaque hit) toward the selected sample; the
   pass writes diffuse and specular direct light separately, demodulated, for the denoiser ([0042](0042-denoiser.md)).

**Unbiased combination: pairwise MIS.** Every combination step weighs the canonical reservoir (the pixel's own)
against each other reservoir with pairwise MIS, evaluating `p̂` of each sample in the domain it came from: the
previous frame's surfaces for temporal reuse, the neighbor's surface for spatial reuse. Weights sum to one for
every sample, so the estimate is unbiased whatever the neighbors' surfaces are. The cost is O(k) target
evaluations, not the O(k²) of the generalized balance heuristic.

**No visibility in reused reservoirs.** Reservoirs never store a visibility result: the only shadow ray is the
shading ray. That keeps the estimator unbiased (the harness checks it); reusing visibility is a known bias for
quality trade that stays off unless measurements later justify it as an option.

**Directional lights take part like any other.** A sun is one entry in the table; with temporal reuse it is
selected nearly everywhere it lights, at one ray. No separate exact path.

## Consequences

- One shadow ray per pixel for any number of lights; cost no longer scales with lights or emissive samples.
- Noise moves from per-sample to per-pixel correlation; the denoiser must handle the resulting blotches, which it
  does better than independent noise.
- Unbiasedness is a test, not a claim: the average of many undenoised frames must match the reference.
- The alias table and light records are rebuilt on light changes; moving lights reset reservoirs.

## Alternatives considered

- **A light BVH for candidates:** better for many small lights far apart (Bistro's interior); more code and a
  rebuild on change. The alias table is enough for this milestone's scenes; a BVH can replace the candidate
  source without changing the reservoirs.
- **Generalized balance heuristic:** also unbiased, O(k²) target evaluations per pixel.
- **The original ReSTIR's 1/M weights:** biased where neighbors' surfaces differ; fails the harness.
- **Keeping punctual lights exact:** one ray per light per pixel; the cost ReSTIR removes.
- **BSDF candidates (rays):** help glossy reflections of large lights, at one more ray per pixel; out of scope
  for direct light at this budget.
