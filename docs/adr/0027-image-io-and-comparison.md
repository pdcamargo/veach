# 0027: Image I/O and perceptual comparison

Date: 2026-11-24

## Status

Accepted

## Context

First light ([spec](../specs/first-light/spec.md)) needs screenshots written as files agents and humans can open,
and golden-image tests that tolerate harmless differences between GPUs and drivers (edge rasterization, ULP-level
shading changes) but catch real regressions. Exact byte comparison fails across machines; RMSE-style metrics
don't match what a person notices.

## Decision

- **PNG via stb** (`stb_image`, `stb_image_write`, pinned commit). `veach::render::write_png` and `read_png` live
  in the render library. Screenshots are 8-bit RGBA PNGs of the scene color.
- **NVIDIA FLIP** (BSD-3-Clause, header-only `FLIP.h`, pinned commit) for comparisons, behind
  `veach imgdiff <reference> <test> [--max-mean F] [--diff out.png]`. Inputs are decoded from sRGB to linear
  before evaluation. A test passes when the mean FLIP error is ≤ `--max-mean` (0.01 for goldens). Failures write
  a magma error map.
- Goldens live in `tests/golden/` under Git LFS ([ADR 0017](0017-git-lfs-for-binaries.md)) and change only via
  `cmake --build --preset <preset> --target update-goldens`.

## Consequences

- One number per comparison with a perceptual meaning, plus an error map that shows where the images differ.
- FLIP is linked only into `veach-cli`; apps carry stb's PNG code (small).
- Thresholds may need tuning once goldens are compared across different Apple GPUs (VCH-044).

## Alternatives considered

- **Byte-exact or RMSE comparison**: brittle across GPUs, or blind to what matters.
- **SSIM**: structural, but less sensitive to color shifts than FLIP and needs more tuning.
- **libpng / lodepng**: more code (libpng) or no meaningful gain (lodepng) over stb for 8-bit PNGs.
