#!/bin/sh
# Runs a command under macOS `leaks --atExit` and fails if anything leaked.
#   tools/scripts/leak-check.sh build/debug/tests/veach-tests
# The target's output goes to a log file, not to our stdout: `leaks --atExit`
# can leave the inspected process stopped after reporting, and a stopped
# process holding CTest's pipe would hang the test run. We kill it ourselves.

log=$(mktemp -t veach-leaks)
MallocStackLogging=lite leaks --atExit -- "$@" >"$log" 2>&1
status=$?

pid=$(sed -n 's/^Process \([0-9][0-9]*\): .* leaks for .*/\1/p' "$log" | head -n 1)
if [ -n "$pid" ]; then
    kill -9 "$pid" 2>/dev/null
fi

grep -v "MallocStackLogging" "$log" | sed -n '/leaks Report Version/,$p'
if ! grep -q "leaks for" "$log"; then
    echo "leak-check: leaks produced no report (exit $status). Full output:"
    cat "$log"
    rm -f "$log"
    exit 2
fi
rm -f "$log"
# leaks exits 1 when it found leaks, 0 otherwise.
exit $status
