# 0021: Structured logging and errors

Date: 2026-11-24

## Status

Accepted

## Context

Logs are read by humans in a terminal and by agents and tools as data. Errors must tell the reader how to fix the problem, and must be referenceable by a stable identifier that links to documentation ([ARCHITECTURE.md](../ARCHITECTURE.md): "Errors tell you the fix").

## Decision

**Logging** (`veach/core/log.hpp`):

- Records carry a level (`trace`, `debug`, `info`, `warn`, `error`), a category (`engine`, `platform`, `asset`, `script`, `rhi`, ...), a message, an optional error code and seconds since start.
- Console output goes to stdout in one of two formats, chosen with `--log-format`:
  - `human`: `[    0.010] info  script: Hello from Luau — frame 1`, with ` [code]` appended for errors;
  - `json`: one object per line, `{"t":…,"level":"…","cat":"…","msg":"…","code":"…"}`.
- Additional sinks (`add_sink` / `remove_sink`) receive every record. Tests capture logs this way; the engine counts errors for its exit code.
- The API is `log::info("cat", "fmt {}", args)` on top of `std::format`, plus `log::error("cat", Error)`. Logging is process-wide and guarded by a mutex.

**Errors** (`veach/core/error.hpp`):

- `veach::Error { ErrorCode code; std::string message; std::string hint; }`.
- `Result<T> = std::expected<T, Error>`, built with `make_error(code, message, hint)`.
- `to_string()` renders `message — hint [slug]`.
- Failures a user or data file can cause return `Result`. Engine bugs use `VEACH_ASSERT` (debug) or `VEACH_PANIC`, which log `core.internal` and abort.

**Stable codes:**

- Codes are defined once in `engine/core/include/veach/core/error_codes.inl` as `VEACH_ERROR(enumerator, "area.name", "summary")`.
- Slugs are dotted (`script.unknown_component`, `data.unknown_field`) and are never renamed or reused.
- Every slug has a `## <slug>` section in [reference/errors.md](../reference/errors.md). `tests/core_tests.cpp` fails if one is missing or duplicated.

**Did-you-mean.** `closest_match` and `did_you_mean` (`veach/core/suggest.hpp`) use case-insensitive edit distance, with the threshold `max(2, len/3)`. They are used for flags, data fields, enum values, components, phases and asset paths, as in `unknown component 'Tranform' — Did you mean 'Transform'? [script.unknown_component]`.

**Luau** errors raised by bindings carry the same `message — hint [slug]` text, so script failures are just as actionable.

## Consequences

- Agents can parse JSON logs, filter by `code`, and look the code up in the docs.
- Adding an error means one line in `error_codes.inl` plus a docs section. The test makes that impossible to forget.
- Errors are values (`std::expected`); exceptions are not part of the engine API. Luau uses C++ exceptions internally for its errors, contained within `lua_pcall`.
- Logging is synchronous on the main thread. An async sink can come with the job system if traces show logging cost.

## Alternatives considered

- **spdlog.** See [0013](0013-dependency-set.md): it would still need custom formatting for JSON plus codes, and it pulls in fmt.
- **Numeric error codes.** Harder to remember and grep than dotted slugs. GitHub anchors work fine with slugs.
- **Exceptions for recoverable errors.** Hidden control flow, poor across the Luau and Flecs C boundaries, and inconsistent with MSVC/Clang code-size goals.
