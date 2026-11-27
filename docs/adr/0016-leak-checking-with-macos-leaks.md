# 0016: Leak checking with macOS `leaks`

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0023).

## Context

The engine shuts down by RAII in reverse init order, and "no leaks" is part of the lifecycle contract
([spec](../specs/engine-lifecycle/spec.md)). ASan on Apple Clang has no LeakSanitizer, so the `asan` job
([0015](0015-code-quality-tooling.md)) finds memory errors but not leaks. MSVC has no leak checker we can run in CI.

## Decision

- Use Apple's `leaks --atExit` on macOS through `tools/scripts/leak-check.sh <command...>`: exit 1 if `leaks` reports
  any leak, exit 2 if it produced no report.
- Two CTest tests run under it when `VEACH_LEAK_CHECK=ON` (the `leaks` preset): the unit-test binary and a headless
  lab hello-world run. Configuring with `VEACH_LEAK_CHECK` without macOS, or together with `VEACH_SANITIZE`, is an
  error.
- CI runs a separate macOS job with `ctest --preset leaks` (`leaks` cannot inspect sanitized processes).
- The script sends the target's output to a log file and kills the inspected process itself: on macOS 26,
  `leaks --atExit` can leave the target stopped, and a stopped process holding CTest's output pipe hangs the run.

## Consequences

- A leak anywhere in init, a frame, scripts or shutdown fails CI on macOS.
- Windows has no leak checking yet; MSVC ASan comes first.
- Leaks inside GPU drivers or system frameworks, if they appear, are suppressed narrowly and documented here, never by
  disabling the check.

## Alternatives considered

- **LeakSanitizer:** not available with Apple Clang.
- **Valgrind:** does not support Apple Silicon macOS.
- **Leak counters in our own allocators:** we do not own most allocations (Flecs, Luau, SDL, the STL).
