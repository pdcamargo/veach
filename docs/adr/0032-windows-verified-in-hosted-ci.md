# 0032: Windows is verified only through GitHub-hosted CI

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0030, with real-GPU verification per old ADR 0036,
now [ADR 0034](0034-pre-push-gate-no-gpu-runner.md)).

## Context

Windows (x64, DX12) is a target platform ([ADR 0003](0003-platforms-and-hardware-target.md)), but there is no
Windows machine: the dev lead works on macOS and the owner does not port or test by hand. The repo is public, so
GitHub's standard hosted runners are free, `windows-2025` included. They have no GPU, but run D3D12 on WARP.

## Decision

- **All Windows verification runs on GitHub-hosted runners** ([ADR 0033](0033-dx12-on-warp-in-ci.md)). Nothing in
  the plan or the backlog may depend on a person at a Windows machine. There is no self-hosted runner.
- **Real GPU hardware is verified on macOS only**, by the pre-push gate on the developer's Mac (ADR 0034).
- **If something cannot be verified without a Windows machine** (a real DX12 GPU, vendor drivers, a physical
  display), it goes in the backlog as **Blocked**, with what would unblock it, and work moves on.
- **CI stays cheap:**
  - one concurrency group per ref with `cancel-in-progress` (`.github/workflows/ci.yml`);
  - a `changes` job (`tools/ci/code_changed.sh`) compares `HEAD` with the last commit whose CI run passed. If only
    docs changed (`docs/`, `*.md`, `LICENSE`), the build jobs are skipped and only lint runs. Comparing against the
    last green run means a docs push that cancels a code push's run still builds that code.
- **Commits that touch Windows code stay small**, so a red Windows job points at one change.

## Consequences

- Windows code is written on macOS and proven by hosted CI one step at a time; a compile error costs a CI round
  trip, which is why each step is its own commit.
- WARP is not a GPU: driver bugs, performance and vendor differences on real Windows hardware stay unverified
  until a Windows GPU runner exists (Blocked in the backlog).
- Whatever a hosted runner's desktop session cannot show goes in the backlog as Blocked.

## Alternatives considered

- **A self-hosted Windows GPU runner on the owner's machine:** real hardware, but it needs a person to set up and
  keep running, which this policy rules out.
- **Paid hosted runners with GPUs:** worth revisiting once real-hardware coverage is needed.
- **Every job on every push:** simpler, and spends runner time on docs-only pushes.
