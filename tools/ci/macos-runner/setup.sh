#!/bin/bash
# Sets up the self-hosted macOS GPU runner for Veach (docs/guides/ci-runners.md, ADR 0028, VCH-044).
#
#   sudo tools/ci/macos-runner/setup.sh            # run from the repo root, as an admin, with `gh` logged in
#
# What it does (and nothing else):
#   1. creates a hidden, non-admin user `veachci` (random password, never stored);
#   2. makes your home directory 700 so that user cannot read your files (the old mode is saved for teardown);
#   3. installs GitHub's actions runner (pinned, SHA256-checked) in /Users/veachci/actions-runner;
#   4. registers it for this repo with labels `self-hosted, macOS, ARM64, gpu-metal`, using a one-time
#      registration token fetched with *your* `gh` login (the token is not stored);
#   5. runs it as a LaunchDaemon (`dev.veach.actions-runner`) under `veachci`, started at boot.
# Undo everything with tools/ci/macos-runner/teardown.sh.
set -euo pipefail

REPO="${REPO:-pdcamargo/veach}"
RUNNER_USER="veachci"
RUNNER_HOME="/Users/${RUNNER_USER}"
RUNNER_DIR="${RUNNER_HOME}/actions-runner"
RUNNER_NAME="${RUNNER_NAME:-$(scutil --get LocalHostName)-gpu}"
RUNNER_VERSION="2.338.2"
RUNNER_SHA256="25a065af409ffa317ab4d5a3b614dd15fc9c40f2172bc1b35a1b1258e919a388"
PLIST="/Library/LaunchDaemons/dev.veach.actions-runner.plist"

if [[ $EUID -ne 0 || -z "${SUDO_USER:-}" ]]; then
    echo "Run with sudo from your own (admin) account: sudo $0" >&2
    exit 1
fi
OWNER="$SUDO_USER"
OWNER_HOME=$(dscl . -read "/Users/${OWNER}" NFSHomeDirectory | awk '{print $2}')
as_owner() { sudo -u "$OWNER" -H "$@"; }
as_runner() { sudo -u "$RUNNER_USER" -H "$@"; }

echo "==> Checking prerequisites"
command -v gh >/dev/null || { echo "gh (GitHub CLI) is required" >&2; exit 1; }
as_owner gh auth status >/dev/null 2>&1 || { echo "Log in first: gh auth login" >&2; exit 1; }
xcrun -sdk macosx -f metal >/dev/null 2>&1 || { echo "Xcode with the Metal toolchain is required" >&2; exit 1; }
for tool in cmake ninja git-lfs ccache; do
    [[ -x "/opt/homebrew/bin/${tool}" ]] || { echo "Missing /opt/homebrew/bin/${tool}: brew install ${tool}" >&2; exit 1; }
done

echo "==> Creating hidden non-admin user '${RUNNER_USER}'"
if id "$RUNNER_USER" >/dev/null 2>&1; then
    echo "    already exists"
else
    sysadminctl -addUser "$RUNNER_USER" -fullName "Veach CI runner" -password "$(openssl rand -base64 32)" \
        -home "$RUNNER_HOME" >/dev/null 2>&1
    createhomedir -c -u "$RUNNER_USER" >/dev/null
    dscl . -create "/Users/${RUNNER_USER}" IsHidden 1
fi
chmod 700 "$RUNNER_HOME"

echo "==> Making ${OWNER_HOME} private (700); every local account is in group 'staff'"
if [[ ! -f "${RUNNER_HOME}/.veach-owner-home-mode" ]]; then
    stat -f %Lp "$OWNER_HOME" > "${RUNNER_HOME}/.veach-owner-home-mode"
fi
chmod 700 "$OWNER_HOME"
if as_runner test -r "${OWNER_HOME}/."; then
    echo "    ERROR: ${RUNNER_USER} can still read ${OWNER_HOME}" >&2
    exit 1
fi
echo "    verified: ${RUNNER_USER} cannot read ${OWNER_HOME}"

echo "==> Installing actions runner ${RUNNER_VERSION}"
as_runner mkdir -p "$RUNNER_DIR"
archive="${RUNNER_HOME}/actions-runner-${RUNNER_VERSION}.tar.gz"
as_runner curl -fsSL -o "$archive" \
    "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-osx-arm64-${RUNNER_VERSION}.tar.gz"
echo "${RUNNER_SHA256}  ${archive}" | shasum -a 256 -c - >/dev/null
as_runner tar xzf "$archive" -C "$RUNNER_DIR"
rm -f "$archive"

echo "==> Registering with ${REPO} as '${RUNNER_NAME}'"
token=$(as_owner gh api -X POST "repos/${REPO}/actions/runners/registration-token" -q .token)
(cd "$RUNNER_DIR" && as_runner ./config.sh --url "https://github.com/${REPO}" --token "$token" \
    --name "$RUNNER_NAME" --labels gpu-metal --work _work --unattended --replace >/dev/null)
unset token
# The daemon's environment: tools on PATH, caches in the runner's home, GPU tests mandatory.
as_runner bash -c "echo '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin' > '${RUNNER_DIR}/.path'"
as_runner bash -c "printf 'LANG=en_US.UTF-8\nCPM_SOURCE_CACHE=${RUNNER_HOME}/.cpm-cache\nCCACHE_DIR=${RUNNER_HOME}/.ccache\nVEACH_REQUIRE_GPU=1\n' > '${RUNNER_DIR}/.env'"

echo "==> Installing LaunchDaemon ${PLIST}"
cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>dev.veach.actions-runner</string>
    <key>UserName</key><string>${RUNNER_USER}</string>
    <key>WorkingDirectory</key><string>${RUNNER_DIR}</string>
    <key>ProgramArguments</key><array><string>${RUNNER_DIR}/run.sh</string></array>
    <key>EnvironmentVariables</key><dict><key>HOME</key><string>${RUNNER_HOME}</string></dict>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ProcessType</key><string>Interactive</string>
    <key>StandardOutPath</key><string>${RUNNER_DIR}/_diag/launchd.out.log</string>
    <key>StandardErrorPath</key><string>${RUNNER_DIR}/_diag/launchd.err.log</string>
</dict>
</plist>
PLIST_EOF
chown root:wheel "$PLIST"
chmod 644 "$PLIST"
launchctl bootout system "$PLIST" >/dev/null 2>&1 || true
launchctl bootstrap system "$PLIST"

echo "==> Checking the runner is online"
for _ in $(seq 1 30); do
    status=$(as_owner gh api "repos/${REPO}/actions/runners" -q ".runners[] | select(.name==\"${RUNNER_NAME}\") | .status" || true)
    [[ "$status" == "online" ]] && break
    sleep 2
done
echo "    runner '${RUNNER_NAME}': ${status:-unknown}"
echo "Done. Trigger a run with: gh workflow run ci.yml   (see docs/guides/ci-runners.md)"
