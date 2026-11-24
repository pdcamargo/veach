# 0012: Code quality tooling

Date: 2026-11-24

## Status

Accepted

## Context

Most code will be written by agents, so mechanical checks matter more than review stamina. Build loops must also be fast, because agents rebuild constantly. Third-party code must not fail our builds over its warnings.

## Decision

- **Formatting.** `.clang-format` is based on Google style, with 4-space indents and a 120-column limit. CI checks formatting with a pinned clang-format. A Claude Code `PostToolUse` hook formats C/C++ files after each edit when `clang-format` is installed ([AGENTS.md](../../AGENTS.md)).
- **Static analysis.** `.clang-tidy` enables a curated subset of `bugprone-*`, `performance-*` and `modernize-*`, plus `readability-identifier-naming` encoding our naming:
  - types are `PascalCase`;
  - functions and variables are `snake_case`;
  - private members have a trailing `_`;
  - enumerators are `snake_case`, std style, so they reflect cleanly into JSON and Luau.
  `HeaderFilterRegex` restricts it to `engine/`, `apps/`, `tools/` and `tests/`.
- **Warnings as errors on Veach targets only.** `veach_apply_warnings()` in `cmake/VeachTargets.cmake` adds:
  - Clang: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wnull-dereference`, plus `-Werror`;
  - MSVC: `/W4 /permissive- /external:W0`, plus `/WX`.
  Dependencies are added `SYSTEM`, so their headers don't warn. `VEACH_WARNINGS_AS_ERRORS=OFF` is the escape hatch for local experiments, never for CI.
- **Sanitizers.**
  - The `asan` preset sets `VEACH_SANITIZE=address;undefined` with `-fno-sanitize-recover=all`, applied globally so all code links consistently.
  - macOS CI runs the full build and test suite under it.
  - MSVC has no UBSan, so Windows CI runs the plain `debug` preset.
- **Compile speed.**
  - `ccache` is used as the compiler launcher when installed (`VEACH_USE_CCACHE`). `CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime` keeps PCH builds cacheable.
  - MSVC uses embedded debug info (`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`, policy CMP0141), because ccache cannot cache `/Zi` PDB builds.
  - Precompiled headers (`VEACH_USE_PCH`) cover `<flecs.h>`, `<nlohmann/json.hpp>` and `<lua.h>`/`<lualib.h>` on the targets that include them heavily.
  - Dependencies are `EXCLUDE_FROM_ALL`, so only what we link gets built.

## Consequences

- Style debates are settled by the tools, and diffs stay minimal.
- New warnings block merges. Occasionally a warning flag will need a targeted suppression with a comment explaining why.
- Sanitizer coverage is mac-only until MSVC ASan is wired up (backlog).
- clang-tidy is slower than the build. It runs in its own CI job, not on every local build.

## Alternatives considered

- **No formatter, review-enforced style.** Wastes review time and doesn't scale to agent output.
- **Warnings as errors everywhere, including dependencies.** Breaks on every compiler update in code we don't own.
- **sccache.** Comparable, but ccache handles Clang PCH and MSVC well enough, and it is one tool on both platforms.
- **Unity builds.** Faster cold builds, but they hide include errors and hurt incremental builds, which is the agent loop.
