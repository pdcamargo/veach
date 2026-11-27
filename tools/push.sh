#!/bin/sh
# The way to push (AGENTS.md): the local gate first, then `git push`, so no connection to GitHub sits idle while the
# gate runs. The gate checks the commits as committed, so the working tree must be clean.
#
#   tools/push.sh            the fast test set, or the full one when the pushed commits touch rendering code
#   tools/push.sh --full     the full test set (Bistro and other heavy GPU tests)
#
# Any other arguments go to git push.
set -eu
cd "$(git rev-parse --show-toplevel)"
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "push: commit or stash your changes first: the gate checks what is pushed" >&2
    exit 1
fi
tests=fast
if [ "${1:-}" = "--full" ]; then
    tests=full
    shift
fi
upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || echo origin/main)
if git diff --name-only "$upstream"..HEAD |
    grep -qE '^(engine/render/|engine/rhi/|apps/lab/scripts/|apps/lab/shaders/|tests/golden/)'; then
    tests=full  # rendering changed: the full set, Bistro included
fi
VEACH_TESTS=$tests sh tools/scripts/pre-push.sh
VEACH_GATE_PASSED=$(git rev-parse HEAD) git push "$@"
