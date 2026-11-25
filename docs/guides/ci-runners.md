# Self-hosted GPU runners

GitHub's hosted runners have no real GPU: the macOS ones expose a paravirtual device without Metal 3 or ray
tracing. GPU golden images therefore run on our own machines (VCH-044, [ADR 0028](../adr/0028-golden-images-across-backends.md)).
This guide sets up a runner on the Mac now, and on the Windows machine for DX12 later, under the same rules.

## Rules (both machines)

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

In that job's log, `golden: triangle (raster)` and `golden: triangle (raytraced)` must say **Passed**, not Skipped.

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

## Windows (DX12 goldens, next): the same recipe

Do these by hand in an elevated PowerShell. A script will follow once the steps are proven (VCH-047).

1. **Account.** Create a standard (non-admin) local user, e.g. `veachci`:
   `New-LocalUser veachci -NoPassword` (or set a random password), then
   `Set-LocalUser veachci -PasswordNeverExpires $true`. Do not add it to any group.
2. **No access to your files.** Your profile (`C:\Users\<you>`) already denies other standard users. Check with
   `icacls C:\Users\<you>`: only you, `SYSTEM` and `Administrators` should be listed. Don't put the runner under
   your profile.
3. **Runner.** As admin, create `C:\veachci\actions-runner`, give `veachci` full control of `C:\veachci`, and
   extract `actions-runner-win-x64-2.338.2.zip` there. Its SHA256 must be
   `a1f17aa987fc8158bc7521d284912267a0d01b7c2c4684b33c86783b5e8221cc`.
4. **Register as a service** running as that user, with the DX12 label:
   ```powershell
   $token = gh api -X POST repos/pdcamargo/veach/actions/runners/registration-token -q .token
   .\config.cmd --url https://github.com/pdcamargo/veach --token $token --name "$env:COMPUTERNAME-gpu" `
       --labels gpu-dx12 --work _work --unattended --runasservice `
       --windowslogonaccount ".\veachci" --windowslogonpassword "<its password>"
   ```
5. **Tools for the service account.** Visual Studio 2022 Build Tools with C++, CMake, Ninja and Git LFS, all
   machine-wide installs. Set `VEACH_REQUIRE_GPU=1`, `CPM_SOURCE_CACHE=C:\veachci\cpm-cache` and
   `CCACHE_DIR=C:\veachci\ccache` in `C:\veachci\actions-runner\.env`.
6. **CI job.** Copy the `gpu` job in `.github/workflows/ci.yml` as `gpu-dx12`:
   `runs-on: [self-hosted, Windows, X64, gpu-dx12]`, the same `if`, `permissions` and `VEACH_REQUIRE_GPU`, plus
   `ilammy/msvc-dev-cmd` before configuring. Register the DX12 golden tests with `-DRHI=dx12`, against the same
   shared goldens (ADR 0028).
7. **Teardown.** `.\config.cmd remove --token (gh api -X POST repos/pdcamargo/veach/actions/runners/remove-token -q .token)`,
   then remove the service if it is still listed (`sc.exe delete actions.runner.*`), then
   `Remove-LocalUser veachci` and delete `C:\veachci`.

Sleep and power behave as on the Mac: jobs queue until the machine is online, for up to 24 hours.
