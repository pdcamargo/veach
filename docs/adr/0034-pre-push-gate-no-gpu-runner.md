# 0034: No self-hosted GPU runner; real-GPU tests run in the pre-push gate

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0036), updated with `tools/push.sh` and the fast and
full test sets.

## Context

GitHub's hosted macOS runners have no Metal 3 (their GPU is paravirtual), so Metal golden images cannot run there.
A self-hosted runner on the owner's Mac was planned, never registered, and left a queued job on every push. The
owner decided against it: a local gate is enough.

## Decision

- **There is no self-hosted runner.** CI runs only on GitHub-hosted runners: Linux (lint, change detection), macOS
  (build, unit tests, ASan/UBSan, leaks; GPU goldens skip on its GPU) and Windows (DX12 on WARP, goldens required,
  [ADR 0033](0033-dx12-on-warp-in-ci.md)).
- **Real Apple GPUs are verified by the pre-push gate**, `tools/scripts/pre-push.sh`: it configures and builds the
  `debug` preset (`VEACH_PREPUSH_PRESET` overrides), runs the tests with `VEACH_REQUIRE_GPU=1` on macOS (so a
  skipped Metal golden fails), then `tools/scripts/check_repo.py` and a clang-format check of every C/C++ file.
- **Two test sets.** `VEACH_TESTS=fast` (the default) runs `ctest -LE full`, leaving out the tests labeled `full`
  (Bistro and other heavy GPU tests); `VEACH_TESTS=full` runs everything.
- **The way to push is `tools/push.sh`.** It refuses a dirty working tree (the gate checks what is committed), runs
  the gate, then `git push`, so no connection sits idle during the gate. It picks the full set when the pushed
  commits touch `engine/render/`, `engine/rhi/`, `apps/lab/scripts/`, `apps/lab/shaders/` or `tests/golden/`, or
  with `--full`. The same gate is the git `pre-push` hook (`git config core.hooksPath tools/githooks`) for a plain
  `git push`.
- Nobody pushes around the gate; a push that needs to skip it is a bug in the gate. Run the full set before any
  milestone summary.

## Consequences

- No queued jobs, nothing to keep awake, no machine holding a GitHub registration token.
- Metal regressions are caught on the developer's Mac before the push, not after it. A push from a machine without
  the hook is not gated; AGENTS.md requires it.
- Large local-only test assets (Bistro) fit this model: their goldens run in the gate's full set, and CI does not
  download them.

## Alternatives considered

- **The self-hosted runner:** independent of the developer's discipline, but needs a machine kept awake and
  registered, and was never set up.
- **A paid cloud Mac with a real GPU:** cost, for the coverage the gate already gives.
- **Always the full set:** simpler, but every push pays for the heavy tests even when rendering did not change.
