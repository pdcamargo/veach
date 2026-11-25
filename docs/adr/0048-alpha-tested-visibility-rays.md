# 0048: Visibility rays test alpha-tested triangles as candidates in one traversal

Date: 2026-11-25

## Status

Accepted. Supersedes how [0035](0035-alpha-by-continuing-opaque-rays.md) treats visibility rays (closest-hit rays
keep 0035's continuation).

## Context

After the exact sun ([0046](0046-exact-directional-lights.md)), Bistro's shade pass took 5.36 ms at 1440p output:
4.87 ms of shadow rays, 3.16 ms of it alpha testing (measured by turning coverage tests off,
`frame_times.py --breakdown`). Visibility rays found opaque occluders with one first-hit query, then traced the
alpha-tested instances closest-hit first and, at each transparent texel, started a new traversal just past it (up
to 16): through a tree's foliage that is many full traversals. ADR 0035 chose continuation because committing
non-opaque candidates "varied run to run on Metal"; that variation was Metal's shader validation, which varies
these ray queries whatever the approach (found later; goldens now render without it).

Options considered for the cost:

| Approach | Backends | Shadows equal the path tracer's? | Measured |
|---|---|---|---|
| Candidates in one traversal (this ADR) | Metal, DX12 | yes: same coverage test | Bistro shade 5.36 -> 4.04 ms |
| Opacity micromaps (DXR 1.2, SM 6.9) | DX12 only: no micromap API in the macOS 26 SDK's Metal | yes, if built from the same textures | not measurable: WARP is software, no DX12 GPU (VCH-050) |
| Opaque proxies or coarse alpha for shadow rays | both | no: different shapes, the path tracer would need the same proxies | not built |

Our pinned Agility SDK (1.620.2) and WARP (1.0.22) are recent enough for micromaps, so the option stays open on DX12.

## Decision

- Alpha-tested instances are **non-opaque** in the TLAS (`TlasInstance::opaque = false`: Metal's `NonOpaque`
  option; D3D12's `FORCE_NON_OPAQUE`, which the DX12 backend now sets for it).
- `visible()` traces opaque instances first (first hit ends it), then alpha-tested instances in **one traversal**
  with `ACCEPT_FIRST_HIT_AND_END_SEARCH`: each non-opaque candidate runs `covered()`, the same test as before, and
  a covered texel is committed and ends the search.
- Closest-hit rays (`trace()`: camera and bounce rays in the path tracer) force opacity and keep 0035's continuation.
- Both renderers share `visible()`, so their shadows stay identical by construction.

## Consequences

- Bistro: shade 5.36 -> 4.04 ms, frame 15.75 -> 14.62 ms at 1440p (M5 Max); Sponza unchanged (little foliage).
- Every image and comparison is unchanged within tolerance ([0047](0047-image-tolerance-policy.md): Bistro 0.0150,
  baseline 0.0151; unbiased 0.0148).
- Blended materials' stochastic coverage now draws its random numbers in traversal order: still unbiased.
- Micromaps remain a DX12-only optimization to measure on real hardware (VCH-063).

## Alternatives considered

See the table: micromaps leave Metal, our primary platform, without a gain; proxies and coarse alpha change the
shadows the comparison harness checks against the path tracer.
