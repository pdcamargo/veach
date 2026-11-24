# 0013: Dependency set

Date: 2026-11-24

## Status

Accepted

## Context

Each dependency costs compile time, binary size, license obligations and an upgrade burden. The boilerplate needs the minimum set that makes the engine's seams real. It should not add libraries for roadmap features before those features are built.

## Decision

All dependencies are pinned by version, URL and SHA256 in `cmake/VeachDependencies.cmake` ([0001](0001-language-and-build-system.md)).

| Dependency | Version | License | Why |
|---|---|---|---|
| Flecs | 4.2.0 | MIT | ECS, reflection, JSON, REST, modules ([0004](0004-ecs-flecs-v4.md)) |
| Luau | 0.748 | MIT | Scripting VM, compiler, `luau-analyze` ([0006](0006-scripting-luau.md)) |
| SDL3 | 3.4.20 | zlib | Windows and input, private to `engine/platform` ([0008](0008-platform-sdl3.md)) |
| glm | 1.0.3 | MIT | Math, exposed as `veach::math` ([0010](0010-conventions-and-math.md)) |
| nlohmann/json | 3.12.0 | MIT | JSON DOM for data files and schema output |
| Tracy | 0.15.0 | BSD-3-Clause | Profiling, only with `VEACH_TRACY=ON` ([0011](0011-profiling-tracy.md)) |
| doctest | 2.5.3 | MIT | Unit tests (test builds only) |
| CPM.cmake | 0.43.2 | MIT | Dependency fetching, vendored as `cmake/CPM.cmake` |

**Choices within the set:**

- **doctest** over Catch2 and GoogleTest: much lighter on compile time, a single header, and CTest discovery via `doctest_discover_tests`.
- **Our own logger** over spdlog. Structured JSON lines and stable error codes ([0021](0021-structured-logging-and-errors.md)) took about 200 lines on top of `std::format`, with no fmt dependency and no sink or formatter configuration to learn.
- **nlohmann/json** over simdjson, glaze and yyjson. We need a mutable DOM, error positions and familiarity, not parse speed; data files are small. It stays out of public headers (`json.hpp` uses `json_fwd.hpp`) to limit compile cost.
- **No CLI library.** The core flags are fixed and few, so a hand-written parser in `runtime/config.cpp` gives us did-you-mean errors for free.

**Deferred**, each a backlog item: Slang, Box3D, fastgltf, KTX2/BC7 tooling, meshoptimizer, a job system (enkiTS or Taskflow), an audio library.

**Bumping a dependency:**

1. Change the version, URL and SHA256 together (`shasum -a 256` of the tarball).
2. Build `debug` and `asan` and run all tests.
3. Update this table, `THIRD_PARTY_NOTICES.md`, and any version mentioned in docs (for example the vendored Flecs docs).
4. Commit as `build(deps): bump <name> to <version>`.

## Consequences

- A cold build compiles six libraries. ccache and `EXCLUDE_FROM_ALL` keep warm builds fast.
- Owning the logger and CLI parser means maintaining a few hundred lines. That is accepted for the control over error quality.
- Every dependency's license is permissive and compatible with MIT ([0019](0019-license-mit.md)).

## Alternatives considered

Listed per choice above. Adding Slang, Box3D or the others now was rejected: they would have no consumer yet, and their integration choices belong in the specs that use them.
