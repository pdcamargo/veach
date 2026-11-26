#!/bin/sh
# The local gate before every push (AGENTS.md): build, tests (GPU goldens required on macOS), repo hygiene,
# formatting. tools/push.sh runs it and then pushes. VEACH_TESTS=fast (the default) leaves out the tests labeled
# `full` (Bistro and other heavy GPU tests, minutes); VEACH_TESTS=full runs them all.
# Use another preset with VEACH_PREPUSH_PRESET=asan; skip the GPU requirement with VEACH_REQUIRE_GPU=0.
set -eu
cd "$(git rev-parse --show-toplevel)"
preset="${VEACH_PREPUSH_PRESET:-debug}"

if [ "$(uname -s)" = "Darwin" ]; then
    export VEACH_REQUIRE_GPU="${VEACH_REQUIRE_GPU:-1}"
fi

echo "pre-push: build ($preset)"
cmake --preset "$preset" >/dev/null
cmake --build --preset "$preset"
tests="${VEACH_TESTS:-fast}"
echo "pre-push: $tests tests (VEACH_REQUIRE_GPU=${VEACH_REQUIRE_GPU:-0})"
if [ "$tests" = "full" ]; then
    ctest --preset "$preset"
else
    ctest --preset "$preset" -LE full
fi
echo "pre-push: hygiene"
python3 tools/scripts/check_repo.py
echo "pre-push: formatting"
format=clang-format
command -v "$format" >/dev/null 2>&1 || format=/opt/homebrew/opt/llvm/bin/clang-format
git ls-files '*.cpp' '*.hpp' '*.h' '*.mm' '*.inl' | xargs "$format" --dry-run --Werror
echo "pre-push: ok"
