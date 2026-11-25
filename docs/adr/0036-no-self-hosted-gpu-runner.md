# 0036: No self-hosted GPU runner; real-GPU tests are gated before push

Date: 2026-11-25

## Status

Accepted. Supersedes the self-hosted Metal runner of [ADR 0030](0030-windows-verified-in-hosted-ci.md) ("the
self-hosted Metal runner", VCH-044) and the "self-hosted RT runners (backlog)" note of
[ADR 0002](0002-target-platforms-and-hardware.md).

## Context

GitHub's hosted macOS runners have no Metal 3 or ray tracing, so Metal golden images cannot run there. VCH-044
planned a self-hosted runner on the owner's Mac (a separate non-admin user, push and manual dispatch only). It
was never registered, the CI job for it stayed queued on every push, and each summary had to flag it. The owner
decided against it: the local gate is enough.

## Decision

- **There is no self-hosted runner.** CI runs only on GitHub-hosted runners: Linux (lint, change detection),
  macOS (build, unit tests, ASan/UBSan, leaks; GPU goldens skip on its paravirtual GPU) and Windows (DX12 on
  WARP, goldens required, [ADR 0031](0031-dx12-on-warp-in-ci.md)).
- **Real Apple GPUs are verified by the pre-push gate** (`tools/scripts/pre-push.sh`, installed as the git
  hook): it builds and runs every test with `VEACH_REQUIRE_GPU=1` on macOS, so a skipped Metal golden fails the
  push. Nobody pushes past it; a push that needs to skip the hook is a bug in the hook.
- The `macOS GPU` job, the runner scripts (`tools/ci/macos-runner/`) and the runner guide are removed. VCH-044
  is closed as won't-do.

## Consequences

- No queued jobs, nothing to keep awake, no machine with a GitHub registration token.
- Metal regressions are caught on the developer's Mac before the push, not after it. A push from a machine
  without the hook installed is not gated; AGENTS.md requires the hook.
- Large local-only test assets (Sponza, Bistro) fit this model: their goldens run in the pre-push gate, and CI
  does not download them.

## Alternatives considered

- **The self-hosted runner (VCH-044):** independent of the developer's discipline, but it needs a machine kept
  awake and registered, and it was never set up.
- **A paid cloud Mac with a real GPU:** cost, and the same golden coverage the pre-push gate already gives.
