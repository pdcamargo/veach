# 0019: License: MIT

Date: 2026-11-24

## Status

Accepted

## Context

Veach is a public repository. We want game developers and studios to adopt and modify it without legal friction, and its dependencies must be compatible with the license we choose.

## Decision

- Veach is licensed under the **MIT License** (`LICENSE` at the repo root).
- Every dependency uses a permissive license compatible with MIT: Flecs, Luau, glm, nlohmann/json, doctest and CPM.cmake are MIT; SDL3 is zlib; Tracy is BSD-3-Clause. See [0013](0013-dependency-set.md).
- `THIRD_PARTY_NOTICES.md` reproduces each dependency's copyright and license text. Flecs, Luau and SDL3 require attribution; distributed binaries must ship this file.
- New dependencies must be MIT-compatible: permissive (MIT, BSD, zlib, Apache-2.0, Boost). Copyleft (GPL, LGPL for static linking) is not allowed in the engine.

## Consequences

- Commercial use, forks and closed-source games are all permitted.
- No patent grant, unlike Apache-2.0. That is accepted for simplicity and ecosystem norms in game tech.
- Adding a dependency includes a license check and an update to `THIRD_PARTY_NOTICES.md`.

## Alternatives considered

- **Apache-2.0.** Explicit patent grant, but longer and with more obligations (NOTICE handling). MIT matches the dependencies and the ecosystem.
- **MPL-2.0 / LGPL.** File-level or library copyleft complicates static linking and console ports later.
- **Proprietary or source-available.** Contradicts the goal of open adoption.
