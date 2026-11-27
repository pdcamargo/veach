# 0023: License: MIT

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0019).

## Context

Veach is a public repository. Game developers and studios, including people building games on the space toolbox,
should be able to adopt and modify it without legal friction, and its dependencies must be compatible with the
license we choose.

## Decision

- Veach is licensed under the **MIT License** (`LICENSE` at the repo root).
- Everything linked into Veach binaries uses a permissive license compatible with MIT: Flecs, Luau, glm (under its
  MIT option), nlohmann/json, fastgltf, meshoptimizer, doctest and CPM.cmake are MIT; SDL3 is zlib; Tracy and
  NVIDIA FLIP are BSD-3-Clause; stb and bc7enc_rdo are MIT or public domain; simdjson and simdutf are used under
  their MIT option.
- Build tools that are not linked or shipped: Slang (Apache-2.0 WITH LLVM-exception) and DXC (LLVM Release License;
  `dxil.dll` under Microsoft's terms).
- Windows runtime packages: the D3D12 Agility SDK's DLLs are redistributable under Microsoft's license terms (its
  headers are MIT). WARP is licensed for testing only and **must not ship** with Veach.
- `THIRD_PARTY_NOTICES.md` lists every dependency with its version, license and use, and reproduces each license
  text. Distributed binaries ship it. Sample assets (FlightHelmet CC0, Bistro CC BY 4.0) are listed there too.
- New dependencies must be permissive and MIT-compatible (MIT, BSD, zlib, Apache-2.0, Boost). Copyleft (GPL, LGPL
  for static linking) is not allowed in the engine or the toolbox.

## Consequences

- Commercial use, forks and closed-source games are all permitted.
- No patent grant, unlike Apache-2.0. Accepted for simplicity and ecosystem norms in game tech.
- Adding a dependency includes a license check and an update to `THIRD_PARTY_NOTICES.md`.

## Alternatives considered

- **Apache-2.0.** Explicit patent grant, but longer and with more obligations (NOTICE handling).
- **MPL-2.0 / LGPL.** File-level or library copyleft complicates static linking and console ports later.
- **Proprietary or source-available.** Contradicts the goal of open adoption and of a toolbox others extend.
