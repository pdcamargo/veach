# Conventions cheat sheet

## Space, units, time ([ADR 0010](../adr/0010-conventions-and-math.md))

We match glTF so imported assets never need conversion.

| | |
|---|---|
| Handedness | right-handed |
| Up | **+Y** (`veach::math::world_up`) |
| Forward | cameras and objects look down **−Z** (`veach::math::world_forward`); +X is right (`world_right`) |
| Transforms | `veach::Transform`: translation, rotation (quaternion, glTF `x, y, z, w`), scale, relative to the parent (`ChildOf`); `GlobalTransform` = parent's · T · R · S, written only by TransformPropagation |
| Units | meters, seconds, **radians** |
| Matrices | glm, column-major, column vectors: `v' = M * v` |
| Depth | **reverse-Z**: near → 1, far/infinity → 0, clip depth in [0, 1]. Use `veach::math::perspective_reverse_z(fov_y, aspect, near)`, never `glm::perspective` |
| Simulation | fixed **60 Hz** step (`veach::fixed_delta_seconds`); Simulation, PostSimulation and TransformPropagation run once per step |
| Math | `veach::math` is glm plus these helpers (`<veach/core/math.hpp>`); Luau `vector` ↔ `veach::math::vec3` |

## Naming

| Thing | Style | Example |
|---|---|---|
| Types | PascalCase | `AssetServer`, `Window` |
| Functions, methods, variables | snake_case (like std and Flecs) | `load_project_file`, `frame_count()` |
| Enumerators | snake_case | `WindowMode::fullscreen`, `log::Level::warn` |
| Private members | trailing underscore | `steps_` |
| Files | snake_case | `asset_server.cpp` |
| Namespaces | lowercase | `veach`, `veach::rhi` |
| Luau API and fields | snake_case | `veach.time.frame()`, `radians_per_second` |
| Phases (C++ tags and Luau strings) | PascalCase | `veach::phase::Simulation`, `"Simulation"` |
| Error codes | `area.snake_case` | `script.unknown_component` |

Reflected names are what tools, JSON and Luau see, so C++ field and enumerator names are
the public data names.

## Errors ([ADR 0021](../adr/0021-structured-logging-and-errors.md))

- Fallible functions return `veach::Result<T>` (`std::expected<T, veach::Error>`).
- `veach::Error{code, message, hint}`: `message` says what happened with the concrete values;
  `hint` says how to fix it (use `veach::did_you_mean(name, candidates)` for typos).
- Every code is listed in `error_codes.inl` and documented in [../reference/errors.md](../reference/errors.md).
- `VEACH_ASSERT` is for engine bugs only, never for things users or data can cause.

## Logging

`veach::log::info("category", "format {}", value)`; errors with `veach::log::error("category", error)`.
Categories in use: `engine`, `platform`, `asset`, `script` (also what Luau `veach.log` writes), `rhi`, `core`;
apps use their own (`lab`). `--log-format json` gives one JSON object per line
(`t`, `level`, `cat`, `msg`, `code`). An app exits 1 if anything was logged at error level.

## TODOs

Every `TODO`/`FIXME` names a backlog item: `// TODO(VCH-012): create the DX12 device`.
The item must exist in [../backlog/BACKLOG.md](../backlog/BACKLOG.md). CTest and CI reject bare TODOs.

## Determinism

- Simulation time comes from the `veach::Time` singleton (or `it.delta_time()`), never a clock.
- Randomness comes from the `veach::Random` singleton (`veach::Rng`, PCG32, seeded by `--seed`),
  never `std::rand` or `std::random_device`.
- Headless runs take exactly one fixed step per frame, so `--headless --frames N --seed S` is reproducible.
- Order matters: systems in a phase run in declaration order; script component fields are sorted by name.

## Headless first

Every app runs with `--headless --frames N` (no window, no GPU, null RHI). Agents and CI verify
work that way; if a feature cannot be checked headlessly, add something that makes it checkable.

## Apps vs engine

Outside `engine/`, include `<veach/veach.hpp>` and use `veach::` names only — no `flecs::`
(CI checks `apps/`). Gameplay never touches renderer internals or GPU handles; data reaches the
renderer only through extraction.
