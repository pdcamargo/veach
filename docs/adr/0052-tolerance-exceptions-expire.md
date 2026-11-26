# 0052: Image tolerance exceptions expire with their bug

Date: 2026-11-25

## Status

Accepted. Amends [0047](0047-image-tolerance-policy.md): its baselines move to `tests/tolerance.json`.

## Context

ADR 0047 allows named exceptions to its tolerance (Sponza's blended decal, Bistro's missing bulbs under the sky),
each with a backlog item. Nothing removed an exception once its bug was fixed: the limit stayed loose, and a later
regression up to that limit would pass unnoticed. The baselines were also written in three places (the ADR, specs,
`tests/CMakeLists.txt`), which drift.

## Decision

- **One file.** `tests/tolerance.json` holds every scene's baseline (mean FLIP, mean luminance, the commit it was
  measured at), how it is checked (`gate`: the pre-push gate runs it; `benchmark`: by hand in a release build), and
  its exceptions. The gate's tests are generated from it, and `compare_direct.py --tolerance` computes each scene's
  limits from it. Specs quote the numbers; the file is the source.
- **Every exception names an open bug and says why**: `{ "metric": "luminance", "limit": 0.03, "bug": "VCH-064",
  "why": "..." }`. `tools/scripts/check_repo.py` (CI's lint job and the pre-push gate) fails when the bug is missing
  or is no longer open, that is, listed under Done in the backlog. Closing the bug therefore means removing the
  exception and recording the new baseline in the same commit.

## Consequences

- A fixed bug cannot leave a loose limit behind.
- Moving a backlog item to Done can fail the lint job: that is the point.
- ADR 0047's baseline table is history; `tests/tolerance.json` is current.

## Alternatives considered

- **Expiry dates on exceptions:** an arbitrary date fails builds for reasons unrelated to the bug, or gets pushed back.
- **Keeping the limits in CMake with a comment naming the bug:** no check can tell whether the comment is still true.
