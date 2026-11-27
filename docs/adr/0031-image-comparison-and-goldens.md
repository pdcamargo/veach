# 0031: Image comparison with FLIP, and one shared set of golden images

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADRs 0027 and 0028).

## Context

Agents and humans verify visual work from screenshots, and golden-image tests must tolerate harmless differences
between GPUs, drivers and backends (edge coverage, ULP-level shading, texture filtering details) while catching
real regressions. Byte comparison fails across machines; RMSE does not match what a person notices. Two backends
(Metal and DX12) render the same scenes.

## Decision

**I/O.** PNG via stb (`stb_image`, `stb_image_write`, pinned commit). `veach::render::write_png` / `read_png` live
in the render library. Screenshots are 8-bit RGBA PNGs of the scene color.

**Comparison.** NVIDIA FLIP (BSD-3-Clause, header-only, pinned commit) behind
`veach imgdiff <reference> <test> [--max-mean F] [--max-p999 F] [--max-outliers N] [--diff out.png]`. Inputs are
decoded from sRGB to linear first. A comparison passes when all three hold:

- mean FLIP ≤ `--max-mean` (default **0.01**);
- the 99.9th-percentile pixel error ≤ `--max-p999` (default **0.25**);
- at most `--max-outliers` pixels (default **0**) have an error above 0.5 (plainly wrong to the eye).

Failures write a magma error map. FLIP is linked only into `veach-cli`.

**Goldens.**

- `tests/golden/<scene>.png`, in Git LFS ([ADR 0021](0021-git-lfs-for-binaries.md)), is the reference for **every
  backend**. They change only through `cmake --build --preset <preset> --target update-goldens`, which renders on
  Metal (the reference backend) so goldens don't flip-flop between machines. Review the images before committing.
- **Per-backend override by exception:** `tests/golden/<scene>.<backend>.png` (e.g. `.dx12.png`) replaces the shared
  golden for that backend; `tests/cmake/golden_test.cmake` picks it up. Each override needs a line in
  `tests/golden/README.md` saying which API difference makes identical output impossible. A WARP-only difference
  is not a reason.
- Limits are per test (`MAX_MEAN`, `MAX_P999`, `MAX_OUTLIERS` in `veach_golden_test`); loosening one needs a written
  reason next to it.
- `golden_test.cmake` renders headless with `--screenshot`; on Metal with shader validation off for the image,
  plus a second run with validation on that must exit 0.
- A GPU that cannot run a test (no Metal 3, unsupported DX12 device; for photo-mode goldens also no hardware ray
  tracing, [ADR 0041](0041-photo-mode.md)) logs `rhi.backend_unavailable`; the test prints `VEACH_GPU_UNAVAILABLE`
  and CTest reports it skipped, unless `VEACH_REQUIRE_GPU=1`, which makes it a failure. A skip is not a pass.

## Consequences

- One number per comparison with a perceptual meaning, plus an error map that shows where images differ.
- A backend that renders a scene differently fails the shared golden: a divergence is a bug until proven otherwise.
- Every visual change needs one regeneration and one review, not one per backend.
- Stochastic images (photo mode) depend on identical RNG streams across backends; where they still differ, the test
  gets a documented looser limit, not a second copy.

## Alternatives considered

- **Byte-exact or RMSE comparison:** brittle across GPUs, or blind to what matters.
- **SSIM:** structural, less sensitive to color shifts than FLIP, more tuning.
- **One golden set per backend:** never flaky across backends, but nothing checks that backends agree.
- **libpng / lodepng:** more code, or no gain over stb for 8-bit PNGs.
