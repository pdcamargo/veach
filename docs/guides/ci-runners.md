# Self-hosted GPU runners

GitHub's hosted runners have no real GPU: the macOS ones expose a paravirtual device without Metal 3 or ray
tracing. Metal golden images therefore run on a self-hosted Mac (VCH-044, [ADR 0028](../adr/0028-golden-images-across-backends.md)).
This guide sets it up. Windows has no self-hosted runner (see the end of this guide).

## Rules

| Rule | How it is enforced |
|---|---|
| Runs under a **separate, non-admin account**, with no access to the owner's files | `veachci` user; the owner's home is made private (`700`); setup verifies `veachci` cannot read it |
| **No secrets** | The CI job uses none. The workflow token is read-only (`permissions: contents: read`; repo default is read). The registration token is fetched with the owner's `gh` login at setup and never stored |
| **Only pushes to `main` and manual dispatch**, never `pull_request` | `if: github.event_name == 'push' \|\| github.event_name == 'workflow_dispatch'` on the `gpu` job, plus the repo setting "require approval for **all** external contributors" (set 2026-11-25), so a fork cannot run an edited workflow on the runner without your approval |
| **Queued jobs wait** while the machine sleeps or is off | Self-hosted jobs stay queued until a runner with the labels comes online (GitHub keeps a queued job for up to **24 hours**, then fails it). A newer push to `main` cancels older queued runs (`concurrency`) |
| A skipped GPU test is a failure | The job sets `VEACH_REQUIRE_GPU=1` |

Since we push straight to `main`, the runner cannot block a push; it turns `main` red within minutes of the Mac
waking. The gate *before* a push is the pre-push hook (below), which runs the same GPU goldens locally.

## macOS (Apple Silicon): setup

Prerequisites on the machine: Xcode (with the Metal toolchain), Homebrew `cmake ninja git-lfs ccache`, and the
GitHub CLI logged in as a repo admin (`gh auth status`).

```sh
sudo tools/ci/macos-runner/setup.sh
```

The script (read it first; it is short) does exactly this:

1. creates a hidden, non-admin user `veachci` with a random password nobody knows;
2. `chmod 700` on your home directory, remembering the old mode, and verifies `veachci` cannot read it. This is
   needed because every local account on macOS is in the `staff` group, and macOS homes default to `750`;
3. downloads GitHub's runner v2.338.2 (SHA256-checked) into `/Users/veachci/actions-runner`;
4. registers it with labels `self-hosted, macOS, ARM64, gpu-metal` using a one-time token;
5. installs `/Library/LaunchDaemons/dev.veach.actions-runner.plist`, which runs the runner as `veachci` at boot,
   with no login session needed.

Caches (`CPM_SOURCE_CACHE`, `CCACHE_DIR`) live in `/Users/veachci`. The environment comes from
`/Users/veachci/actions-runner/.env` and `.path`.

### Verify

```sh
gh api repos/pdcamargo/veach/actions/runners -q '.runners[] | "\(.name) \(.status) \(.labels | map(.name) | join(","))"'
gh workflow run ci.yml --ref main             # manual dispatch; or push to main
gh run watch                                  # the "macOS GPU (Metal goldens, self-hosted)" job must pass
sudo launchctl print system/dev.veach.actions-runner | head -20   # daemon state
tail -f /Users/veachci/actions-runner/_diag/launchd.out.log       # needs sudo
```

In that job's log, `golden: background`, `golden: triangle (raster)` and `golden: triangle (raytraced)` must say
**Passed**, not Skipped.

### Teardown

```sh
sudo tools/ci/macos-runner/teardown.sh
```

It stops and removes the daemon, unregisters the runner from GitHub, restores your home directory's previous
mode, and deletes the `veachci` user and its home.

## Pre-push gate (every developer machine)

```sh
git config core.hooksPath tools/githooks      # once per clone
```

From then on, `git push` runs `tools/scripts/pre-push.sh`: build (`debug`), every test with `VEACH_REQUIRE_GPU=1`
on macOS (GPU goldens must pass, not skip), repo hygiene and clang-format. Run it by hand the same way. For
another preset use `VEACH_PREPUSH_PRESET=asan`. On a Mac without a suitable GPU, use `VEACH_REQUIRE_GPU=0`.

## Windows: hosted runners only

There is no self-hosted Windows runner ([ADR 0030](../adr/0030-windows-verified-in-hosted-ci.md)). Windows,
DX12 included, is verified on GitHub's hosted `windows-2025` runners. Real DXR hardware is blocked until a Windows
GPU runner exists (VCH-050).
