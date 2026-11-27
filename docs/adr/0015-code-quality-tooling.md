# 0015: Code quality tooling

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0012).

## Context

Most code is written by agents, so mechanical checks matter more than review stamina. Build loops must be fast,
because agents rebuild constantly. Third-party code must not fail our builds over its warnings.

## Decision

- **Formatting.** `.clang-format` is Google-based with 4-space indents and a 120-column limit. CI checks every
  `*.cpp *.hpp *.h *.mm *.inl` with a pinned clang-format (23.1.5, `--dry-run --Werror`), and the pre-push gate runs
  it too ([0034](0034-pre-push-gate-no-gpu-runner.md)). A Claude Code `PostToolUse` hook
  (`tools/scripts/format-hook.sh`) formats C/C++ files after each edit when `clang-format` is installed.
- **Static analysis.** `.clang-tidy` enables a curated subset of `bugprone-*`, `performance-*` and `modernize-*`, plus
  `readability-identifier-naming`: types `PascalCase`; functions and variables `snake_case`; private members with a
  trailing `_`; enumerators `snake_case` (so they reflect cleanly into JSON and Luau). `HeaderFilterRegex` limits it to
  `engine/`, `apps/`, `tools/` and `tests/`. It is run by hand, not on every build.
- **Warnings as errors on Veach targets only.** `veach_apply_warnings()` in `cmake/VeachTargets.cmake`:
  - Clang: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast
    -Woverloaded-virtual -Wnull-dereference -Wno-gnu-zero-variadic-macro-arguments`, plus `-Werror`;
  - MSVC: `/W4 /permissive- /external:W0 /wd4324` (C4324, padding from `alignas`, is intended for GPU parameter
    blocks), plus `/WX`.
  Dependencies are `SYSTEM`, so their headers don't warn. `VEACH_WARNINGS_AS_ERRORS=OFF` is for local experiments,
  never CI.
- **Sanitizers.** The `asan` preset sets `VEACH_SANITIZE=address;undefined` with `-fno-sanitize-recover=all`, applied
  globally. macOS CI runs the full build and tests under it; MSVC has no UBSan, so Windows CI runs `debug`. Leaks:
  [0016](0016-leak-checking-with-macos-leaks.md).
- **Compile speed.** `ccache` as the compiler launcher when installed (`VEACH_USE_CCACHE`, with
  `CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime`); MSVC embedded debug info
  (`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`, CMP0141) so ccache can cache it; precompiled headers
  (`VEACH_USE_PCH`) for `<flecs.h>`, `<nlohmann/json.hpp>` and `<lua.h>`/`<lualib.h>`; dependencies
  `EXCLUDE_FROM_ALL`.

## Consequences

- Style is settled by tools and diffs stay minimal.
- New warnings block pushes. A targeted suppression needs a comment saying why.
- Sanitizer coverage is macOS-only until MSVC ASan is wired up.

## Alternatives considered

- **Review-enforced style:** wastes review time and doesn't scale to agent output.
- **Warnings as errors in dependencies too:** breaks on every compiler update in code we don't own.
- **sccache:** comparable; ccache handles Clang PCH and MSVC well enough with one tool on both platforms.
- **Unity builds:** faster cold builds, but hide include errors and hurt the incremental builds agents live in.
