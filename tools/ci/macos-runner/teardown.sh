#!/bin/bash
# Removes the self-hosted macOS GPU runner installed by setup.sh (docs/guides/ci-runners.md).
#   sudo tools/ci/macos-runner/teardown.sh
set -euo pipefail

REPO="${REPO:-pdcamargo/veach}"
RUNNER_USER="veachci"
RUNNER_HOME="/Users/${RUNNER_USER}"
RUNNER_DIR="${RUNNER_HOME}/actions-runner"
PLIST="/Library/LaunchDaemons/dev.veach.actions-runner.plist"

if [[ $EUID -ne 0 || -z "${SUDO_USER:-}" ]]; then
    echo "Run with sudo from your own (admin) account: sudo $0" >&2
    exit 1
fi
OWNER="$SUDO_USER"
OWNER_HOME=$(dscl . -read "/Users/${OWNER}" NFSHomeDirectory | awk '{print $2}')
as_owner() { sudo -u "$OWNER" -H "$@"; }

echo "==> Stopping the LaunchDaemon"
launchctl bootout system "$PLIST" >/dev/null 2>&1 || true
rm -f "$PLIST"

echo "==> Unregistering from ${REPO}"
if [[ -x "${RUNNER_DIR}/config.sh" ]]; then
    token=$(as_owner gh api -X POST "repos/${REPO}/actions/runners/remove-token" -q .token)
    (cd "$RUNNER_DIR" && sudo -u "$RUNNER_USER" -H ./config.sh remove --token "$token" >/dev/null) || true
    unset token
fi

echo "==> Restoring ${OWNER_HOME} permissions"
if [[ -f "${RUNNER_HOME}/.veach-owner-home-mode" ]]; then
    chmod "$(cat "${RUNNER_HOME}/.veach-owner-home-mode")" "$OWNER_HOME"
fi

echo "==> Deleting user '${RUNNER_USER}' and its home"
if id "$RUNNER_USER" >/dev/null 2>&1; then
    sysadminctl -deleteUser "$RUNNER_USER" >/dev/null 2>&1
fi
rm -rf "$RUNNER_HOME"
echo "Done. If the runner still shows on GitHub: gh api -X DELETE repos/${REPO}/actions/runners/<id>"
