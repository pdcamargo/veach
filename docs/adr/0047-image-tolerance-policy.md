# 0047: Image accuracy is judged against recorded baselines with explicit tolerances

Date: 2026-11-25

## Status

Accepted. Replaces "no regression" (temporal.AC6) as the rule for real-time image accuracy.

## Context

The temporal milestone asked for "no FLIP regression" against the path tracer's direct-light reference. Taken
literally it fails on differences far below what anyone sees: Bistro's final image measured 0.0151 against v0's
0.0148 (FLIP, a perceptual image difference from 0 = identical; about 0.01 is where differences become visible).
Every change to a stochastic renderer moves these numbers a little. We need a rule that catches real regressions,
tolerates noise-level movement, and says which baseline it is measured against.

## Decision

**Protocol.** `tools/bench/compare_direct.py --final` (the real-time renderer's denoised image, full resolution
through pixel centers, no upscaler, 64 warm-up frames) against the path tracer's direct-light reference at the same
pixel centers (`PathTracer.mode = 1`, 1,024 samples per pixel). It reports mean FLIP and the mean luminance
difference. Upscaled images are judged separately (TAAU and MetalFX against each other, ADR 0043).

**Baselines** (as of c6a1c4b, 640x360):

| Scene | Mean FLIP baseline | Luminance | Checked |
|---|---|---|---|
| Cornell box | 0.0208 | -0.95% | pre-push gate (`tolerance: ...`) |
| FlightHelmet | 0.0018 | -0.22% | pre-push gate |
| Sponza (Intel) | 0.0119 | +5.96% (exception, VCH-060) | release benchmark (local asset) |
| Bistro (exterior) | 0.0151 | -0.13% | release benchmark |

**Tolerance.** A change passes a scene if its mean FLIP is at most the baseline plus the larger of **0.001 absolute
or 5% relative**, and its mean luminance is within **2%** of the reference (bias). Named exceptions (Sponza's
blended decal) carry a backlog item. Anything beyond the tolerance needs a reason in the commit and, if accepted,
a new baseline here: baselines move deliberately, like goldens.

**Unbiasedness stays separate** and stricter: the undenoised lighting averaged over 256 frames must match the
reference to FLIP 0.02 and luminance 2% (temporal.AC4).

## Consequences

- Bistro's 0.0151 is the baseline; the 0.0003 over v0 is the denoiser on fine foliage (VCH-062), not a failure.
- Improvements lower baselines (in this table) so later regressions are measured from the better number.
- Two scenes are in the gate (fast in a debug build); Sponza and Bistro need a release build and Bistro's LFS
  assets, so they are checked when a change touches lighting, the denoiser or the upscaler, and recorded here.

## Alternatives considered

- **Strict no-regression:** fails on noise-level movement; every rendering change would need an exception.
- **A single absolute threshold (e.g. 0.01, "invisible"):** too loose for clean scenes (FlightHelmet is 0.0018: a
  5x regression would pass) and too tight for noisy ones.
- **Relative only:** meaningless for near-zero baselines (FlightHelmet) where 5% is 0.0001.
