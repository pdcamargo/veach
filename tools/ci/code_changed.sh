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
    # A run counts by its hosted jobs alone: its queued GPU job keeps the run itself "in progress" for hours.
    # Never the run asking (its lint may already be green while its builds are not even listed yet), and only
    # runs that list their Windows job (or, when it was skipped, the unexpanded matrix).
    for run in $(gh run list --workflow ci.yml --branch main --event push --limit 30 \
        --json databaseId -q '.[].databaseId'); do
        if [ "$run" = "${GITHUB_RUN_ID:-}" ]; then
            continue
        fi
        green=$(gh run view "$run" --json jobs -q \
            '[.jobs[] | select(.name | test("GPU|Detect") | not)] as $jobs |
             ($jobs | any(.name | test("Windows|matrix"))) and
             ($jobs | all(.status == "completed" and (.conclusion == "success" or .conclusion == "skipped")))')
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
