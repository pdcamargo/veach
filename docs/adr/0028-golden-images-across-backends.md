# 0028: One shared set of golden images across backends

Date: 2026-11-25

## Status

Accepted

## Context

Golden-image tests ([ADR 0027](0027-image-io-and-comparison.md)) run on Metal today. DX12 starts next week, and
its goldens will run on a self-hosted Windows runner the same way the Mac's run (VCH-044). Two backends will
render the same scenes. Either they share one set of reference images compared with a tolerance, or each
backend keeps its own set.

What we know so far: the rasterized and ray-traced triangles, two different pipelines on the same GPU, differ by
a mean FLIP of 0.00000 (worst pixel 0.013). Differences between backends come from the same places: edge
coverage rules, float precision in interpolation, and texture filtering details. All of these are small by
construction.

## Decision

**One shared set of goldens per scene, compared with a perceptual tolerance (mean FLIP ≤ 0.01) on every backend.**

- `tests/golden/<scene>.png` is the reference for all backends. Metal regenerates it with `update-goldens`.
  Regeneration always happens on the reference backend (Metal, for now), so goldens don't flip-flop between
  machines.
- **Per-backend override, by exception:** `tests/golden/<scene>.<backend>.png` (for example `.dx12.png`) replaces
  the shared golden for that backend only. `tests/cmake/golden_test.cmake` picks it up automatically. An
  override must have a line in `tests/golden/README.md` saying why it exists (the API difference that makes
  identical output impossible) and which backlog item would remove it.
- Tolerances are per test (`MAX_MEAN`). Loosening one needs the same written reason.

## Consequences

- A backend that renders the scene differently fails the shared golden. That is the point: a divergence is a bug
  until proven otherwise, and per-backend copies would let it hide forever.
- The DX12 port gets an immediate, objective target: it is done when it passes the existing goldens unchanged.
- Some future features may legitimately differ, such as stochastic effects with backend-specific RNG or hardware
  RT traversal order with any-hit shaders. Those scenes get overrides (documented) or a looser tolerance, not a
  second copy of every golden.
- `update-goldens` stays Metal-only until DX12 exists; the Windows guide adds the `.dx12` path only if needed.

## Alternatives considered

- **One set per backend:** never flaky across backends, but nothing checks that backends agree, and every visual
  change needs N regenerations and N reviews.
- **Pixel-exact shared goldens:** fails on harmless rasterization differences between vendors and drivers.
