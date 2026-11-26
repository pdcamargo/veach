#!/bin/sh
# Runs every repro: compiles its kernel.metal like Veach's build and creates the pipeline. Exit 0 if all crash
# (still reproducing), 1 if any pipeline was created (the bug may be fixed: see README.md).
cd "$(dirname "$0")"
fixed=0
for dir in */; do
    dir=${dir%/}
    function=$(cat "$dir/FUNCTION")
    echo "== $dir ($function)"
    if ./build.sh "$dir" "$function" 2>/dev/null | grep -v warning | tail -1 | tee /dev/stderr | grep -q "created"; then
        fixed=1
    fi
    rm -f "$dir/kernel.air" "$dir/kernel.metallib"
done
exit $fixed
