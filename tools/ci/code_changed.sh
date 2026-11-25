#!/usr/bin/env bash
# Prints "code=true" if anything other than docs changed since the last commit on main whose hosted CI jobs
# passed, else "code=false" (ADR 0030). Docs are docs/, *.md and LICENSE. The self-hosted GPU job is ignored
# when looking for that commit: it may sit queued for hours. Any doubt (no green run, history not fetched,
# manual dispatch) means code=true.
#
#   tools/ci/code_changed.sh [<event>] [<pull request base sha>]      # needs gh (GH_TOKEN) and full history
set -euo pipefail

event="${1:-push}"
base="${2:-}"

if [ "$event" = workflow_dispatch ]; then
    echo "code=true"
    exit 0
fi

if [ "$event" != pull_request ]; then
    for run in $(gh run list --workflow ci.yml --branch main --event push --status completed --limit 30 \
        --json databaseId -q '.[].databaseId'); do
        green=$(gh run view "$run" --json jobs -q \
            '[.jobs[] | select(.name | test("GPU|Detect") | not) | .conclusion] | length > 0 and all(. == "success" or . == "skipped")')
        if [ "$green" = true ]; then
            base=$(gh run view "$run" --json headSha -q .headSha)
            break
        fi
    done
fi

if [ -z "$base" ] || ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
    echo "code=true"
    exit 0
fi

if git diff --name-only "$base" HEAD | grep -qvE '^docs/|\.md$|^LICENSE'; then
    echo "code=true"
else
    echo "code=false"
fi
echo "compared with ${base}" >&2
