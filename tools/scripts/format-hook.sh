#!/bin/sh
# Claude Code PostToolUse hook: clang-format the C++ file that was just edited.
# Cheap, silent, and never fails the edit (exit 0 always). See .claude/settings.json.

input=$(cat)
file=$(printf '%s' "$input" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)

case "$file" in
    *.cpp | *.hpp | *.h | *.mm | *.inl) ;;
    *) exit 0 ;;
esac
case "$file" in
    */.cpm-cache/* | */build/*) exit 0 ;;
esac

for candidate in clang-format /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format; do
    if command -v "$candidate" >/dev/null 2>&1; then
        "$candidate" -i "$file" >/dev/null 2>&1 || true
        exit 0
    fi
done
exit 0
