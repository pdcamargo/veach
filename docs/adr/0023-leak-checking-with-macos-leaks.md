# 0023: Leak checking with macOS `leaks`

Date: 2026-11-24

## Status

Accepted

## Context

The engine shuts down by RAII in reverse init order, and "no leaks" is part of the lifecycle contract
([spec](../specs/engine-lifecycle/spec.md)). ASan on Apple Clang has no LeakSanitizer, so the `asan` CI job
([ADR 0012](0012-code-quality-tooling.md)) finds memory errors but not leaks. MSVC has no leak checker we can
run in CI either.

## Decision

- Use Apple's `leaks --atExit` on macOS, through `tools/scripts/leak-check.sh <command...>`. The script fails
  (exit 1) if `leaks` reports any leak, and exit 2 if it produced no report.
- Two CTest tests run under it, registered when `VEACH_LEAK_CHECK=ON` (the `leaks` preset): the unit-test
  binary, and a headless lab hello-world run.
- CI runs a third macOS job, `macOS (Debug, leak checks)`, with `ctest --preset leaks`. It is separate from the
  ASan job because `leaks` cannot inspect sanitized processes.
- The script sends the target's output to a log file and kills the inspected process itself. On macOS 26,
  `leaks --atExit` can leave the target stopped after reporting, and a stopped process that holds CTest's output
  pipe hangs the run.

## Consequences

- A leak anywhere in init, a frame, scripts or shutdown fails CI on macOS.
- One more macOS CI job (ccache keeps it to a few minutes).
- Windows has no leak checking yet; it gets MSVC ASan first (VCH-039).
- Leaks inside GPU drivers or system frameworks could show up once tests exercise Metal. If they do, suppress
  them narrowly and document the suppression here, never disable the check.

## Alternatives considered

- **LeakSanitizer**: not available with Apple Clang.
- **Valgrind**: does not support Apple Silicon macOS.
- **Leak counters in our own allocators**: we do not own most allocations (Flecs, Luau, SDL, the STL).
