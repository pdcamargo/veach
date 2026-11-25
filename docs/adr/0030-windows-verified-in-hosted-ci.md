# 0030: Windows is verified only through GitHub-hosted CI

Date: 2026-11-25

## Status

Accepted. Replaces the self-hosted Windows runner planned in [ci-runners](../guides/ci-runners.md) and
[ADR 0028](0028-golden-images-across-backends.md), and the "CI runs headless with the null RHI" line of
[ADR 0002](0002-target-platforms-and-hardware.md).

## Context

Windows (x64, DX12) is a target platform ([ADR 0002](0002-target-platforms-and-hardware.md)), but there is no
Windows machine for development: the dev lead works on macOS and the product owner will not port or test by hand.
The repo is public, so GitHub's standard hosted runners are free, including `windows-2025`. They have no GPU, but
they can run D3D12 on WARP, Microsoft's software rasterizer.

## Decision

- **All Windows verification runs on GitHub-hosted runners.** Nothing in the plan or the backlog may depend on a
  person at a Windows machine. There is no self-hosted Windows runner.
- **Real GPU hardware is verified on macOS only**: the developer's Mac and the self-hosted Metal runner (VCH-044).
- **If something cannot be verified without a Windows machine** (a real DXR GPU, vendor drivers, a physical
  display), it goes in the backlog as **blocked**, with what would unblock it, and work moves on. Nobody is asked
  to do it.
- **CI stays cheap:**
  - one concurrency group per ref with `cancel-in-progress` (`.github/workflows/ci.yml`);
  - a `changes` job compares `HEAD` with the last commit whose CI run passed. If only docs changed (`docs/`,
    `*.md`, `LICENSE`), the build jobs (Windows, macOS, GPU) are skipped and only lint runs. Comparing against the
    last green run, rather than the previous commit, means a docs push that cancels a code push's run still builds
    that code.
- **Commits that touch Windows code stay small**, so a red Windows job points at one change.

## Consequences

- The DX12 backend (VCH-012) is written on macOS and proven by hosted CI jobs, one step at a time.
  Compile errors cost a CI round trip (about four minutes), which is why each step is a separate commit.
- WARP is not a GPU. Driver bugs, performance and vendor differences on real DXR hardware stay unverified until a
  Windows GPU runner exists (VCH-050, blocked).
- Windowed runs on hosted runners depend on their desktop session; whatever they cannot show goes in the backlog
  as blocked.

## Alternatives considered

- **A self-hosted Windows GPU runner on the owner's machine:** real hardware, but it needs a person to set it up
  and keep it running, which this policy rules out.
- **Larger hosted runners with GPUs:** they are paid even for public repos. Worth revisiting once real-hardware
  coverage is needed (VCH-050).
- **Run every job on every push:** simpler, but docs-only pushes would spend about ten runner minutes for nothing.
